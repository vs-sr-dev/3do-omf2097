/*
 * build_atlas.c — consolidate a HAR moveset's per-frame 3DO CEL files into ONE
 * ".ATL" atlas, so the runtime loads the whole moveset with a single AllocMem
 * for pixel data instead of 306 page-rounded LoadCel allocations.
 *
 * WHY: measured 2026-06-02, one Jaguar moveset costs 270 KB on disk but 707 KB
 * in 3DO memory — a 2.6x blow-up from per-CEL page rounding (306 tiny separate
 * allocations, each rounded up to a memory-protect page). The real pixel data
 * (sum of PDAT chunks) is only ~231 KB; an atlas (PDAT blob + one shared PLUT +
 * a CCB array) is ~262 KB, saving ~444 KB/moveset. Two full symmetric movesets
 * then fit the DRAM+VRAM budget with ~640 KB to spare (see memory
 * project-dram-budget-s1-1).
 *
 * 3DO CEL file = a sequence of IFF-like chunks, each [id:4][size:4(BE,incl
 * header)][data]. A single-cel file is 'CCB ' then 'PDAT' then 'PLUT'. The CCB
 * chunk's serialized layout (mapped from a hexdump, offsets relative to the
 * chunk start C):
 *   C+12 Flags  C+36 hdx  C+40 hdy  C+44 vdx  C+48 vdy
 *   C+52 hddx   C+56 hddy C+60 PIXC C+64 PRE0 C+68 PRE1 C+72 W  C+76 H
 * All values are big-endian (3DO is big-endian); we copy them through verbatim.
 * Per-frame HDX/VDY MUST be preserved (3it packed-stride gotcha,
 * project-3it-cel-hdx-vdy).
 *
 * NOTE: 3it --find-smallest gives each CEL its OWN minimal PLUT (different
 * colors AND bpp per sprite), so PLUTs canNOT be shared — each frame keeps its
 * own. They are tiny (8-44 B), packed into a PLUT blob (~9 KB for a moveset).
 *
 * ATL format (all u32 big-endian, so the 3DO reads CCB fields directly):
 *   "ATL1" | frame_count | plut_total | pdat_total
 *   frame table: frame_count x 16 u32 =
 *     pdat_off, pdat_len, plut_off, plut_len, flags, hdx, hdy, vdx, vdy,
 *     hddx, hddy, pixc, pre0, pre1, width, height
 *   PLUT blob (plut_total) — every frame's PLUT chunk data, concatenated
 *   PDAT blob (pdat_total) — every frame's PDAT chunk data, concatenated
 *
 * Usage: build_atlas <out.atl> <frame0.CEL> <frame1.CEL> ...
 *        build_atlas <out.atl> @<manifest>   (manifest = one CEL path per line)
 *   (paths must be given in the runtime's non-missing frame iteration order)
 *
 * Build/run on WSL: gcc -O2 -Wall build_atlas.c -o build/build_atlas
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ATL_FIELDS 16   /* u32 per frame-table entry */

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void wr32be(FILE *f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)(v);
    fwrite(b, 1, 4, f);
}

/* Read a whole file into a malloc'd buffer; *len gets the size. */
static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    if (!f) { fprintf(stderr, "open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(*len);
    if (buf && fread(buf, 1, *len, f) != (size_t)*len) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* Locate a chunk by 4-byte id; returns pointer to the chunk start (the id) and
 * its data length (size-8) via *dlen, or NULL if absent. */
static const uint8_t *find_chunk(const uint8_t *buf, long len,
                                 const char *id, uint32_t *dlen)
{
    long o = 0;
    while (o + 8 <= len) {
        uint32_t sz = rd32be(buf + o + 4);
        if (sz < 8 || o + (long)sz > len) break;
        if (memcmp(buf + o, id, 4) == 0) { *dlen = sz - 8; return buf + o; }
        o += sz;
    }
    return NULL;
}

/* Per-frame metadata captured from each CEL's CCB chunk. */
typedef struct {
    uint32_t fields[ATL_FIELDS];  /* pdat_off..height (see enum)         */
    const uint8_t *pdat;          /* -> PDAT data in the slurped buffer  */
    uint32_t pdat_len;
    const uint8_t *plut;          /* -> PLUT data in the slurped buffer  */
    uint32_t plut_len;
} frame_t;

enum { F_PDOFF, F_PDLEN, F_PLOFF, F_PLLEN, F_FLAGS, F_HDX, F_HDY, F_VDX,
       F_VDY, F_HDDX, F_HDDY, F_PIXC, F_PRE0, F_PRE1, F_W, F_H };

int main(int argc, char **argv)
{
    int nframes, i;
    frame_t *fr;
    uint32_t plut_total = 0;
    uint32_t pdat_total = 0;
    uint8_t **bufs;
    long *lens;
    FILE *out;
    char **paths = NULL;          /* resolved cel path list (manifest or argv) */
    char *manifest = NULL;        /* malloc'd manifest text, freed at exit     */

    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.atl> <cel> [cel...]\n", argv[0]);
        fprintf(stderr, "       %s <out.atl> @<manifest>\n", argv[0]);
        return 2;
    }

    if (argc == 3 && argv[2][0] == '@') {
        /* Manifest mode: one CEL path per line (robust vs 300+ argv). */
        long mlen;
        char *p, *line;
        int cap = 0;
        manifest = (char *)slurp(argv[2] + 1, &mlen);
        if (!manifest) { fprintf(stderr, "read manifest %s failed\n", argv[2] + 1); return 1; }
        nframes = 0;
        for (p = manifest; p < manifest + mlen; p++) if (*p == '\n') nframes++;
        cap = nframes + 1;
        paths = (char **)calloc(cap, sizeof(char *));
        nframes = 0;
        line = manifest;
        for (p = manifest; p < manifest + mlen; p++) {
            if (*p == '\n' || *p == '\r') {
                *p = '\0';
                if (line < p && *line) paths[nframes++] = line;
                line = p + 1;
            }
        }
        if (line < manifest + mlen && *line) {
            manifest[mlen ? mlen : 0] = '\0';
            paths[nframes++] = line;
        }
    } else {
        nframes = argc - 2;
        paths = (char **)calloc(nframes, sizeof(char *));
        for (i = 0; i < nframes; i++) paths[i] = argv[2 + i];
    }

    fr   = (frame_t *)calloc(nframes, sizeof(frame_t));
    bufs = (uint8_t **)calloc(nframes, sizeof(uint8_t *));
    lens = (long *)calloc(nframes, sizeof(long));
    if (!fr || !bufs || !lens) { fprintf(stderr, "oom\n"); return 1; }

    for (i = 0; i < nframes; i++) {
        const char *path = paths[i];
        const uint8_t *ccb, *pdat, *cel_plut;
        uint32_t clen, pdlen, pllen;
        long len;
        uint8_t *buf = slurp(path, &len);
        if (!buf) return 1;
        bufs[i] = buf; lens[i] = len;

        ccb  = find_chunk(buf, len, "CCB ", &clen);
        pdat = find_chunk(buf, len, "PDAT", &pdlen);
        if (!ccb || !pdat) {
            fprintf(stderr, "%s: missing CCB/PDAT chunk\n", path);
            return 1;
        }
        /* Pull the CCB fields straight through (already big-endian). */
        fr[i].fields[F_FLAGS] = rd32be(ccb + 12);
        fr[i].fields[F_HDX]   = rd32be(ccb + 36);
        fr[i].fields[F_HDY]   = rd32be(ccb + 40);
        fr[i].fields[F_VDX]   = rd32be(ccb + 44);
        fr[i].fields[F_VDY]   = rd32be(ccb + 48);
        fr[i].fields[F_HDDX]  = rd32be(ccb + 52);
        fr[i].fields[F_HDDY]  = rd32be(ccb + 56);
        fr[i].fields[F_PIXC]  = rd32be(ccb + 60);
        fr[i].fields[F_PRE0]  = rd32be(ccb + 64);
        fr[i].fields[F_PRE1]  = rd32be(ccb + 68);
        fr[i].fields[F_W]     = rd32be(ccb + 72);
        fr[i].fields[F_H]     = rd32be(ccb + 76);
        fr[i].pdat     = pdat + 8;
        fr[i].pdat_len = pdlen;
        fr[i].fields[F_PDOFF] = pdat_total;   /* offset into the PDAT blob */
        fr[i].fields[F_PDLEN] = pdlen;
        pdat_total += pdlen;

        /* Per-frame PLUT (find-smallest makes each cel's palette unique).
         * The 3DO 'PLUT' chunk data is [count:4][count x uint16 colors]; the
         * CEL engine's ccb_PLUTPtr must point at the COLOR ENTRIES, not the
         * count word — so skip the leading 4 bytes (chunk+8+4). Pointing at the
         * count word shifts every color and corrupts palettes per-frame. */
        cel_plut = find_chunk(buf, len, "PLUT", &pllen);
        if (!cel_plut || pllen < 4) {
            fprintf(stderr, "%s: missing/short PLUT chunk\n", path);
            return 1;
        }
        fr[i].plut     = cel_plut + 8 + 4;     /* skip 4-byte count */
        fr[i].plut_len = pllen - 4;            /* color entries only */
        fr[i].fields[F_PLOFF] = plut_total;    /* offset into the PLUT blob */
        fr[i].fields[F_PLLEN] = fr[i].plut_len;
        plut_total += fr[i].plut_len;
    }

    out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "create %s failed\n", argv[1]); return 1; }
    fwrite("ATL1", 1, 4, out);
    wr32be(out, (uint32_t)nframes);
    wr32be(out, plut_total);
    wr32be(out, pdat_total);
    for (i = 0; i < nframes; i++) {
        int k;
        for (k = 0; k < ATL_FIELDS; k++) wr32be(out, fr[i].fields[k]);
    }
    for (i = 0; i < nframes; i++)
        fwrite(fr[i].plut, 1, fr[i].plut_len, out);
    for (i = 0; i < nframes; i++)
        fwrite(fr[i].pdat, 1, fr[i].pdat_len, out);
    fclose(out);

    {
        uint32_t table = (uint32_t)nframes * ATL_FIELDS * 4;
        uint32_t total = 16 + table + plut_total + pdat_total;
        uint32_t runtime_ccb = (uint32_t)nframes * 104;  /* est. CCB struct */
        printf("atlas %s: %d frames\n", argv[1], nframes);
        printf("  PDAT blob = %u bytes (%u KB)\n", pdat_total, pdat_total / 1024);
        printf("  PLUT blob = %u bytes (%u frames)\n", plut_total, nframes);
        printf("  table     = %u bytes\n", table);
        printf("  .ATL file = %u bytes (%u KB)\n", total, total / 1024);
        printf("  runtime mem est = PDAT %u + PLUT %u + CCBs %u = %u bytes (%u KB)\n",
               pdat_total, plut_total, runtime_ccb,
               pdat_total + plut_total + runtime_ccb,
               (pdat_total + plut_total + runtime_ccb) / 1024);
    }

    for (i = 0; i < nframes; i++) free(bufs[i]);
    free(bufs); free(lens); free(fr); free(paths);
    if (manifest) free(manifest);
    return 0;
}
