/*
 * dump_altpals.c -- ALTPALS.DAT decoder + pilot-color-swatch PNG emitter.
 *
 * For each of the 10 selectable OMF pilots, looks up the pilot's three
 * altpal-slot indices (color_1/2/3, hardcoded from openomf
 * resources/pilots.c) and emits a small PNG strip showing the
 * representative RGB of each slot. The PNGs feed into 3it -> CEL so the
 * 3DO pilot-select scene can display per-pilot color swatches.
 *
 * The "representative RGB" of an altpal slot is the middle entry
 * (index 8 of 16) of that slot's 16-color gradient.
 *
 * Usage:
 *   dump_altpals dump <ALTPALS.DAT> <out_dir>
 *       Writes out_dir/pilot_NN_swatch.png (NN = 0..9) + manifest.txt.
 */

#include "formats/altpal.h"
#include "formats/error.h"
#include "formats/palette.h"
#include "utils/path.h"

#include <png.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Pilot table -- canonical values from openomf-master/src/resources/pilots.c.
 * Each pilot has three altpal slot indices (0..15) for primary/secondary/
 * tertiary HAR colors. */
typedef struct {
    int         id;
    const char *name;
    int         color_1;
    int         color_2;
    int         color_3;
} pilot_def;

static const pilot_def gPilots[] = {
    { 0,  "CRYSTAL",   5, 11,  8 },
    { 1,  "STEFFAN",  10, 15,  7 },
    { 2,  "MILANO",   11, 12,  7 },
    { 3,  "CHRISTIAN", 8, 15,  6 },
    { 4,  "SHIRRO",    4,  7, 14 },
    { 5,  "JEANPAUL",  1,  7,  6 },
    { 6,  "IBRAHIM",   8,  6, 14 },
    { 7,  "ANGEL",     0, 15,  7 },
    { 8,  "COSSETTE",  0,  8,  2 },
    { 9,  "RAVEN",     9, 10,  4 },
    /* 10 KREISSACK -- boss-only, not in playable select grid */
};
#define PILOT_COUNT   (sizeof(gPilots) / sizeof(gPilots[0]))

#define SWATCH_W      24    /* 3 colors x 8 px wide */
#define SWATCH_H       8

static int
ensure_dir(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(p, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    fprintf(stderr, "mkdir(%s) failed: %s\n", p, strerror(errno));
    return -1;
}

static int
write_png_rgba(const char *filename, const unsigned char *rgba, int w, int h)
{
    FILE       *fp;
    png_structp png;
    png_infop   info;
    png_bytep  *rows = NULL;
    int         y;
    fp = fopen(filename, "wb");
    if (!fp) { fprintf(stderr, "fopen(%s) failed\n", filename); return -1; }
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(rows); fclose(fp); return -1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    rows = malloc(sizeof(png_bytep) * h);
    for (y = 0; y < h; y++) rows[y] = (png_bytep)(rgba + y * w * 4);
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(rows); fclose(fp);
    return 0;
}

static void
fill_band(unsigned char *rgba, int w, int h, int x0, int x1,
          unsigned char r, unsigned char g, unsigned char b)
{
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = x0; x < x1; x++) {
            int off = (y * w + x) * 4;
            rgba[off + 0] = r;
            rgba[off + 1] = g;
            rgba[off + 2] = b;
            rgba[off + 3] = 255;
        }
    }
}

static int
cmd_dump(const char *altpals_path, const char *out_dir)
{
    altpal_file ap;
    path        p;
    int         rc;
    unsigned    i;
    char        png_path[512];
    char        manifest_path[512];
    FILE       *mf;
    unsigned char *rgba;
    vga_palette *pal;

    path_from_c(&p, altpals_path);
    if (altpal_create(&ap) != SD_SUCCESS) {
        fprintf(stderr, "altpal_create failed\n"); return 1;
    }
    rc = altpals_load(&ap, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "altpals_load(%s) failed: %d (%s)\n",
                altpals_path, rc, sd_get_error(rc));
        altpal_free(&ap);
        return 1;
    }

    if (ensure_dir(out_dir) != 0) { altpal_free(&ap); return 1; }

    /* palettes[0] is the altpal set used for HAR player coloring (per
     * openomf palette.c:230 -- `altpals->palettes[0].colors[...]`). */
    pal = &ap.palettes[0];

    rgba = malloc(SWATCH_W * SWATCH_H * 4);
    if (rgba == NULL) {
        fprintf(stderr, "malloc failed\n");
        altpal_free(&ap); return 1;
    }

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", out_dir);
    mf = fopen(manifest_path, "wb");
    if (mf == NULL) {
        fprintf(stderr, "fopen %s failed\n", manifest_path);
        free(rgba); altpal_free(&ap); return 1;
    }
    fprintf(mf, "# id name color_1 color_2 color_3 swatch_png\n");

    for (i = 0; i < PILOT_COUNT; i++) {
        const pilot_def *pd = &gPilots[i];
        /* Pick middle entry (idx 8 of 16) of each 16-color slot. */
        vga_color c1 = pal->colors[pd->color_1 * 16 + 8];
        vga_color c2 = pal->colors[pd->color_2 * 16 + 8];
        vga_color c3 = pal->colors[pd->color_3 * 16 + 8];

        memset(rgba, 0, SWATCH_W * SWATCH_H * 4);
        fill_band(rgba, SWATCH_W, SWATCH_H,  0,  8, c1.r, c1.g, c1.b);
        fill_band(rgba, SWATCH_W, SWATCH_H,  8, 16, c2.r, c2.g, c2.b);
        fill_band(rgba, SWATCH_W, SWATCH_H, 16, 24, c3.r, c3.g, c3.b);

        snprintf(png_path, sizeof(png_path),
                 "%s/pilot_%02d_swatch.png", out_dir, pd->id);
        if (write_png_rgba(png_path, rgba, SWATCH_W, SWATCH_H) != 0) {
            fprintf(stderr, "PNG write failed: %s\n", png_path);
            fclose(mf); free(rgba); altpal_free(&ap); return 1;
        }
        fprintf(mf, "%d %s %d %d %d pilot_%02d_swatch.png\n",
                pd->id, pd->name, pd->color_1, pd->color_2, pd->color_3, pd->id);
        printf("pilot %d (%s) c=(%d,%d,%d) middle RGB: "
               "(%d,%d,%d) (%d,%d,%d) (%d,%d,%d)\n",
               pd->id, pd->name, pd->color_1, pd->color_2, pd->color_3,
               c1.r, c1.g, c1.b, c2.r, c2.g, c2.b, c3.r, c3.g, c3.b);
    }
    fclose(mf);
    free(rgba);
    altpal_free(&ap);
    printf("dump_altpals: wrote %u swatches to %s\n",
           (unsigned)PILOT_COUNT, out_dir);
    return 0;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage:\n  %s dump <ALTPALS.DAT> <out_dir>\n", argv0);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "dump") == 0) {
        if (argc != 4) { usage(argv[0]); return 1; }
        return cmd_dump(argv[2], argv[3]);
    }
    usage(argv[0]);
    return 1;
}
