/*
 * dump_har.c — host-side OMF→3DO asset converter.
 *
 * Subcommands:
 *   list <FIGHTR*.AF>
 *       Print the move table (Phase A — sanity check).
 *
 *   dump <FIGHTR*.AF> <ARENA*.BK> <move_id> <out_dir>
 *       For the given move, decode every sprite into RGBA via the BK palette,
 *       write frame_NN.png into out_dir, and emit out_dir/manifest.txt with
 *       per-frame metadata (filename, w, h, pos_x, pos_y).
 *
 * Phase B output is consumed by:
 *   1. `3it to-cel --find-smallest regular` to produce 3DO CELs.
 *   2. The 3DO runtime which reads manifest.txt-derived constants compiled
 *      into the takeme image to step animations frame-by-frame.
 *
 * Linked against a hand-picked subset of openomf-master/src/ — see
 * [[project-host-tools-strategy]] in /memory. openomf-master stays
 * untouched per [[feedback-isolated-project]].
 */

#include "formats/af.h"
#include "formats/altpal.h"
#include "formats/bk.h"
#include "formats/error.h"
#include "formats/move.h"
#include "formats/animation.h"
#include "formats/script.h"
#include "formats/sounds.h"
#include "formats/sprite.h"
#include "formats/palette.h"
#include "formats/rgba_image.h"
#include "formats/vga_image.h"
#include "utils/path.h"
#include "utils/vector.h"

#include <png.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------- AIFF writer (8-bit signed mono PCM at native rate) ---------- */

static void write_be16(FILE *fp, unsigned int v) {
    fputc((v >> 8) & 0xff, fp);
    fputc(v & 0xff, fp);
}

static void write_be32(FILE *fp, unsigned int v) {
    fputc((v >> 24) & 0xff, fp);
    fputc((v >> 16) & 0xff, fp);
    fputc((v >> 8)  & 0xff, fp);
    fputc(v & 0xff, fp);
}

/* IEEE 754 80-bit extended big-endian, integer-valued (used for AIFF sample rate). */
static void write_extended80(FILE *fp, unsigned int rate) {
    unsigned char buf[10];
    int e;
    uint32_t n;
    uint64_t mantissa;
    int exp;

    memset(buf, 0, sizeof(buf));
    if (rate == 0) { fwrite(buf, 1, 10, fp); return; }
    e = 0;
    n = rate;
    while ((n >> 1) != 0) { n >>= 1; e++; }
    /* highest set bit is at position e. */
    exp = 16383 + e;
    buf[0] = (unsigned char)((exp >> 8) & 0xff);
    buf[1] = (unsigned char)(exp & 0xff);
    mantissa = ((uint64_t)rate) << (63 - e);
    buf[2] = (unsigned char)((mantissa >> 56) & 0xff);
    buf[3] = (unsigned char)((mantissa >> 48) & 0xff);
    buf[4] = (unsigned char)((mantissa >> 40) & 0xff);
    buf[5] = (unsigned char)((mantissa >> 32) & 0xff);
    buf[6] = (unsigned char)((mantissa >> 24) & 0xff);
    buf[7] = (unsigned char)((mantissa >> 16) & 0xff);
    buf[8] = (unsigned char)((mantissa >> 8)  & 0xff);
    buf[9] = (unsigned char)(mantissa & 0xff);
    fwrite(buf, 1, 10, fp);
}

/* Writes a 16-bit signed big-endian mono PCM AIFF. `unsigned_pcm` is OMF
 * 8-bit UNSIGNED data (0..255 centered on 128); we sign-convert and promote
 * to 16-bit BE (s16 = (s8) << 8) — 3DO LoadSample expects 16-bit samples
 * (confirmed by inspecting devkit example AIFFs which are all pcm_s16be). */
static int write_aiff_8bit_mono(const char *filename, const char *unsigned_pcm,
                                int len, int sample_rate) {
    FILE *fp;
    int comm_size, ssnd_size, form_size, data_bytes, i;

    fp = fopen(filename, "wb");
    if (!fp) { fprintf(stderr, "fopen(%s, wb) failed\n", filename); return -1; }

    data_bytes = len * 2;                 /* 16-bit samples */
    comm_size = 18;                       /* numCh(2)+frames(4)+bits(2)+rate(10) */
    ssnd_size = 8 + data_bytes;           /* offset(4)+blockSize(4)+samples */
    form_size = 4 + (8 + comm_size) + (8 + ssnd_size);

    /* FORM */
    fwrite("FORM", 1, 4, fp);
    write_be32(fp, (unsigned)form_size);
    fwrite("AIFF", 1, 4, fp);

    /* COMM */
    fwrite("COMM", 1, 4, fp);
    write_be32(fp, (unsigned)comm_size);
    write_be16(fp, 1);                    /* numChannels */
    write_be32(fp, (unsigned)len);        /* numSampleFrames (frames, not bytes) */
    write_be16(fp, 16);                   /* sampleSize */
    write_extended80(fp, (unsigned)sample_rate);

    /* SSND */
    fwrite("SSND", 1, 4, fp);
    write_be32(fp, (unsigned)ssnd_size);
    write_be32(fp, 0);                    /* offset */
    write_be32(fp, 0);                    /* blockSize */
    for (i = 0; i < len; i++) {
        int u = (unsigned char)unsigned_pcm[i];
        int s16 = (u - 128) << 8;         /* sign-convert and promote */
        if (s16 > 32767) s16 = 32767;
        if (s16 < -32768) s16 = -32768;
        fputc((s16 >> 8) & 0xff, fp);     /* big-endian */
        fputc(s16 & 0xff, fp);
    }
    fclose(fp);
    return 0;
}

static int write_rgba_png(const char *filename, int w, int h, const unsigned char *rgba) {
    FILE *fp;
    png_structp png;
    png_infop info;
    png_bytep *rows = NULL;
    int y;

    fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "fopen(%s, wb) failed\n", filename);
        return -1;
    }
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

static int cmd_list(const char *af_filename) {
    sd_af_file af;
    path p;
    int rc;
    int i;
    int total_moves;
    int total_sprites;

    path_from_c(&p, af_filename);
    if (sd_af_create(&af) != SD_SUCCESS) {
        fprintf(stderr, "sd_af_create failed\n");
        return 1;
    }
    rc = sd_af_load(&af, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_af_load(%s) failed: %d (%s)\n",
                af_filename, rc, sd_get_error(rc));
        sd_af_free(&af);
        return 1;
    }

    printf("AF file: %s\n", af_filename);
    printf("  file_id=%u exec_window=%u endurance=%.2f health=%u\n",
           af.file_id, af.exec_window, af.endurance, af.health);
    printf("  speeds: fwd=%.2f bwd=%.2f jump=%.2f fall=%.2f\n",
           af.forward_speed, af.reverse_speed, af.jump_speed, af.fall_speed);

    total_moves = 0;
    total_sprites = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af.moves[i];
        if (mv == NULL) continue;
        total_moves++;
        if (mv->animation != NULL) {
            total_sprites += mv->animation->sprite_count;
        }
        printf("  move[%2d]: cat=%-3u next=%-3u dmg=%-3u sprites=%-3u  in='%s'  foot='%s'\n",
               i,
               mv->category,
               mv->next_anim_id,
               mv->damage_amount,
               mv->animation ? mv->animation->sprite_count : 0,
               mv->move_string[0] ? mv->move_string : "",
               mv->footer_string[0] ? mv->footer_string : "");
    }
    printf("  total: %d moves, %d sprites\n", total_moves, total_sprites);

    sd_af_free(&af);
    return 0;
}

/* Sound playback rate from OMF freq_key byte (per openomf soundtool).
 *   freq_hz = 1_000_000 / (256 - freq_key)
 * freq_key 128 -> ~7812 Hz, 200 -> ~17857 Hz, etc. */
static int omf_freq_from_key(int freq_key) {
    int denom = 256 - (freq_key & 0xff);
    if (denom <= 0) denom = 1;
    return 1000000 / denom;
}

/* For each non-zero entry in af->soundtable[30], export the referenced sample from
 * SOUNDS.DAT as an AIFF file in out_dir, and write `sfx_index.txt`:
 *
 *   <param>  <sounds_dat_id>  <freq_hz>  <len_bytes>  <filename>
 *
 * Plus a 'used_sids.txt' listing the (deduplicated) underlying SOUNDS.DAT ids so the
 * runtime can build a small effect cache without duplicating shared samples. */
static int cmd_sounds(const char *snd_filename,
                      const char *af_filename,
                      const char *out_dir) {
    sd_sound_file sf;
    sd_af_file af;
    path p;
    int rc;
    int i, j;
    char aiff_name[512];
    char index_path[512];
    FILE *ix;
    int dumped_sids[300];
    int n_dumped = 0;

    /* Load SOUNDS.DAT */
    sd_sounds_create(&sf);
    path_from_c(&p, snd_filename);
    rc = sd_sounds_load(&sf, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_sounds_load(%s) failed: %d (%s)\n",
                snd_filename, rc, sd_get_error(rc));
        sd_sounds_free(&sf);
        return 1;
    }

    /* Load AF for the per-fighter soundtable[30]. */
    if (sd_af_create(&af) != SD_SUCCESS) {
        sd_sounds_free(&sf); return 1;
    }
    path_from_c(&p, af_filename);
    rc = sd_af_load(&af, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_af_load(%s) failed: %d (%s)\n",
                af_filename, rc, sd_get_error(rc));
        sd_af_free(&af); sd_sounds_free(&sf); return 1;
    }

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        sd_af_free(&af); sd_sounds_free(&sf); return 1;
    }

    snprintf(index_path, sizeof(index_path), "%s/sfx_index.txt", out_dir);
    ix = fopen(index_path, "w");
    if (!ix) {
        fprintf(stderr, "fopen(%s) failed\n", index_path);
        sd_af_free(&af); sd_sounds_free(&sf); return 1;
    }
    fprintf(ix, "# OMF SFX index for %s\n", af_filename);
    fprintf(ix, "# param  sounds_dat_id  freq_hz  len_bytes  aiff_filename\n");

    /* Iterate the AF soundtable. Index 0 is treated as "unused" in animations
     * (the `s` tag values start at 1 in practice), but we still export it if it
     * points to a non-empty sample because some moves may use s0. */
    for (i = 0; i < 30; i++) {
        int sound_id;
        const sd_sound *snd;
        int freq_hz;
        int already_dumped;

        sound_id = (unsigned char)af.soundtable[i];
        if (sound_id == 0) {
            /* Empty slot — never referenced. */
            continue;
        }
        snd = sd_sounds_get(&sf, sound_id - 1);
        if (snd == NULL || snd->len < 2) {
            fprintf(ix, "# param=%d sounds_dat_id=%d -> invalid/empty\n", i, sound_id);
            continue;
        }
        freq_hz = omf_freq_from_key(snd->freq_key);

        /* Deduplicate: only write one AIFF per underlying SOUNDS.DAT id. */
        already_dumped = 0;
        for (j = 0; j < n_dumped; j++) {
            if (dumped_sids[j] == sound_id) { already_dumped = 1; break; }
        }
        snprintf(aiff_name, sizeof(aiff_name), "%s/sfx_%03d.aiff", out_dir, sound_id);
        if (!already_dumped) {
            if (write_aiff_8bit_mono(aiff_name, snd->data, snd->len, freq_hz) != 0) {
                fprintf(stderr, "write_aiff_8bit_mono(%s) failed\n", aiff_name);
                continue;
            }
            dumped_sids[n_dumped++] = sound_id;
            printf("  wrote %s (%d bytes, %d Hz)\n", aiff_name, snd->len, freq_hz);
        }
        fprintf(ix, "%d %d %d %d sfx_%03d.aiff\n",
                i, sound_id, freq_hz, snd->len, sound_id);
    }
    fclose(ix);
    printf("sfx index: %s (%d unique samples)\n", index_path, n_dumped);

    sd_af_free(&af);
    sd_sounds_free(&sf);
    return 0;
}

/* Extract specific SOUNDS.DAT entries by ID (1-based). Used by sessione 10.6
 * to grab the announcer voice samples for the ROUND/FIGHT intro lock without
 * needing an AF context. Usage:
 *   dump_har sound <SOUNDS.DAT> <out_dir> <id1> [id2 ...] */
static int cmd_sound_by_id(const char *snd_filename,
                           const char *out_dir,
                           int argc_ids, char **argv_ids) {
    sd_sound_file sf;
    path p;
    int rc, i;
    char aiff_name[512];

    sd_sounds_create(&sf);
    path_from_c(&p, snd_filename);
    rc = sd_sounds_load(&sf, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_sounds_load(%s) failed: %d (%s)\n",
                snd_filename, rc, sd_get_error(rc));
        sd_sounds_free(&sf); return 1;
    }
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        sd_sounds_free(&sf); return 1;
    }
    for (i = 0; i < argc_ids; i++) {
        int sound_id = atoi(argv_ids[i]);
        const sd_sound *snd;
        int freq_hz;
        if (sound_id < 1) { fprintf(stderr, "skip invalid id %d\n", sound_id); continue; }
        snd = sd_sounds_get(&sf, sound_id - 1);
        if (snd == NULL || snd->len < 2) {
            fprintf(stderr, "id=%d invalid/empty\n", sound_id);
            continue;
        }
        freq_hz = omf_freq_from_key(snd->freq_key);
        snprintf(aiff_name, sizeof(aiff_name), "%s/sfx_%03d.aiff", out_dir, sound_id);
        if (write_aiff_8bit_mono(aiff_name, snd->data, snd->len, freq_hz) != 0) {
            fprintf(stderr, "write_aiff_8bit_mono(%s) failed\n", aiff_name);
            continue;
        }
        printf("  wrote %s (%d bytes, %d Hz)\n", aiff_name, snd->len, freq_hz);
    }
    sd_sounds_free(&sf);
    return 0;
}

/* Write steps.txt for one move using sd_script_decode for proper tag parsing.
 * Each line:
 *   <step_idx> <sprite_idx> <dwell_ticks> <sound_param> <sound_freq> <sound_vol> <sound_pan> <flip_r> <flip_f>
 *
 * - sound_param: -1 if no `s` tag, else 0..29 (index into AF soundtable)
 * - sound_freq:  0 if no `sf` tag, else signed -128..128 pitch offset
 * - sound_vol:   -1 if no `l`  tag, else 0..63
 * - sound_pan:   -127 if no `sb` tag, else -100..100
 * - flip_r/_f:   0 or 1 */
static int write_steps_txt(const char *out_path, const char *anim_string) {
    sd_script script;
    int rc, i;
    FILE *f;
    int invalid_pos = -1;
    unsigned n_frames;

    sd_script_create(&script);
    rc = sd_script_decode(&script, anim_string, &invalid_pos);
    /* Even partial parses (rc != SUCCESS) often yield usable frames; we just
     * emit whatever frames were decoded. */
    (void)rc;

    f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "fopen(%s, w) failed\n", out_path);
        sd_script_free(&script);
        return -1;
    }
    fprintf(f, "# OMF per-frame decoded animation steps\n");
    fprintf(f, "# anim_string: %s\n", anim_string);
    fprintf(f, "# step sprite dwell s_param s_freq s_vol s_pan r f\n");

    n_frames = vector_size(&script.frames);
    for (i = 0; i < (int)n_frames; i++) {
        const sd_script_frame *fr = sd_script_get_frame(&script, i);
        int s_param  = sd_script_isset(fr, "s")  ? sd_script_get(fr, "s")  : -1;
        int s_freq   = sd_script_isset(fr, "sf") ? sd_script_get(fr, "sf") : 0;
        int s_vol    = sd_script_isset(fr, "l")  ? sd_script_get(fr, "l")  : -1;
        int s_pan    = sd_script_isset(fr, "sb") ? sd_script_get(fr, "sb") : -127;
        int flip_r   = sd_script_isset(fr, "r")  ? 1 : 0;
        int flip_f   = sd_script_isset(fr, "f")  ? 1 : 0;
        fprintf(f, "%d %d %d %d %d %d %d %d %d\n",
                i, fr->sprite, fr->tick_len,
                s_param, s_freq, s_vol, s_pan, flip_r, flip_f);
    }
    fclose(f);
    sd_script_free(&script);
    return 0;
}

/* Internal helper: write PNGs + manifest.txt + steps.txt for one move,
 * given an already-loaded AF + palette. Used by both cmd_dump (single move)
 * and cmd_dump_all (all moves). */
static int dump_move_inner(sd_af_file *af, const vga_palette *pal,
                           int move_id, const char *out_dir, int verbose) {
    sd_move *mv;
    sd_animation *ani;
    int i, rc;
    char png_name[512];
    char manifest_path[512];
    FILE *mf;

    if (move_id < 0 || move_id >= MAX_AF_MOVES || af->moves[move_id] == NULL) {
        return -1;  /* move doesn't exist — quiet skip in dump-all mode */
    }
    mv = af->moves[move_id];
    ani = mv->animation;
    if (ani == NULL) return -1;

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        return 1;
    }

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", out_dir);
    mf = fopen(manifest_path, "w");
    if (!mf) {
        fprintf(stderr, "fopen(%s) failed\n", manifest_path);
        return 1;
    }
    fprintf(mf, "# OMF HAR animation manifest\n");
    fprintf(mf, "# move: %d  category: %u\n", move_id, mv->category);
    fprintf(mf, "# move_string: %s\n", mv->move_string[0] ? mv->move_string : "");
    fprintf(mf, "# anim_string: %s\n", ani->anim_string);
    fprintf(mf, "# start_x=%d start_y=%d sprite_count=%d\n",
            ani->start_x, ani->start_y, ani->sprite_count);
    fprintf(mf, "# frame  filename  w  h  pos_x  pos_y  missing\n");

    for (i = 0; i < ani->sprite_count; i++) {
        sd_sprite *s = ani->sprites[i];
        sd_rgba_image img;
        sd_vga_image vimg;
        if (s == NULL) continue;
        /* A 'missing' sprite is a cross-move reference resolved by
         * sd_af_postprocess: its `data` pointer is reassigned to point at
         * another move's master sprite. We can decode it just like a regular
         * sprite as long as data/len are populated. Only the truly orphan
         * cases (no master found) stay marked missing in the manifest. */
        if (s->missing && (s->data == NULL || s->len == 0)) {
            fprintf(mf, "%d - %d %d %d %d 1\n",
                    i, s->width, s->height, s->pos_x, s->pos_y);
            continue;
        }
        rc = sd_sprite_rgba_decode(&img, s, pal);
        if (rc != SD_SUCCESS) {
            fprintf(stderr, "sd_sprite_rgba_decode(move=%d sprite=%d) failed: %d\n",
                    move_id, i, rc);
            continue;
        }
        snprintf(png_name, sizeof(png_name), "%s/frame_%02d.png", out_dir, i);
        if (write_rgba_png(png_name, (int)img.w, (int)img.h, (unsigned char *)img.data) == 0) {
            if (verbose) {
                printf("  wrote %s (%ux%u, pos=(%d,%d))\n",
                       png_name, img.w, img.h, s->pos_x, s->pos_y);
            }
            fprintf(mf, "%d frame_%02d.png %u %u %d %d 0\n",
                    i, i, img.w, img.h, s->pos_x, s->pos_y);
        } else {
            fprintf(stderr, "write_rgba_png(%s) failed\n", png_name);
        }
        sd_rgba_image_free(&img);

        /* ---- Per-frame hitmask emission (sessione 5 / tappa B + sessione 6 pack) ----
         * Decode the sprite to raw VGA-indexed pixels, then bit-pack 1 bit per
         * pixel into the .mask file: set = opaque, clear = transparent. Matches
         * the reader expectation in port_3do/src/intersect.c (diff #5). Saves
         * 8× vs the prior 1-byte/pixel 0/50/200 encoding — ~800 KB out of the
         * baked Jaguar hitmasks. The main-vs-accent distinction (was idx<96)
         * is folded into a single opacity bit; see intersect.c comment. */
        if (sd_sprite_vga_decode(&vimg, s) == SD_SUCCESS) {
            char mask_path[512];
            FILE *mfp;
            unsigned int npix = vimg.w * vimg.h;
            unsigned int nbytes = (npix + 7) / 8;
            unsigned int p;
            unsigned char *buf = (unsigned char *)calloc(nbytes, 1);
            if (buf != NULL) {
                for (p = 0; p < npix; p++) {
                    if ((unsigned char)vimg.data[p] != 0) {
                        buf[p >> 3] |= (unsigned char)(1u << (p & 7));
                    }
                }
                snprintf(mask_path, sizeof(mask_path), "%s/frame_%02d.mask", out_dir, i);
                mfp = fopen(mask_path, "wb");
                if (mfp) {
                    fwrite(buf, 1, nbytes, mfp);
                    fclose(mfp);
                } else {
                    fprintf(stderr, "fopen(%s, wb) failed\n", mask_path);
                }
                free(buf);
            }
            sd_vga_image_free(&vimg);
        }
    }
    fclose(mf);

    /* Per-move collision-coords table (sessione 5 / tappa B). Format:
     *   <frame_idx> <x> <y>
     * One line per entry; collision_coord_count is derivable from line count. */
    {
        char coords_path[512];
        FILE *cf;
        snprintf(coords_path, sizeof(coords_path), "%s/coords.txt", out_dir);
        cf = fopen(coords_path, "w");
        if (cf) {
            int ci;
            fprintf(cf, "# OMF collision coords for move %d\n", move_id);
            fprintf(cf, "# frame_idx x y\n");
            for (ci = 0; ci < ani->coord_count; ci++) {
                fprintf(cf, "%d %d %d\n",
                        ani->coord_table[ci].frame_id,
                        ani->coord_table[ci].x,
                        ani->coord_table[ci].y);
            }
            fclose(cf);
        }
    }

    /* Also emit per-frame parsed steps for the runtime. */
    {
        char steps_path[512];
        snprintf(steps_path, sizeof(steps_path), "%s/steps.txt", out_dir);
        write_steps_txt(steps_path, ani->anim_string);
    }
    return 0;
}

static int load_af_bk(sd_af_file *af, sd_bk_file *bk,
                      const char *af_filename, const char *bk_filename) {
    path p;
    int rc;
    path_from_c(&p, af_filename);
    if (sd_af_create(af) != SD_SUCCESS) {
        fprintf(stderr, "sd_af_create failed\n"); return -1;
    }
    rc = sd_af_load(af, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_af_load(%s) failed: %d (%s)\n",
                af_filename, rc, sd_get_error(rc));
        sd_af_free(af); return -1;
    }
    path_from_c(&p, bk_filename);
    if (sd_bk_create(bk) != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_create failed\n");
        sd_af_free(af); return -1;
    }
    rc = sd_bk_load(bk, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_load(%s) failed: %d (%s)\n",
                bk_filename, rc, sd_get_error(rc));
        sd_bk_free(bk); sd_af_free(af); return -1;
    }
    if (bk->palettes[0] == NULL) {
        fprintf(stderr, "BK has no palette[0]\n");
        sd_bk_free(bk); sd_af_free(af); return -1;
    }
    return 0;
}

static int cmd_dump(const char *af_filename,
                    const char *bk_filename,
                    int move_id,
                    const char *out_dir) {
    sd_af_file af;
    sd_bk_file bk;
    int rc;

    if (load_af_bk(&af, &bk, af_filename, bk_filename) != 0) return 1;
    rc = dump_move_inner(&af, bk.palettes[0], move_id, out_dir, 1);
    sd_bk_free(&bk);
    sd_af_free(&af);
    if (rc != 0) {
        fprintf(stderr, "move %d does not exist or has no animation\n", move_id);
        return 1;
    }
    printf("manifest: %s/manifest.txt\n", out_dir);
    return 0;
}

/* Pilot color triples mirror dump_altpals.c gPilots[]. Index = PILOT_*
 * enum from openomf common_defines.h:78-94. Only 10 playable pilots;
 * NOVA / Kreissack (10) is the boss and isn't selectable. */
typedef struct {
    int color_1;
    int color_2;
    int color_3;
} pilot_altpal_triple;

static const pilot_altpal_triple gPilotTriples[10] = {
    {  5, 11,  8 },   /* CRYSTAL  */
    { 10, 15,  7 },   /* STEFFAN  */
    { 11, 12,  7 },   /* MILANO   */
    {  8, 15,  6 },   /* CHRISTIAN*/
    {  4,  7, 14 },   /* SHIRRO   */
    {  1,  7,  6 },   /* JEANPAUL */
    {  8,  6, 14 },   /* IBRAHIM  */
    {  0, 15,  7 },   /* ANGEL    */
    {  0,  8,  2 },   /* COSSETTE */
    {  9, 10,  4 },   /* RAVEN    */
};

/* Apply pilot altpal injection to the BK arena palette in place. Mirrors
 * the canonical openomf palette_load_altpal_player_color routine
 * (palette.c:227): for player=0 (P1), palette indices 0..47 get
 * overwritten with the pilot's three 16-color shades from
 * altpals->palettes[0]. */
static int apply_pilot_altpal(vga_palette *pal, const char *altpals_path,
                              int pilot_id) {
    altpal_file ap;
    path p;
    int rc;
    const pilot_altpal_triple *t;
    int dst_offsets[3];
    int src_offsets[3];
    vga_color saved_color0;
    int i;
    if (pilot_id < 0 || pilot_id >= 10) {
        fprintf(stderr, "apply_pilot_altpal: pilot %d out of range\n", pilot_id);
        return -1;
    }
    if (altpal_create(&ap) != SD_SUCCESS) {
        fprintf(stderr, "altpal_create failed\n"); return -1;
    }
    path_from_c(&p, altpals_path);
    rc = altpals_load(&ap, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "altpals_load(%s) failed: %d (%s)\n",
                altpals_path, rc, sd_get_error(rc));
        altpal_free(&ap);
        return -1;
    }
    t = &gPilotTriples[pilot_id];
    /* Canon: dst_index = dst_color * 16 + player * 48; player=0 for P1.
     * The player_color enum is { TERTIARY=0, SECONDARY=1, PRIMARY=2 }
     * (openomf src/formats/pilot.h:118-120), so palette indices 0..15
     * receive the TERTIARY (color_3) shade, 16..31 SECONDARY (color_2),
     * 32..47 PRIMARY (color_1). My first PoC pass had it backwards which
     * the user spotted (CRYSTAL's blue and grey appearing swapped). */
    dst_offsets[0] = 0  * 16 + 0 * 48;   /* TERTIARY  -> indices  0..15 */
    dst_offsets[1] = 1  * 16 + 0 * 48;   /* SECONDARY -> indices 16..31 */
    dst_offsets[2] = 2  * 16 + 0 * 48;   /* PRIMARY   -> indices 32..47 */
    src_offsets[0] = t->color_3 * 16;
    src_offsets[1] = t->color_2 * 16;
    src_offsets[2] = t->color_1 * 16;
    saved_color0 = pal->colors[0];        /* canon preserves slot 0    */
    for (i = 0; i < 3; i++) {
        memcpy(&pal->colors[dst_offsets[i]],
               &ap.palettes[0].colors[src_offsets[i]],
               16 * 3);                    /* 16 colors x RGB byte triple */
    }
    pal->colors[0] = saved_color0;
    printf("apply_pilot_altpal: pilot %d colors=(%d,%d,%d) applied to "
           "palette indices 0..47\n",
           pilot_id, t->color_1, t->color_2, t->color_3);
    altpal_free(&ap);
    return 0;
}

static int cmd_dump_all(const char *af_filename,
                        const char *bk_filename,
                        const char *out_dir) {
    sd_af_file af;
    sd_bk_file bk;
    int i, n_dumped;
    char move_dir[512];
    char index_path[512];
    FILE *ix;

    if (load_af_bk(&af, &bk, af_filename, bk_filename) != 0) return 1;
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }

    snprintf(index_path, sizeof(index_path), "%s/index.txt", out_dir);
    ix = fopen(index_path, "w");
    if (!ix) {
        fprintf(stderr, "fopen(%s) failed\n", index_path);
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    fprintf(ix, "# OMF HAR move index — generated by dump_har dump-all\n");
    fprintf(ix, "# AF: %s  BK: %s\n", af_filename, bk_filename);
    /* Sessione 9 health bars: export per-HAR base health + endurance so
     * run_pipeline_all.sh can emit them as #defines. endurance is a float
     * in OMF (~24.0 typical); scale by 1000 to retain precision in int. */
    fprintf(ix, "# AF_INFO health=%u endurance_x1000=%d\n",
            af.health, (int)(af.endurance * 1000.0f));
    fprintf(ix, "# move_id  category  sprite_count  damage  move_string  anim_string\n");
    fprintf(ix, "# (move_string '-' means empty/non-matchable)\n");

    n_dumped = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af.moves[i];
        const char *mstring;
        if (mv == NULL || mv->animation == NULL) continue;
        snprintf(move_dir, sizeof(move_dir), "%s/move_%02d", out_dir, i);
        mstring = mv->move_string[0] ? mv->move_string : "-";
        if (dump_move_inner(&af, bk.palettes[0], i, move_dir, 0) == 0) {
            fprintf(ix, "%d %u %u %u %s %s\n",
                    i, mv->category, mv->animation->sprite_count,
                    mv->damage_amount,
                    mstring,
                    mv->animation->anim_string);
            n_dumped++;
            printf("move %2d: %u sprites, cat=%u dmg=%u -> %s\n",
                   i, mv->animation->sprite_count, mv->category,
                   mv->damage_amount, move_dir);
        }
    }
    fclose(ix);
    printf("done: %d moves dumped, index at %s\n", n_dumped, index_path);

    sd_bk_free(&bk);
    sd_af_free(&af);
    return 0;
}

/* ===================================================================
 * §1.4 SENTINEL extraction — the real fix for per-pilot fight palettes.
 *
 * The raw ARENA BK palette has the three player-color zones (tertiary 1..15,
 * secondary 17..31, primary 33..47) as IDENTICAL grey ramps, so decoding a
 * sprite through it collapses all three zones into the same greys and 3it
 * merges them into one PLUT slot — the zone (hue) is lost irreversibly from
 * the neutral atlas (see [[project-pilot-plut-swap]], verified 2026-06-03).
 *
 * Sentinel extraction overwrites the 47 player-colorable indices (1..47) with
 * 47 DISTINCT vivid colors before sprite decode, so each OMF armor index maps
 * to its own PLUT entry. The colors are chosen on a 4x4x3 grid offset by +3 in
 * each 5-bit channel, so:
 *   - every index 1..47 -> a unique RGB555 triple (injective),
 *   - the +3 offset keeps them off the round multiples natural body colors sit
 *     on, minimizing collision with the real (index>47) body palette,
 *   - the 8-bit PNG value (c5<<3) round-trips through 3it's 8->5 quantization
 *     EXACTLY, so the atlas PLUT entry equals sentinel555(i) and gen-pluts can
 *     recover i exactly (no nearest-match ambiguity).
 * Index 0 (transparency) and indices 48..255 (fixed body colors) stay REAL.
 *
 * FEASIBILITY (measured 2026-06-03): a 3DO CODED CEL's PLUT caps at 32 colors.
 * Keeping all three zones distinct pushes 185/306 Jaguar frames (197/299 Thorn)
 * to 33..48 distinct colors — NOT codeable, forcing 8bpp/uncoded and ~2.4 MB
 * for two resident fighters vs a ~1.2 MB budget. So we only make the PRIMARY
 * zone (indices SENTINEL_FIRST..47) distinct and let secondary+tertiary stay on
 * the shared grey ramp (they collapse, as before). That caps every frame at
 * <=32 colors (measured max exactly 32, 0 frames over) -> codeable. Result:
 * pilot-colored PRIMARY armor + grey trim/body. Recolor touches ONLY 32..47,
 * which are unambiguous sentinels — the grey ramp (shared by armor sec/ter AND
 * body greys) is left alone, so body metal is never mis-tinted. */
#define SENTINEL_FIRST 32   /* primary-zone-only recolor (32..47); 1 = all zones */

/* Map OMF armor index i (1..47) to its sentinel color. Writes 8-bit RGB.
 * Encodes the index in base-4 digits across R/G/B, each digit lifted to a
 * distinct 5-bit slot. The per-channel offsets (3/5/1) are DIFFERENT so the
 * grey diagonal (d0==d1==d2) never produces a grey color — that avoids
 * colliding with the natural grey body colors in the sprite (e.g. body
 * indices that decode to (11,11,11)/(19,19,19)). Still injective: each i ->
 * a unique (R5,G5,B5), invertible by gen-pluts. The same mapping is used by
 * gen-pluts (via apply_sentinel_palette) so the recolor match is exact. */
static void sentinel_index_rgb(int i, uint8_t *r, uint8_t *g, uint8_t *b) {
    int d0 = i & 3;            /* 0..3 -> R */
    int d1 = (i >> 2) & 3;     /* 0..3 -> G */
    int d2 = (i >> 4) & 3;     /* 0..2 -> B (i<=47 => d2<=2) */
    *r = (uint8_t)((d0 * 8 + 3) << 3);   /* R5 in {3,11,19,27} */
    *g = (uint8_t)((d1 * 8 + 5) << 3);   /* G5 in {5,13,21,29} */
    *b = (uint8_t)((d2 * 8 + 1) << 3);   /* B5 in {1,9,17}     */
}

/* Overwrite the player-colorable zone (1..47) of a palette with sentinels.
 * Index 0 and 48..255 are left untouched. Returns the number of collisions
 * detected between a sentinel and a body color actually used (>47); 0 = clean.
 * `used_body` may be NULL to skip the check. */
static int apply_sentinel_palette(vga_palette *pal, const uint8_t *used_body) {
    int i, collisions = 0;
    for (i = SENTINEL_FIRST; i < 48; i++) {   /* primary zone only (see header) */
        uint8_t r, g, b;
        sentinel_index_rgb(i, &r, &g, &b);
        pal->colors[i].r = r;
        pal->colors[i].g = g;
        pal->colors[i].b = b;
    }
    if (used_body) {
        for (i = 48; i < 256; i++) {
            int j;
            if (!used_body[i]) continue;
            for (j = SENTINEL_FIRST; j < 48; j++) {
                uint8_t r, g, b;
                sentinel_index_rgb(j, &r, &g, &b);
                /* compare in 5-bit space (what 3it actually stores) */
                if ((r >> 3) == (pal->colors[i].r >> 3) &&
                    (g >> 3) == (pal->colors[i].g >> 3) &&
                    (b >> 3) == (pal->colors[i].b >> 3)) {
                    fprintf(stderr, "sentinel: COLLISION body idx %d == sentinel %d "
                            "(5bit %d,%d,%d)\n", i, j, r >> 3, g >> 3, b >> 3);
                    collisions++;
                }
            }
        }
    }
    return collisions;
}

/* Scan every sprite of the AF and mark which body indices (>=48) are used,
 * so apply_sentinel_palette can flag collisions. */
static void scan_used_body(sd_af_file *af, uint8_t *used_body /*[256]*/) {
    int i, j;
    memset(used_body, 0, 256);
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af->moves[i];
        sd_animation *ani;
        if (mv == NULL || mv->animation == NULL) continue;
        ani = mv->animation;
        for (j = 0; j < ani->sprite_count; j++) {
            sd_sprite *s = ani->sprites[j];
            sd_vga_image vimg;
            long n, p;
            if (s == NULL) continue;
            if (s->missing && (s->data == NULL || s->len == 0)) continue;
            if (sd_sprite_vga_decode(&vimg, s) != SD_SUCCESS) continue;
            n = (long)vimg.w * (long)vimg.h;
            for (p = 0; p < n; p++) used_body[(uint8_t)vimg.data[p]] = 1;
            sd_vga_image_free(&vimg);
        }
    }
}

/* Like cmd_dump_all but applies the sentinel palette first. The resulting PNGs
 * carry zone-distinct armor colors so the rebuilt neutral atlas keeps each OMF
 * index separable -> gen-pluts can recolor per pilot via the PLUT swap. */
static int cmd_dump_all_sentinel(const char *af_filename,
                                 const char *bk_filename,
                                 const char *out_dir) {
    sd_af_file af;
    sd_bk_file bk;
    int i, n_dumped, collisions;
    uint8_t used_body[256];
    char move_dir[512];
    char index_path[512];
    FILE *ix;

    if (load_af_bk(&af, &bk, af_filename, bk_filename) != 0) return 1;

    scan_used_body(&af, used_body);
    collisions = apply_sentinel_palette(bk.palettes[0], used_body);
    if (collisions > 0) {
        fprintf(stderr, "sentinel: %d collisions — recolor may corrupt those "
                "body pixels; adjust the sentinel scheme.\n", collisions);
        /* not fatal: report and continue so we can inspect */
    } else {
        printf("sentinel: palette clean (no body/sentinel collisions)\n");
    }

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    snprintf(index_path, sizeof(index_path), "%s/index.txt", out_dir);
    ix = fopen(index_path, "w");
    if (!ix) {
        fprintf(stderr, "fopen(%s) failed\n", index_path);
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    fprintf(ix, "# OMF HAR move index — SENTINEL (zone-distinct) variant\n");
    fprintf(ix, "# AF: %s  BK: %s\n", af_filename, bk_filename);
    fprintf(ix, "# AF_INFO health=%u endurance_x1000=%d\n",
            af.health, (int)(af.endurance * 1000.0f));
    fprintf(ix, "# move_id  category  sprite_count  damage  move_string  anim_string\n");

    n_dumped = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af.moves[i];
        const char *mstring;
        if (mv == NULL || mv->animation == NULL) continue;
        snprintf(move_dir, sizeof(move_dir), "%s/move_%02d", out_dir, i);
        mstring = mv->move_string[0] ? mv->move_string : "-";
        if (dump_move_inner(&af, bk.palettes[0], i, move_dir, 0) == 0) {
            fprintf(ix, "%d %u %u %u %s %s\n",
                    i, mv->category, mv->animation->sprite_count,
                    mv->damage_amount, mstring, mv->animation->anim_string);
            n_dumped++;
        }
    }
    fclose(ix);
    printf("done: %d moves dumped (sentinel), index at %s\n", n_dumped, index_path);

    sd_bk_free(&bk);
    sd_af_free(&af);
    return 0;
}

static int cmd_bg(const char *bk_filename, const char *out_dir) {
    sd_bk_file bk;
    path p;
    int rc;
    sd_rgba_image img;
    char png_name[512];

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        return 1;
    }

    path_from_c(&p, bk_filename);
    if (sd_bk_create(&bk) != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_create failed\n"); return 1;
    }
    rc = sd_bk_load(&bk, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_bk_load(%s) failed: %d (%s)\n",
                bk_filename, rc, sd_get_error(rc));
        sd_bk_free(&bk); return 1;
    }
    if (bk.background == NULL) {
        fprintf(stderr, "BK has no background image\n");
        sd_bk_free(&bk); return 1;
    }
    if (bk.palettes[0] == NULL) {
        fprintf(stderr, "BK has no palette[0]\n");
        sd_bk_free(&bk); return 1;
    }

    rc = sd_vga_image_decode(&img, bk.background, bk.palettes[0]);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_vga_image_decode failed: %d\n", rc);
        sd_bk_free(&bk); return 1;
    }

    snprintf(png_name, sizeof(png_name), "%s/bg.png", out_dir);
    if (write_rgba_png(png_name, (int)img.w, (int)img.h, (unsigned char *)img.data) != 0) {
        fprintf(stderr, "write_rgba_png(%s) failed\n", png_name);
        sd_rgba_image_free(&img);
        sd_bk_free(&bk); return 1;
    }
    printf("wrote %s (%ux%u from %s)\n", png_name, img.w, img.h, bk_filename);

    sd_rgba_image_free(&img);
    sd_bk_free(&bk);
    return 0;
}

/* Variant of cmd_dump_all that applies pilot color altpal injection to
 * the BK palette before extracting sprites. Produces pilot-specific
 * HAR CEL sets (e.g. Jaguar with CRYSTAL's blue/tan/grey colors). */
static int cmd_dump_all_pilot(const char *af_filename,
                              const char *bk_filename,
                              const char *altpals_filename,
                              int pilot_id,
                              const char *out_dir) {
    sd_af_file af;
    sd_bk_file bk;
    int i, n_dumped;
    char move_dir[512];
    char index_path[512];
    FILE *ix;

    if (load_af_bk(&af, &bk, af_filename, bk_filename) != 0) return 1;

    /* Inject pilot's altpal into bk.palettes[0] BEFORE any sprite decode. */
    if (apply_pilot_altpal(bk.palettes[0], altpals_filename, pilot_id) != 0) {
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed: %s\n", out_dir, strerror(errno));
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    snprintf(index_path, sizeof(index_path), "%s/index.txt", out_dir);
    ix = fopen(index_path, "w");
    if (!ix) {
        fprintf(stderr, "fopen(%s) failed\n", index_path);
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    fprintf(ix, "# OMF HAR move index -- pilot-altpal variant\n");
    fprintf(ix, "# AF: %s  BK: %s  ALTPALS: %s  pilot: %d\n",
            af_filename, bk_filename, altpals_filename, pilot_id);
    fprintf(ix, "# AF_INFO health=%u endurance_x1000=%d\n",
            af.health, (int)(af.endurance * 1000.0f));
    fprintf(ix, "# move_id  category  sprite_count  damage  move_string  anim_string\n");

    n_dumped = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af.moves[i];
        const char *mstring;
        if (mv == NULL || mv->animation == NULL) continue;
        snprintf(move_dir, sizeof(move_dir), "%s/move_%02d", out_dir, i);
        mstring = mv->move_string[0] ? mv->move_string : "-";
        if (dump_move_inner(&af, bk.palettes[0], i, move_dir, 0) == 0) {
            fprintf(ix, "%d %u %u %u %s %s\n",
                    i, mv->category, mv->animation->sprite_count,
                    mv->damage_amount,
                    mstring, mv->animation->anim_string);
            n_dumped++;
            printf("move %2d: %u sprites dmg=%u -> %s\n",
                   i, mv->animation->sprite_count, mv->damage_amount, move_dir);
        }
    }
    fclose(ix);
    printf("done: %d moves dumped (pilot %d), index at %s\n",
           n_dumped, pilot_id, index_path);

    sd_bk_free(&bk);
    sd_af_free(&af);
    return 0;
}

/* ===================================================================
 * §1.4 per-pilot fight palettes (MILESTONE_1V1): gen-pluts
 *
 * Emits one ".PAL" blob per pilot (0..9) that recolors the NEUTRAL atlas's
 * per-frame PLUTs with that pilot's altpal shades. The pixel data is identical
 * across pilots, so at runtime we just repoint each CCB's ccb_PLUTPtr at the
 * pilot's recolored PLUT blob (see atlas.c AtlasApplyPilotPlut). This keeps ONE
 * atlas per HAR + tiny per-pilot PLUT blobs (~9 KB each) instead of baking 10
 * full atlas variants per HAR (~5 MB of ISO).
 *
 * Provenance: each HAR frame uses <=16 colors (measured), so within a frame the
 * PLUT colors are distinct and we can map each PLUT entry back to its OMF
 * palette index by NEAREST-matching (in 5-bit RGB space) against the base BK
 * palette restricted to the indices that frame actually uses (recovered via
 * sd_sprite_vga_decode). Entries whose index lands in the recolorable zone 1..47
 * get the pilot color; index 0 (transparency) and body/fixed colors (>47) are
 * copied through unchanged. Nearest-match (not exact) tolerates 3it's 8->5 bit
 * quantization rounding.
 *
 * Frame walk MIRRORS build_har_atlas.sh (dump-all move order, non-missing
 * sprites) so .PAL frame k lines up 1:1 with atlas frame k; a frame-count
 * mismatch vs the .ATL header is a hard error.
 * =================================================================== */

#define ATL_NF 16   /* u32 fields per atlas frame-table entry */
/* Atlas frame-table field indices (must match host_tools/build_atlas.c +
 * src/atlas.c). We only need the PLUT offset/length here. */
enum { F_PDOFF = 0, F_PDLEN, F_PLOFF, F_PLLEN };

static uint32_t gh_rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* vga_color (8-bit RGB) -> 3DO PLUT entry (15-bit 0RRRRRGGGGGBBBBB). */
static uint16_t gh_rgb555(unsigned char r, unsigned char g, unsigned char b) {
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

/* Squared distance between two RGB555 colors in 5-bit channel space. */
static int gh_dist555(uint16_t a, uint16_t c) {
    int ar = (a >> 10) & 31, ag = (a >> 5) & 31, ab = a & 31;
    int cr = (c >> 10) & 31, cg = (c >> 5) & 31, cb = c & 31;
    int dr = ar - cr, dg = ag - cg, db = ab - cb;
    return dr * dr + dg * dg + db * db;
}

static uint8_t *gh_slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    if (!f) { fprintf(stderr, "gen-pluts: open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(*len);
    if (buf && fread(buf, 1, *len, f) != (size_t)*len) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* Walk the atlas frame order (mirrors build_har_atlas.sh) and call `fn` for each
 * non-missing sprite with its decoded VGA-index image + the atlas frame index.
 * Returns the number of frames visited, or -1 on a decode hiccup that desyncs. */
static int gh_for_each_atlas_frame(
        sd_af_file *af,
        void (*fn)(int atlas_k, const sd_vga_image *vimg, void *ud),
        void *ud) {
    int i, j, k = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af->moves[i];
        sd_animation *ani;
        if (mv == NULL || mv->animation == NULL) continue;
        ani = mv->animation;
        for (j = 0; j < ani->sprite_count; j++) {
            sd_sprite *s = ani->sprites[j];
            sd_vga_image vimg;
            if (s == NULL) continue;
            if (s->missing && (s->data == NULL || s->len == 0)) continue;
            if (sd_sprite_vga_decode(&vimg, s) != SD_SUCCESS) {
                fprintf(stderr, "gen-pluts: vga_decode move=%d sprite=%d failed\n",
                        i, j);
                /* still advance k so alignment is preserved; pass NULL image */
                fn(k, NULL, ud);
            } else {
                fn(k, &vimg, ud);
                sd_vga_image_free(&vimg);
            }
            k++;
        }
    }
    return k;
}

/* Per-frame recolor context shared across the walk callback. */
typedef struct {
    const uint8_t  *table;       /* atlas frame table (nframes x ATL_NF u32)  */
    int             nframes;
    uint8_t        *pal_blob;    /* recolored PLUT blob being built (plut_total) */
    const vga_palette *base;     /* neutral BK palette                         */
    const vga_palette *pilot;    /* pilot-recolored palette                    */
    uint16_t        base555[256];/* base index -> RGB555 (precomputed)         */
    uint16_t        pilot555[256];
} gh_recolor_ctx;

/* Max 5-bit squared distance for an atlas PLUT entry to still be considered
 * "this primary sentinel". Sentinels are vivid and >2 (5-bit, per-channel) away
 * from every grey/body color (verified by the sentinel collision scan), and
 * 3it round-trips them exactly, so a tight bound recolors exactly the primary
 * entries and never bleeds onto grey trim or body metal. */
#define SENTINEL_MATCH_MAXD 4   /* squared 5-bit distance (~1 channel slop)     */

static void gh_recolor_frame(int k, const sd_vga_image *vimg, void *ud) {
    gh_recolor_ctx *cx = (gh_recolor_ctx *)ud;
    const uint8_t *e;
    uint32_t ploff, pllen;
    int nent, q;

    (void)vimg;   /* provenance no longer needed: primary sentinels are unique  */
    if (k >= cx->nframes) return;          /* alignment guard (caller errors)  */
    e = cx->table + (long)k * ATL_NF * 4;
    ploff = gh_rd32be(e + F_PLOFF * 4);
    pllen = gh_rd32be(e + F_PLLEN * 4);
    nent  = (int)(pllen / 2);

    for (q = 0; q < nent; q++) {
        uint8_t *dst = cx->pal_blob + ploff + (long)q * 2;
        uint16_t entry = (uint16_t)((dst[0] << 8) | dst[1]);   /* BE in blob */
        int best_idx = -1, best_d = 0x7fffffff, idx;
        /* Match DIRECTLY against the PRIMARY sentinel colors (SENTINEL_FIRST..47)
         * only — not the whole used-index set. A grey/body entry is far from all
         * sentinels (> bound) and is left untouched, so it stays grey. This is
         * what stops the prior grey-bleed (Thorn body trim turning red). */
        for (idx = SENTINEL_FIRST; idx <= 47; idx++) {
            int d = gh_dist555(entry, cx->base555[idx]);
            if (d < best_d) { best_d = d; best_idx = idx; }
        }
        if (best_idx >= SENTINEL_FIRST && best_d <= SENTINEL_MATCH_MAXD) {
            uint16_t rc = cx->pilot555[best_idx];
            dst[0] = (uint8_t)(rc >> 8); dst[1] = (uint8_t)(rc & 0xff);
        }
    }
}

static int cmd_gen_pluts(const char *af_filename,
                         const char *bk_filename,
                         const char *altpals_filename,
                         const char *atl_filename,
                         const char *out_prefix) {
    sd_af_file af;
    sd_bk_file bk;
    uint8_t *atl;
    long atl_len;
    uint32_t magic, nframes, plut_total;
    const uint8_t *table;
    vga_palette base;        /* SENTINEL palette — matches the atlas PLUT entries */
    vga_palette real_base;   /* real BK palette — source for pilot recolor        */
    int pilot, i, visited;

    if (load_af_bk(&af, &bk, af_filename, bk_filename) != 0) return 1;

    /* The neutral atlas was extracted with the SENTINEL palette (zone-distinct),
     * so every armor PLUT entry equals sentinel555(idx). We match against that.
     * The pilot recolor source is the REAL BK palette (apply_pilot_altpal). */
    real_base = *bk.palettes[0];
    base = real_base;
    apply_sentinel_palette(&base, NULL);   /* 1..47 -> sentinels; 0,48+ stay real */

    atl = gh_slurp(atl_filename, &atl_len);
    if (!atl || atl_len < 16) {
        fprintf(stderr, "gen-pluts: bad .ATL %s\n", atl_filename);
        sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    magic      = gh_rd32be(atl + 0);
    nframes    = gh_rd32be(atl + 4);
    plut_total = gh_rd32be(atl + 8);
    if (magic != 0x41544C31u) {   /* "ATL1" */
        fprintf(stderr, "gen-pluts: %s bad magic 0x%08x\n", atl_filename, magic);
        free(atl); sd_bk_free(&bk); sd_af_free(&af); return 1;
    }
    table = atl + 16;
    /* The recolored PLUT blob source = the atlas's own neutral PLUT blob, which
     * sits right after the frame table. */
    {
        const uint8_t *neutral_blob = table + (long)nframes * ATL_NF * 4;

        for (pilot = 0; pilot < 10; pilot++) {
            gh_recolor_ctx cx;
            vga_palette pilpal = real_base; /* real BK copy, pilot-recolored below */
            char out_path[1024];
            FILE *of;

            if (apply_pilot_altpal(&pilpal, altpals_filename, pilot) != 0) {
                fprintf(stderr, "gen-pluts: altpal pilot %d failed\n", pilot);
                free(atl); sd_bk_free(&bk); sd_af_free(&af); return 1;
            }

            cx.table   = table;
            cx.nframes = (int)nframes;
            cx.base    = &base;
            cx.pilot   = &pilpal;
            cx.pal_blob = (uint8_t *)malloc(plut_total);
            if (!cx.pal_blob) { fprintf(stderr, "gen-pluts: oom\n");
                free(atl); sd_bk_free(&bk); sd_af_free(&af); return 1; }
            memcpy(cx.pal_blob, neutral_blob, plut_total);   /* start = neutral */
            for (i = 0; i < 256; i++) {
                cx.base555[i]  = gh_rgb555(base.colors[i].r,  base.colors[i].g,  base.colors[i].b);
                cx.pilot555[i] = gh_rgb555(pilpal.colors[i].r, pilpal.colors[i].g, pilpal.colors[i].b);
            }

            visited = gh_for_each_atlas_frame(&af, gh_recolor_frame, &cx);
            if (visited != (int)nframes) {
                fprintf(stderr, "gen-pluts: FRAME MISMATCH walk=%d atlas=%u "
                        "(atlas walk desync — .PAL would be misaligned)\n",
                        visited, nframes);
                free(cx.pal_blob); free(atl);
                sd_bk_free(&bk); sd_af_free(&af); return 1;
            }

            /* Write the .PAL: 16-byte header + recolored PLUT blob.
             *   "PPAL" | plut_total | frame_count | 0 */
            snprintf(out_path, sizeof(out_path), "%s_%d.PAL", out_prefix, pilot);
            of = fopen(out_path, "wb");
            if (!of) { fprintf(stderr, "gen-pluts: create %s failed\n", out_path);
                free(cx.pal_blob); free(atl);
                sd_bk_free(&bk); sd_af_free(&af); return 1; }
            {
                uint8_t hdr[16];
                hdr[0]='P';hdr[1]='P';hdr[2]='A';hdr[3]='L';
                hdr[4]=(uint8_t)(plut_total>>24);hdr[5]=(uint8_t)(plut_total>>16);
                hdr[6]=(uint8_t)(plut_total>>8); hdr[7]=(uint8_t)(plut_total);
                hdr[8]=(uint8_t)(nframes>>24);hdr[9]=(uint8_t)(nframes>>16);
                hdr[10]=(uint8_t)(nframes>>8);hdr[11]=(uint8_t)(nframes);
                hdr[12]=hdr[13]=hdr[14]=hdr[15]=0;
                fwrite(hdr, 1, 16, of);
            }
            fwrite(cx.pal_blob, 1, plut_total, of);
            fclose(of);
            printf("gen-pluts: pilot %d -> %s (%u PLUT bytes, %u frames)\n",
                   pilot, out_path, plut_total, nframes);
            free(cx.pal_blob);
        }
    }

    free(atl);
    sd_bk_free(&bk);
    sd_af_free(&af);
    return 0;
}

/* Parse a move's footer_string (the hit-reaction anim string) and return the
 * first "s" (sound) tag value = the impact sound's AF soundtable param, or -1
 * if the footer has no sound. This is the canonical "clang" played when the
 * move connects (openomf har.c:972 sets the target's anim to the footer
 * string, and player.c:557 plays its "s" tag as the hit sound). We use the
 * canonical sd_script parser (already linked) rather than fragile string
 * scanning, so e.g. the 's' inside "bs255" is never mistaken for a sound. */
/* Parse a move's footer (hit-reaction) string and extract, for the bounded
 * canon-derived knockback (see [[project-harc-port-decision]]):
 *   *snd   = first "s" sound tag = the impact "clang" (param into AF soundtable)
 *   *dx    = NET horizontal knockback in OMF px = sum over footer frames of
 *            (x+ value) - (x- value). Positive = pushed in the move's facing
 *            direction; the runtime applies fighter facing. (player.c:258-260)
 *   *ticks = total footer duration in ticks (sum of per-frame tick_len) =
 *            how long the victim's recoil/slide lasts.
 * All via the canonical sd_script parser. Any out-param may be NULL. */
static void footer_info(const char *footer, int *snd, int *dx, int *ticks) {
    sd_script sc;
    int i;
    int found_snd = -1, sum_dx = 0, sum_ticks = 0;
    if (snd) *snd = -1;
    if (dx) *dx = 0;
    if (ticks) *ticks = 0;
    if (footer == NULL || footer[0] == 0) return;
    if (sd_script_create(&sc) != SD_SUCCESS) return;
    if (sd_script_decode(&sc, footer, NULL) == SD_SUCCESS) {
        for (i = 0; ; i++) {
            const sd_script_frame *f = sd_script_get_frame(&sc, i);
            if (f == NULL) break;
            if (found_snd < 0 && sd_script_isset(f, "s")) {
                found_snd = sd_script_get(f, "s");
            }
            if (sd_script_isset(f, "x+")) sum_dx += sd_script_get(f, "x+");
            if (sd_script_isset(f, "x-")) sum_dx -= sd_script_get(f, "x-");
            sum_ticks += f->tick_len;
        }
    }
    sd_script_free(&sc);
    if (snd)   *snd = found_snd;
    if (dx)    *dx = sum_dx;
    if (ticks) *ticks = sum_ticks;
}

/* Emit per-move af_move SCALAR metadata for the openomf logic port
 * (is_in_range / calc_damage_and_stun / blocking / chaining). Reads the AF
 * ONLY — no CEL/render data, so it never perturbs the baked sprite tables or
 * the ISO. Field mapping mirrors openomf resources/af_move.c af_move_create():
 *   next_move    = sd_move.next_anim_id
 *   successor_id = sd_move.successor_id
 *   category     = sd_move.category
 *   damage       = sd_move.damage_amount
 *   block_damage = sd_move.block_damage
 *   block_stun   = sd_move.block_stun
 *   throw_dur    = sd_move.throw_duration
 *   points       = sd_move.points * 400   (af_move stores the *400 value)
 * The emitted array uses har_move_meta_t (declared in src/har_packs.h). */
static int cmd_dump_meta(const char *af_filename, const char *out_header,
                         const char *name) {
    sd_af_file af;
    path p;
    int rc, i, n;
    FILE *mf;

    path_from_c(&p, af_filename);
    if (sd_af_create(&af) != SD_SUCCESS) {
        fprintf(stderr, "sd_af_create failed\n");
        return 1;
    }
    rc = sd_af_load(&af, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_af_load(%s) failed: %d (%s)\n",
                af_filename, rc, sd_get_error(rc));
        sd_af_free(&af);
        return 1;
    }

    mf = fopen(out_header, "w");
    if (mf == NULL) {
        fprintf(stderr, "fopen(%s) failed: %s\n", out_header, strerror(errno));
        sd_af_free(&af);
        return 1;
    }

    fprintf(mf, "/* Auto-generated by `dump_har meta` from %s. DO NOT EDIT.\n",
            af_filename);
    fprintf(mf, " * Per-move af_move scalar metadata for the openomf logic\n");
    fprintf(mf, " * port. Read directly from the AF (no CEL/render data).\n");
    fprintf(mf, " * Mapping mirrors openomf af_move_create(). */\n");
    fprintf(mf, "/* id  category  damage  hit_sound  knockback_x  recoil_ticks  next  succ  blk_dmg  blk_stun  thr_dur  points */\n");
    fprintf(mf, "static const har_move_meta_t %s_move_meta[] = {\n", name);

    n = 0;
    for (i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *mv = af.moves[i];
        int hit_sound, kb_x, rc_ticks;
        if (mv == NULL || mv->animation == NULL) continue;
        footer_info(mv->footer_string, &hit_sound, &kb_x, &rc_ticks);
        fprintf(mf, "    { %d, %u, %u, %d, %d, %d, %u, %u, %u, %u, %u, %d },\n",
                i,
                mv->category,
                mv->damage_amount,
                hit_sound,
                kb_x,
                rc_ticks,
                mv->next_anim_id,
                mv->successor_id,
                mv->block_damage,
                mv->block_stun,
                mv->throw_duration,
                (int)mv->points * 400);
        n++;
    }
    fprintf(mf, "};\n");
    {
        /* uppercase name for the count macro */
        char up[64];
        int k;
        for (k = 0; name[k] && k < 63; k++) {
            char c = name[k];
            up[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        up[k] = 0;
        fprintf(mf, "#define %s_MOVE_META_COUNT (%d)\n", up, n);

        /* ---- Per-move RECOIL TRACKS (the victim's hit reaction) ----
         * openomf har_take_damage() sets the victim to ANIM_DAMAGE and then
         * plays the ATTACKER move's footer_string as a custom string over it
         * (har.c:980/1032). Each footer frame selects an ANIM_DAMAGE sprite,
         * holds tick_len ticks, and applies x+/x- as a ONE-SHOT position
         * delta (player.c:257-260,446). The delta is `value * direction`; the
         * victim faces the attacker, so for our fixed P2-on-the-right
         * (OBJECT_FACE_LEFT = -1) layout the world delta is (x-) - (x+).
         * We bake that per-frame so the 3DO can replay the AUTHORED recoil
         * trajectory verbatim instead of synthesising one. */
        for (i = 0; i < MAX_AF_MOVES; i++) {
            sd_move *mv = af.moves[i];
            sd_script sc;
            int fi;
            if (mv == NULL || mv->animation == NULL) continue;
            if (mv->footer_string[0] == 0) continue;
            if (sd_script_create(&sc) != SD_SUCCESS) continue;
            if (sd_script_decode(&sc, mv->footer_string, NULL) != SD_SUCCESS) {
                sd_script_free(&sc);
                continue;
            }
            fprintf(mf, "static const har_recoil_frame_t %s_recoil_f%d[] = {\n",
                    name, i);
            for (fi = 0; ; fi++) {
                const sd_script_frame *f = sd_script_get_frame(&sc, fi);
                int xp, xm, ym, is_v;
                if (f == NULL) break;
                xp = sd_script_isset(f, "x+") ? sd_script_get(f, "x+") : 0;
                xm = sd_script_isset(f, "x-") ? sd_script_get(f, "x-") : 0;
                /* Vertical knockback: player.c:251 sets trans_y = (y-)*-1 ONLY
                 * from the 'y-' tag (y+ is not applied as trans_y upstream), so
                 * dy = -(y-). Negative = UP. Combined with 'v' this is the
                 * LAUNCHER pop-up (e.g. Jaguar P1/K1 footers 'vx-6y-9'). */
                ym = sd_script_isset(f, "y-") ? sd_script_get(f, "y-") : 0;
                /* 'v' tag => x/y are VELOCITIES (px/tick) integrated with
                 * friction+gravity, not one-shot position deltas (player.c:440). */
                is_v = sd_script_isset(f, "v") ? 1 : 0;
                fprintf(mf, "    { %d, %d, %d, %d, %d },  /* %s */\n",
                        f->sprite, f->tick_len, xm - xp, -ym, is_v,
                        is_v ? "vel" : "pos");
            }
            fprintf(mf, "};\n");
            sd_script_free(&sc);
        }
        fprintf(mf, "static const har_recoil_move_t %s_recoil[] = {\n", name);
        n = 0;
        for (i = 0; i < MAX_AF_MOVES; i++) {
            sd_move *mv = af.moves[i];
            sd_script sc;
            int fc = 0, fi;
            if (mv == NULL || mv->animation == NULL) continue;
            if (mv->footer_string[0] == 0) continue;
            if (sd_script_create(&sc) != SD_SUCCESS) continue;
            if (sd_script_decode(&sc, mv->footer_string, NULL) != SD_SUCCESS) {
                sd_script_free(&sc);
                continue;
            }
            for (fi = 0; sd_script_get_frame(&sc, fi) != NULL; fi++) fc++;
            sd_script_free(&sc);
            fprintf(mf, "    { %d, %d, %s_recoil_f%d },\n", i, fc, name, i);
            n++;
        }
        fprintf(mf, "};\n");
        fprintf(mf, "#define %s_RECOIL_COUNT (%d)\n", up, n);
    }
    fclose(mf);
    printf("meta: %d moves -> %s\n", n, out_header);

    sd_af_free(&af);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s list     <FIGHTR*.AF>\n"
        "  %s dump     <FIGHTR*.AF> <ARENA*.BK> <move_id> <out_dir>\n"
        "  %s dump-all <FIGHTR*.AF> <ARENA*.BK> <out_dir>\n"
        "  %s dump-all-pilot <FIGHTR*.AF> <ARENA*.BK> <ALTPALS.DAT> <pilot_id 0..9> <out_dir>\n"
        "  %s dump-all-sentinel <FIGHTR*.AF> <ARENA*.BK> <out_dir>\n"
        "  %s gen-pluts <FIGHTR*.AF> <ARENA*.BK> <ALTPALS.DAT> <neutral.ATL> <out_prefix>\n"
        "  %s meta     <FIGHTR*.AF> <out_header.h> <name>\n"
        "  %s bg       <ARENA*.BK> <out_dir>\n"
        "  %s sounds   <SOUNDS.DAT> <FIGHTR*.AF> <out_dir>\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "list") == 0) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return cmd_list(argv[2]);
    }
    if (strcmp(argv[1], "dump") == 0) {
        if (argc != 6) { usage(argv[0]); return 1; }
        return cmd_dump(argv[2], argv[3], atoi(argv[4]), argv[5]);
    }
    if (strcmp(argv[1], "dump-all") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        return cmd_dump_all(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "gen-pluts") == 0) {
        if (argc != 7) { usage(argv[0]); return 1; }
        return cmd_gen_pluts(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }
    if (strcmp(argv[1], "dump-all-pilot") == 0) {
        if (argc != 7) { usage(argv[0]); return 1; }
        return cmd_dump_all_pilot(argv[2], argv[3], argv[4],
                                  atoi(argv[5]), argv[6]);
    }
    if (strcmp(argv[1], "dump-all-sentinel") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        return cmd_dump_all_sentinel(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "meta") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        return cmd_dump_meta(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "bg") == 0) {
        if (argc != 4) { usage(argv[0]); return 1; }
        return cmd_bg(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "sounds") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        return cmd_sounds(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "sound") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        return cmd_sound_by_id(argv[2], argv[3], argc - 4, argv + 4);
    }
    usage(argv[0]);
    return 1;
}
