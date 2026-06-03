/*
 * dump_font.c -- host-side OMF font extractor (CHARSMAL.DAT / GRAPHCHR.DAT
 * -> 95 individual PNG glyphs, white-on-transparent, ready for 3it CEL
 * conversion).
 *
 * Subcommand:
 *   dump <FONT.DAT> <font_h> <out_dir>
 *       font_h = 6 for CHARSMAL.DAT, 8 for GRAPHCHR.DAT.
 *       Writes out_dir/g32.png .. out_dir/g126.png (95 printable ASCII
 *       glyphs) and out_dir/manifest.txt describing the layout.
 *
 * The PNG is padded to a CEL-friendly cell size of 8x8 (small) or 8x8
 * (large) -- the actual glyph occupies the top-left font_h x font_h
 * region; remaining pixels are fully transparent. The 3DO runtime treats
 * the glyph pitch as font_h pixels and lets the extra cells overdraw
 * harmlessly.
 *
 * Linked against the same openomf-master/src/ subset as dump_har -- see
 * [[project-host-tools-strategy]] in /memory. openomf-master stays
 * untouched per [[feedback-isolated-project]].
 */

#include "formats/error.h"
#include "formats/fonts.h"
#include "utils/path.h"

#include <png.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* First printable ASCII glyph emitted; last is FIRST + COUNT - 1. */
#define GLYPH_FIRST   32
#define GLYPH_COUNT   95
/* CEL cell pitch: padded up so 3it has a friendly width/height alignment.
 * For the small (6x6) font we still use 8x8 cells; runtime steps by 6. */
#define CELL_PAD      8

static int ensure_dir(const char *p) {
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(p, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    fprintf(stderr, "mkdir(%s) failed: %s\n", p, strerror(errno));
    return -1;
}

/* Write an RGBA buffer (w*h*4 bytes) as a PNG. Mirrors dump_har.c's
 * write_png_rgba so we stay consistent across host tools. */
static int write_png_rgba(const char *filename, const unsigned char *rgba,
                          int w, int h) {
    FILE *fp;
    png_structp png;
    png_infop info;
    png_bytep *rows = NULL;
    int y;

    fp = fopen(filename, "wb");
    if (!fp) { fprintf(stderr, "fopen(%s, wb) failed\n", filename); return -1; }
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(rows);
        fclose(fp);
        return -1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    rows = malloc(sizeof(png_bytep) * h);
    for (y = 0; y < h; y++) {
        rows[y] = (png_bytep)(rgba + y * w * 4);
    }
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(rows);
    fclose(fp);
    return 0;
}

/* Decode glyph `ch_idx` (0..223, sd_font indexing) into a CELL_PAD x
 * CELL_PAD RGBA buffer. Top-left font_h x font_h holds the glyph
 * (white-on-transparent); padding rows/cols are fully transparent.
 *
 * Bit layout (from formats/fonts.c sd_font_decode):
 *   for row i in 0..font_h-1:
 *     for col c in 0..font_h-1:
 *       bit k = (font_h - 1) - c     -- column 0 = MSB-ish at bit (font_h-1)
 *       if (data[i] & (1 << k)) -> opaque white at (c, i)
 */
static void render_glyph_rgba(const sd_font *font, int ch_idx,
                              unsigned char *out_rgba) {
    int i, c, pix_off;
    unsigned char b;

    memset(out_rgba, 0, CELL_PAD * CELL_PAD * 4);
    if (ch_idx < 0 || ch_idx >= 224) return;

    for (i = 0; i < (int)font->h; i++) {
        b = (unsigned char)font->chars[ch_idx].data[i];
        for (c = 0; c < (int)font->h; c++) {
            int k = (int)font->h - 1 - c;
            if (b & (1 << k)) {
                pix_off = (i * CELL_PAD + c) * 4;
                out_rgba[pix_off + 0] = 255;
                out_rgba[pix_off + 1] = 255;
                out_rgba[pix_off + 2] = 255;
                out_rgba[pix_off + 3] = 255;
            }
        }
    }
}

static int cmd_dump(const char *font_filename, int font_h, const char *out_dir) {
    sd_font font;
    path p;
    int rc, g, ch_idx;
    unsigned char *rgba;
    char png_path[512];
    char manifest_path[512];
    FILE *mf;
    int emitted = 0;
    int blank = 0;

    if (font_h != 6 && font_h != 8) {
        fprintf(stderr, "font_h must be 6 (CHARSMAL) or 8 (GRAPHCHR); got %d\n",
                font_h);
        return 1;
    }

    path_from_c(&p, font_filename);
    if (sd_font_create(&font) != SD_SUCCESS) {
        fprintf(stderr, "sd_font_create failed\n");
        return 1;
    }
    rc = sd_font_load(&font, &p, (unsigned int)font_h);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_font_load(%s, h=%d) failed: %d (%s)\n",
                font_filename, font_h, rc, sd_get_error(rc));
        sd_font_free(&font);
        return 1;
    }

    if (ensure_dir(out_dir) != 0) {
        sd_font_free(&font);
        return 1;
    }

    rgba = malloc(CELL_PAD * CELL_PAD * 4);
    if (!rgba) {
        fprintf(stderr, "malloc failed\n");
        sd_font_free(&font);
        return 1;
    }

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", out_dir);
    mf = fopen(manifest_path, "wb");
    if (!mf) {
        fprintf(stderr, "fopen(%s, wb) failed: %s\n", manifest_path,
                strerror(errno));
        free(rgba);
        sd_font_free(&font);
        return 1;
    }

    fprintf(mf, "# OMF font: %s (h=%d)\n", font_filename, font_h);
    fprintf(mf, "# glyph_first=%d glyph_count=%d\n", GLYPH_FIRST, GLYPH_COUNT);
    fprintf(mf, "# cell_pad=%d glyph_w=%d glyph_h=%d\n",
            CELL_PAD, font_h, font_h);
    fprintf(mf, "# columns: ascii char_idx blank png_w png_h glyph_w glyph_h file\n");

    /* Iterate the printable ASCII range. sd_font indexes chars by
     * (ASCII - 32), so glyph for ASCII 32 = font.chars[0]. */
    for (g = 0; g < GLYPH_COUNT; g++) {
        int ascii = GLYPH_FIRST + g;
        ch_idx = g;        /* sd_font's char[0] = ASCII 32 */
        render_glyph_rgba(&font, ch_idx, rgba);

        /* Tag blank glyphs in the manifest so the pipeline can skip them
         * if convenient (space + some control-ish slots). */
        {
            int any = 0, p2;
            for (p2 = 0; p2 < CELL_PAD * CELL_PAD; p2++) {
                if (rgba[p2 * 4 + 3]) { any = 1; break; }
            }
            if (!any) blank++;
            snprintf(png_path, sizeof(png_path), "%s/g%d.png", out_dir, ascii);
            if (write_png_rgba(png_path, rgba, CELL_PAD, CELL_PAD) != 0) {
                fprintf(stderr, "write_png_rgba(%s) failed\n", png_path);
                fclose(mf);
                free(rgba);
                sd_font_free(&font);
                return 1;
            }
            fprintf(mf, "%d %d %d %d %d %d %d g%d.png\n",
                    ascii, ch_idx, any ? 0 : 1, CELL_PAD, CELL_PAD,
                    font_h, font_h, ascii);
            emitted++;
        }
    }

    fclose(mf);
    free(rgba);
    sd_font_free(&font);

    printf("dump_font: wrote %d PNGs (%d blank) to %s\n",
           emitted, blank, out_dir);
    printf("           manifest: %s\n", manifest_path);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s dump <FONT.DAT> <font_h> <out_dir>\n"
        "    font_h: 6 for CHARSMAL.DAT, 8 for GRAPHCHR.DAT\n",
        argv0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "dump") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        return cmd_dump(argv[2], atoi(argv[3]), argv[4]);
    }
    usage(argv[0]);
    return 1;
}
