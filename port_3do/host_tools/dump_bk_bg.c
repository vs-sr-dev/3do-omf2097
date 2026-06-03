/*
 * dump_bk_bg.c -- host-side extractor for OMF BK files (menu backdrops
 * + per-animation sprite sheets).
 *
 * Subcommands:
 *   dump <FILE.BK> <out.png>
 *       Decodes the BK's static background image through palette 0
 *       into RGBA and writes a PNG.
 *
 *   info <FILE.BK>
 *       Lists per-animation sprite count + sprite-0 (anchor) position.
 *
 *   dump-anim <FILE.BK> <anim_idx> <out_dir>
 *       Decodes EVERY sprite of animation `anim_idx` through palette 0
 *       and writes sprite_NN.png + manifest.txt into out_dir. Used for
 *       MELEE.BK anim[3] (10 HAR portraits 51x36) and anim[4] (11 pilot
 *       portraits 57x57) -- sessione 8 portrait pipeline.
 *
 * Linked against the same openomf-master/src/ subset as dump_har +
 * dump_font (see [[project-host-tools-strategy]]).
 */

#include "formats/altpal.h"
#include "formats/animation.h"
#include "formats/bk.h"
#include "formats/bkanim.h"
#include "formats/error.h"
#include "formats/palette.h"
#include "formats/rgba_image.h"
#include "formats/sprite.h"
#include "formats/vga_image.h"
#include "utils/path.h"

#include <png.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Same PNG writer signature as dump_har.c / dump_font.c. */
static int
write_png_rgba(const char *filename, const unsigned char *rgba, int w, int h)
{
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

static int
cmd_dump(const char *bk_filename, const char *out_png)
{
    sd_bk_file     bk;
    path           p;
    int            rc;
    sd_vga_image  *bg;
    vga_palette   *pal;
    sd_rgba_image  rgba;

    path_from_c(&p, bk_filename);
    if (sd_bk_create(&bk) != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_create failed\n"); return 1;
    }
    rc = sd_bk_load(&bk, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_load(%s) failed: %d (%s)\n",
                bk_filename, rc, sd_get_error(rc));
        sd_bk_free(&bk);
        return 1;
    }

    bg = sd_bk_get_background(&bk);
    if (bg == NULL || bg->data == NULL) {
        fprintf(stderr, "BK has no background image\n");
        sd_bk_free(&bk); return 1;
    }
    pal = sd_bk_get_palette(&bk, 0);
    if (pal == NULL) {
        fprintf(stderr, "BK palette 0 missing\n");
        sd_bk_free(&bk); return 1;
    }

    if (sd_rgba_image_create(&rgba, bg->w, bg->h) != SD_SUCCESS) {
        fprintf(stderr, "sd_rgba_image_create %ux%u failed\n", bg->w, bg->h);
        sd_bk_free(&bk); return 1;
    }
    if (sd_vga_image_decode(&rgba, bg, pal) != SD_SUCCESS) {
        fprintf(stderr, "sd_vga_image_decode failed\n");
        sd_rgba_image_free(&rgba); sd_bk_free(&bk); return 1;
    }

    if (write_png_rgba(out_png, (const unsigned char *)rgba.data,
                       (int)rgba.w, (int)rgba.h) != 0) {
        fprintf(stderr, "PNG write failed\n");
        sd_rgba_image_free(&rgba); sd_bk_free(&bk); return 1;
    }
    printf("dump_bk_bg: wrote %u x %u PNG -> %s\n", rgba.w, rgba.h, out_png);

    sd_rgba_image_free(&rgba);
    sd_bk_free(&bk);
    return 0;
}

/* Dump per-animation info: how many sprites each animation has and the
 * position of sprite 0 (the "sheet origin" pattern menus use). Useful to
 * identify which animation index holds e.g. the har portraits grid. */
static int
cmd_info(const char *bk_filename)
{
    sd_bk_file  bk;
    path        p;
    int         rc, i;

    path_from_c(&p, bk_filename);
    if (sd_bk_create(&bk) != SD_SUCCESS) { return 1; }
    rc = sd_bk_load(&bk, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_load(%s) failed: %d (%s)\n",
                bk_filename, rc, sd_get_error(rc));
        sd_bk_free(&bk);
        return 1;
    }

    printf("BK: %s\n", bk_filename);
    {
        int j;
        printf("  soundtable[30] (BK stl, anim_string s<N> -> SOUNDS.DAT id):\n   ");
        for (j = 0; j < 30; j++) {
            printf(" %3u", (unsigned char)bk.soundtable[j]);
            if ((j & 7) == 7) printf("\n   ");
        }
        printf("\n");
    }
    for (i = 0; i < 50; i++) {
        sd_bk_anim   *bka = sd_bk_get_anim(&bk, i);
        sd_animation *ani;
        sd_sprite    *spr0;
        if (bka == NULL) continue;
        ani = sd_bk_anim_get_animation(bka);
        if (ani == NULL) continue;
        spr0 = (ani->sprite_count > 0) ? ani->sprites[0] : NULL;
        if (spr0 != NULL) {
            printf("  anim[%2d]: sprites=%-3u sprite0 pos=(%4d,%4d) size=%dx%d\n",
                   i, ani->sprite_count, spr0->pos_x, spr0->pos_y,
                   spr0->width, spr0->height);
        } else {
            printf("  anim[%2d]: sprites=%u (no sprite 0)\n", i, ani->sprite_count);
        }
        printf("            start_pos=(%d,%d) anim_string=\"%s\"\n",
               ani->start_x, ani->start_y,
               ani->anim_string ? ani->anim_string : "");
    }

    sd_bk_free(&bk);
    return 0;
}

/* Decode every sprite of one BK animation through palette 0, writing
 * sprite_NN.png + manifest.txt into out_dir. Sessione 8: feeds the
 * pilot-portrait (anim[4], 11 sprites @ 57x57) and HAR-portrait
 * (anim[3], 10 sprites @ 51x36) pipelines.
 *
 * `pal_offset` (0..255) applies canon shader semantics
 * (openomf shaders/palette.frag L104-112) before RGBA conversion:
 *   index 0:              discarded (canon transparency marker; here kept
 *                         as palette[0] color so 3it color-keys it later)
 *   index 1..pal_limit:   clamp(index + pal_offset, 0, pal_limit)
 *   index > pal_limit:    unchanged
 * pal_limit is hardcoded to 96 (canon vs.c L576 for P2 HAR recolor).
 *
 * `inject_pilot` (0..9 = pilot index from pilots_data.h, -1 disables):
 * BEFORE applying the offset, overwrite palette[49..95] with the canon
 * pilot palette computed from ALTPALS.DAT and the pilot's color_1/2/3
 * altpal slot indices. This simulates canon's runtime
 * `palette_load_player_colors(pilot->palette, 1)`. */
static int
cmd_dump_anim(const char *bk_filename, int anim_idx, const char *out_dir,
              int pal_offset, int inject_pilot)
{
    sd_bk_file     bk;
    path           p;
    int            rc, i;
    sd_bk_anim    *bka;
    sd_animation  *ani;
    vga_palette   *pal;
    char           png_path[512];
    char           manifest_path[512];
    FILE          *mf;
    struct stat    st;

    path_from_c(&p, bk_filename);
    if (sd_bk_create(&bk) != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_create failed\n"); return 1;
    }
    rc = sd_bk_load(&bk, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_load(%s) failed: %d (%s)\n",
                bk_filename, rc, sd_get_error(rc));
        sd_bk_free(&bk);
        return 1;
    }

    bka = sd_bk_get_anim(&bk, anim_idx);
    if (bka == NULL) {
        fprintf(stderr, "BK has no animation at index %d\n", anim_idx);
        sd_bk_free(&bk); return 1;
    }
    ani = sd_bk_anim_get_animation(bka);
    if (ani == NULL || ani->sprite_count == 0) {
        fprintf(stderr, "anim[%d] has no sprites\n", anim_idx);
        sd_bk_free(&bk); return 1;
    }
    pal = sd_bk_get_palette(&bk, 0);
    if (pal == NULL) {
        fprintf(stderr, "BK palette 0 missing\n");
        sd_bk_free(&bk); return 1;
    }

    /* Inject canon player-2 palette[49..95] by reading the pilot's three
     * altpal color slots from ALTPALS.DAT, matching the canon flow:
     *   1) pilot->palette[0..15]  = altpals[0][color_3 * 16 .. + 16]   (tertiary)
     *   2) pilot->palette[16..31] = altpals[0][color_2 * 16 .. + 16]   (secondary)
     *   3) pilot->palette[32..47] = altpals[0][color_1 * 16 .. + 16]   (primary)
     *   4) base_palette[49..95]   = pilot->palette[1..47]
     *      (player_color_offset = (player=1) * 48 + 1 = 49, len=47)
     *
     * inject_pilot is the pilot index 0..9 (matches pilots_data.h ordering);
     * a value of -1 disables injection. The static table below mirrors
     * pilots_data.h color_1/color_2/color_3 fields. */
    if (inject_pilot >= 0 && inject_pilot < 10) {
        static const struct { int c1, c2, c3; } pilot_altpals[10] = {
            { 5, 11,  8 },  /* CRYSTAL */
            {10, 15,  7 },  /* STEFFAN */
            {11, 12,  7 },  /* MILANO */
            { 8, 15,  6 },  /* CHRISTIAN */
            { 4,  7, 14 },  /* SHIRRO */
            { 1,  7,  6 },  /* JEANPAUL  -- primary slot 1 = red */
            { 8,  6, 14 },  /* IBRAHIM */
            { 0, 15,  7 },  /* ANGEL */
            { 0,  8,  2 },  /* COSSETTE */
            { 9, 10,  4 },  /* RAVEN */
        };
        altpal_file ap;
        path apath;
        int c1 = pilot_altpals[inject_pilot].c1;
        int c2 = pilot_altpals[inject_pilot].c2;
        int c3 = pilot_altpals[inject_pilot].c3;
        {
            /* OMF data dir from the env (no hardcoded absolute path — same
             * OMF_DATA convention as the pipeline scripts). */
            const char *omf_dir = getenv("OMF_DATA");
            char apath_buf[512];
            if (!omf_dir)
                fprintf(stderr, "dump_bk_bg: OMF_DATA unset — set it to your "
                                "OMF:2097 data dir for pilot-altpal injection\n");
            snprintf(apath_buf, sizeof(apath_buf), "%s/ALTPALS.DAT",
                     omf_dir ? omf_dir : ".");
            path_from_c(&apath, apath_buf);
        }
        if (altpal_create(&ap) == SD_SUCCESS &&
            altpals_load(&ap, &apath) == SD_SUCCESS) {
            int k;
            vga_color pilot_pal[48];
            /* pilot->palette[0..15] = altpals slot c3 (tertiary) */
            for (k = 0; k < 16; k++) pilot_pal[k]      = ap.palettes[0].colors[c3 * 16 + k];
            /* pilot->palette[16..31] = altpals slot c2 (secondary) */
            for (k = 0; k < 16; k++) pilot_pal[16 + k] = ap.palettes[0].colors[c2 * 16 + k];
            /* pilot->palette[32..47] = altpals slot c1 (primary) */
            for (k = 0; k < 16; k++) pilot_pal[32 + k] = ap.palettes[0].colors[c1 * 16 + k];
            /* base[49..95] = pilot_pal[1..47] */
            for (k = 0; k < 47; k++) pal->colors[49 + k] = pilot_pal[1 + k];
            altpal_free(&ap);
        } else {
            fprintf(stderr, "warn: altpals load failed -- P2 colors not injected\n");
            altpal_free(&ap);
        }
    }

    /* Apply pal_offset matching the canon shader semantics
     * (openomf shaders/palette.frag L104-112):
     *   if index == transparency_index (0): DISCARD
     *   else if index <= pal_limit:         clamp(index + pal_offset, 0, pal_limit)
     *   else:                               index unchanged
     *
     * We preserve index 0 (canon transparency marker -- the bake step in 3it
     * color-keys palette[0] later) AND clamp at canon's pal_limit=96 so HAR
     * body pixels (typically index > 96) stay unchanged while only player-
     * color pixels (1..96) get shifted. Previous naive wrap-around rotation
     * shifted EVERYTHING and produced visible blotches on body pixels. */
    if (pal_offset != 0) {
        vga_color rotated[256];
        int j;
        int pal_limit = 96;     /* canon vs.c L576: object_set_pal_limit(_, 96) */
        rotated[0] = pal->colors[0];
        for (j = 1; j <= pal_limit; j++) {
            int new_idx = j + pal_offset;
            if (new_idx > pal_limit) new_idx = pal_limit;
            if (new_idx < 0)         new_idx = 0;
            rotated[j] = pal->colors[new_idx];
        }
        for (j = pal_limit + 1; j < 256; j++) {
            rotated[j] = pal->colors[j];     /* unchanged */
        }
        for (j = 0; j < 256; j++) pal->colors[j] = rotated[j];
    }

    /* Ensure out_dir exists. */
    if (stat(out_dir, &st) != 0) {
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
            sd_bk_free(&bk); return 1;
        }
    }

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", out_dir);
    mf = fopen(manifest_path, "wb");
    if (mf == NULL) {
        fprintf(stderr, "fopen(%s) failed\n", manifest_path);
        sd_bk_free(&bk); return 1;
    }
    fprintf(mf, "# BK anim[%d] sprite dump (%s)\n", anim_idx, bk_filename);
    fprintf(mf, "# sprite_idx  filename  w  h  pos_x  pos_y\n");

    for (i = 0; i < ani->sprite_count; i++) {
        sd_sprite     *s = ani->sprites[i];
        sd_rgba_image  img;
        if (s == NULL) continue;
        if (s->missing && (s->data == NULL || s->len == 0)) {
            fprintf(mf, "%d - %u %u %d %d MISSING\n",
                    i, s->width, s->height, s->pos_x, s->pos_y);
            continue;
        }
        if (sd_sprite_rgba_decode(&img, s, pal) != SD_SUCCESS) {
            fprintf(stderr, "sd_sprite_rgba_decode sprite=%d failed\n", i);
            continue;
        }
        snprintf(png_path, sizeof(png_path),
                 "%s/sprite_%02d.png", out_dir, i);
        if (write_png_rgba(png_path, (const unsigned char *)img.data,
                           (int)img.w, (int)img.h) != 0) {
            fprintf(stderr, "write_png_rgba(%s) failed\n", png_path);
            sd_rgba_image_free(&img);
            continue;
        }
        fprintf(mf, "%d sprite_%02d.png %u %u %d %d\n",
                i, i, img.w, img.h, s->pos_x, s->pos_y);
        printf("anim[%d] sprite %2d: %ux%u pos=(%d,%d) -> %s\n",
               anim_idx, i, img.w, img.h, s->pos_x, s->pos_y, png_path);
        sd_rgba_image_free(&img);
    }
    fclose(mf);

    sd_bk_free(&bk);
    return 0;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage:\n"
        "  %s dump <FILE.BK> <out.png>\n"
        "  %s info <FILE.BK>\n"
        "  %s dump-anim <FILE.BK> <anim_idx> <out_dir> [pal_offset [inject_p2]]\n",
        argv0, argv0, argv0);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "dump") == 0) {
        if (argc != 4) { usage(argv[0]); return 1; }
        return cmd_dump(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "dump-anim") == 0) {
        int pal_offset    = 0;
        int inject_pilot  = -1;
        if (argc < 5 || argc > 7) { usage(argv[0]); return 1; }
        if (argc >= 6) pal_offset   = atoi(argv[5]);
        if (argc >= 7) inject_pilot = atoi(argv[6]);
        return cmd_dump_anim(argv[2], atoi(argv[3]), argv[4],
                             pal_offset, inject_pilot);
    }
    usage(argv[0]);
    return 1;
}
