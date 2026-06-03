/*
 * pal_diag.c — throwaway palette diagnostic for the §1.4 fight-palette question.
 *
 * (1) Print BK palette indices 0..47 (the player-colorable zone) in RGB +
 *     RGB555, and test whether the three 16-color zones are IDENTICAL greys.
 * (2) Walk every Jaguar atlas frame, decode sprite VGA indices, histogram which
 *     0..47 indices are used and, per frame, how many DISTINCT indices come from
 *     each zone — to size a sentinel re-extraction (does a frame stay <=16
 *     distinct colors, i.e. 4bpp, or blow past it -> 6bpp/bigger atlas?).
 *
 * Usage: pal_diag <FIGHTRn.AF> <ARENAm.BK>
 */
#include "formats/af.h"
#include "formats/bk.h"
#include "formats/error.h"
#include "formats/move.h"
#include "formats/animation.h"
#include "formats/sprite.h"
#include "formats/palette.h"
#include "formats/vga_image.h"
#include "utils/path.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_AF_MOVES 70

int main(int argc, char **argv) {
    sd_af_file af;
    sd_bk_file bk;
    path p;
    vga_palette *pal;
    int i, j, z, lvl;

    if (argc != 3) { fprintf(stderr, "usage: %s <AF> <BK>\n", argv[0]); return 2; }

    if (sd_af_create(&af) != SD_SUCCESS) return 1;
    path_from_c(&p, argv[1]);
    if (sd_af_load(&af, &p) != SD_SUCCESS) { fprintf(stderr, "af load fail\n"); return 1; }
    if (sd_bk_create(&bk) != SD_SUCCESS) return 1;
    path_from_c(&p, argv[2]);
    if (sd_bk_load(&bk, &p) != SD_SUCCESS) { fprintf(stderr, "bk load fail\n"); return 1; }

    pal = bk.palettes[0];
    printf("=== BK palette[0] indices 0..47 (player-colorable zone) ===\n");
    printf("idx  R   G   B    (zone, level)\n");
    for (i = 0; i < 48; i++) {
        vga_color c = pal->colors[i];
        z = i / 16; lvl = i % 16;
        printf("%2d  %3d %3d %3d   zone=%d level=%2d %s\n",
               i, c.r, c.g, c.b, z, lvl, (c.r==c.g && c.g==c.b) ? "[grey]" : "");
    }

    /* Are zones identical? Compare TERTIARY(0..15) vs SECONDARY(16..31) vs PRIMARY(32..47). */
    {
        int sec_eq = 1, pri_eq = 1;
        for (lvl = 0; lvl < 16; lvl++) {
            vga_color t = pal->colors[lvl];
            vga_color s = pal->colors[16 + lvl];
            vga_color pr = pal->colors[32 + lvl];
            if (t.r != s.r || t.g != s.g || t.b != s.b) sec_eq = 0;
            if (t.r != pr.r || t.g != pr.g || t.b != pr.b) pri_eq = 0;
        }
        printf("\nZONE COMPARE: tertiary==secondary? %s   tertiary==primary? %s\n",
               sec_eq ? "YES (identical)" : "NO (distinct)",
               pri_eq ? "YES (identical)" : "NO (distinct)");
    }

    /* Walk atlas frame order, histogram 0..47 usage + per-frame zone span. */
    {
        long hist[48]; memset(hist, 0, sizeof(hist));
        int frames = 0;
        int max_distinct_armor = 0;      /* max distinct 0..47 indices in one frame */
        int max_distinct_all = 0;        /* max distinct ANY index in one frame      */
        int frames_over16 = 0;           /* frames needing >16 distinct -> >4bpp      */
        int b_le16=0, b_17_32=0, b_33_48=0;  /* coded-CEL bucket distribution         */
        long pdat_minbpp = 0;            /* sum ceil(w*h*bpp/8), bpp by color count   */
        int max_s2=0, max_s3=0, over32_s2=0, over32_s3=0;  /* partial-recolor scenarios */
        long pdat_s2=0;                  /* S2 PDAT est (4bpp<=16, else 6bpp)         */
        int zone_used_any[3] = {0,0,0};
        for (i = 0; i < MAX_AF_MOVES; i++) {
            sd_move *mv = af.moves[i];
            sd_animation *ani;
            if (mv == NULL || mv->animation == NULL) continue;
            ani = mv->animation;
            for (j = 0; j < ani->sprite_count; j++) {
                sd_sprite *s = ani->sprites[j];
                sd_vga_image vimg;
                uint8_t seen[256];
                int distinct_all = 0, distinct_armor = 0;
                long n, q;
                if (s == NULL) continue;
                if (s->missing && (s->data == NULL || s->len == 0)) continue;
                if (sd_sprite_vga_decode(&vimg, s) != SD_SUCCESS) continue;
                frames++;
                memset(seen, 0, sizeof(seen));
                n = (long)vimg.w * (long)vimg.h;
                for (q = 0; q < n; q++) {
                    uint8_t idx = (uint8_t)vimg.data[q];
                    if (!seen[idx]) {
                        seen[idx] = 1;
                        distinct_all++;
                        if (idx < 48) {
                            distinct_armor++;
                            hist[idx]++;
                            zone_used_any[idx/16] = 1;
                        }
                    }
                }
                /* Scenario counts: how many distinct COLORS a frame has if we
                 * keep only some zones distinct and collapse the rest to the
                 * shared grey ramp (by level). Counts toward the 32 coded cap. */
                {
                    uint8_t pri_seen=0; int s2=0, s3=0;
                    uint8_t glvl_s2[16]={0}, glvl_s3[16]={0};
                    uint8_t pidx_s2[48]={0}, pidx_s3[48]={0};
                    int x;
                    (void)pri_seen;
                    for (x = 0; x < 256; x++) {
                        if (!seen[x]) continue;
                        if (x >= 48) { s2++; s3++; continue; } /* body: always distinct */
                        if (x == 0) continue;                  /* transparency */
                        /* S2: primary(32..47) distinct; sec+ter -> grey level */
                        if (x >= 32) { if(!pidx_s2[x]){pidx_s2[x]=1;s2++;} }
                        else         { int L=x&15; if(!glvl_s2[L]){glvl_s2[L]=1;s2++;} }
                        /* S3: primary+secondary(16..47) distinct; ter -> grey level */
                        if (x >= 16) { if(!pidx_s3[x]){pidx_s3[x]=1;s3++;} }
                        else         { int L=x&15; if(!glvl_s3[L]){glvl_s3[L]=1;s3++;} }
                    }
                    if (s2 > max_s2) max_s2 = s2;
                    if (s3 > max_s3) max_s3 = s3;
                    if (s2 > 32) over32_s2++;
                    if (s3 > 32) over32_s3++;
                    {
                        int bpp2 = (s2 <= 16) ? 4 : (s2 <= 32 ? 6 : 8);
                        pdat_s2 += ((long)vimg.w * (long)vimg.h * bpp2 + 7) / 8;
                    }
                }
                if (distinct_armor > max_distinct_armor) max_distinct_armor = distinct_armor;
                if (distinct_all > max_distinct_all) max_distinct_all = distinct_all;
                if (distinct_all > 16) frames_over16++;
                {
                    int bpp;
                    if (distinct_all <= 16) { b_le16++; bpp = 4; }
                    else if (distinct_all <= 32) { b_17_32++; bpp = 6; }
                    else { b_33_48++; bpp = 8; }  /* >32 cannot be a 3DO coded CEL! */
                    pdat_minbpp += ((long)vimg.w * (long)vimg.h * bpp + 7) / 8;
                }
                sd_vga_image_free(&vimg);
            }
        }
        printf("\n=== sprite index usage across %d frames ===\n", frames);
        printf("zones used by ANY frame: tertiary(0-15)=%d secondary(16-31)=%d primary(32-47)=%d\n",
               zone_used_any[0], zone_used_any[1], zone_used_any[2]);
        printf("max DISTINCT armor(0..47) indices in one frame: %d\n", max_distinct_armor);
        printf("max DISTINCT total indices in one frame:        %d\n", max_distinct_all);
        printf("frames needing >16 distinct colors (>4bpp):     %d\n", frames_over16);
        printf("\n=== 3DO coded-CEL feasibility (PLUT cap = 32 colors) ===\n");
        printf("  <=16 colors (4bpp coded):    %d frames\n", b_le16);
        printf("  17..32 colors (6bpp coded):  %d frames\n", b_17_32);
        printf("  33..48 colors (NOT codeable, need merge/uncoded): %d frames\n", b_33_48);
        printf("  est PDAT (per-frame min bpp, treating >32 as 8bpp): %ld bytes (%ld KB)\n",
               pdat_minbpp, pdat_minbpp / 1024);
        printf("\n=== PARTIAL-recolor scenarios (collapse some zones to shared grey) ===\n");
        printf("  S2 primary-only distinct (sec+ter grey): max %d colors/frame, %d frames >32\n",
               max_s2, over32_s2);
        printf("  S3 primary+secondary distinct (ter grey): max %d colors/frame, %d frames >32\n",
               max_s3, over32_s3);
        printf("  S2 est PDAT blob: %ld bytes (%ld KB)  [vs current neutral atlas]\n",
               pdat_s2, pdat_s2 / 1024);
        printf("\nper-index armor histogram (frame count using each index):\n");
        for (i = 0; i < 48; i++) {
            if (i % 16 == 0) printf("  zone %d: ", i/16);
            printf("%4ld", hist[i]);
            if (i % 16 == 15) printf("\n");
        }
    }

    sd_bk_free(&bk); sd_af_free(&af);
    return 0;
}
