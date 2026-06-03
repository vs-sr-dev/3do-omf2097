/*
 * atlas.c — runtime .ATL loader. See atlas.h for rationale and format.
 *
 * C90 declarations are hoisted to block tops (ARM SDT 2.51 rejects mid-block
 * declarations — see project-3do-pipeline-gotchas).
 */
#include "atlas.h"
#include "mem.h"
#include "blockfile.h"   /* LoadFile, UnloadFile */
#include "hardware.h"    /* CCB_SPABS, CCB_PPABS */
#include "debug3do.h"
#include "stdio.h"       /* printf */

#define ATL_MAGIC   0x41544C31U   /* "ATL1" read big-endian (native on 3DO) */
#define ATL_FIELDS  16

/* Frame-table field indices (must match host_tools/build_atlas.c). */
enum {
    F_PDOFF = 0, F_PDLEN, F_PLOFF, F_PLLEN, F_FLAGS, F_HDX, F_HDY, F_VDX,
    F_VDY, F_HDDX, F_HDDY, F_PIXC, F_PRE0, F_PRE1, F_W, F_H
};

int
AtlasLoad(const char *path, har_atlas_t *atl)
{
    int32   size = 0;
    uint32 *hdr;
    int32   frame_count, plut_total, i;
    uint8  *base, *table, *plut_blob, *pdat_blob;

    atl->buf = NULL;
    atl->ccbs = NULL;
    atl->frame_count = 0;

    atl->buf = LoadFile((char *)path, &size, MEMTYPE_ANY);
    if (atl->buf == NULL) {
        printf("AtlasLoad: LoadFile(%s) failed\n", path);
        return -1;
    }
    base = (uint8 *)atl->buf;
    hdr  = (uint32 *)atl->buf;
    if (hdr[0] != ATL_MAGIC) {
        printf("AtlasLoad: %s bad magic 0x%lx\n", path, (long)hdr[0]);
        AtlasFree(atl);
        return -1;
    }
    frame_count = (int32)hdr[1];
    plut_total  = (int32)hdr[2];
    /* hdr[3] = pdat_total (implicit: rest of the file) */

    table     = base + 16;
    plut_blob = table + (uint32)frame_count * ATL_FIELDS * 4;
    pdat_blob = plut_blob + plut_total;

    atl->ccbs = (CCB *)AllocMem((int32)(frame_count * (int32)sizeof(CCB)),
                                MEMTYPE_ANY);
    if (atl->ccbs == NULL) {
        printf("AtlasLoad: %s CCB array alloc (%d) failed\n",
               path, (int)(frame_count * (int32)sizeof(CCB)));
        AtlasFree(atl);
        return -1;
    }

    for (i = 0; i < frame_count; i++) {
        uint32 *e = (uint32 *)(table + i * ATL_FIELDS * 4);
        CCB    *c = &atl->ccbs[i];
        /* Keep the cel's own flags (bpp/packed/load bits, LAST), but force ALL
         * pointers ABSOLUTE: SPABS/PPABS for our source/plut, and NPABS so that
         * when the battle render chains this CCB to the next one (setting
         * ccb_NextPtr to a real address + clearing LAST) the cel engine reads
         * NextPtr as absolute. Without NPABS the chain breaks after the head —
         * the cel's file flags have npabs:false (LoadCel sets it at load time;
         * we must too). */
        c->ccb_Flags     = e[F_FLAGS] | CCB_SPABS | CCB_PPABS | CCB_NPABS;
        c->ccb_NextPtr   = NULL;
        c->ccb_SourcePtr = (CelData *)(pdat_blob + e[F_PDOFF]);
        c->ccb_PLUTPtr   = (void *)(plut_blob + e[F_PLOFF]);
        c->ccb_XPos      = 0;
        c->ccb_YPos      = 0;
        c->ccb_HDX       = (int32)e[F_HDX];
        c->ccb_HDY       = (int32)e[F_HDY];
        c->ccb_VDX       = (int32)e[F_VDX];
        c->ccb_VDY       = (int32)e[F_VDY];
        c->ccb_HDDX      = (int32)e[F_HDDX];
        c->ccb_HDDY      = (int32)e[F_HDDY];
        c->ccb_PIXC      = e[F_PIXC];
        c->ccb_PRE0      = e[F_PRE0];
        c->ccb_PRE1      = e[F_PRE1];
        c->ccb_Width     = (int32)e[F_W];
        c->ccb_Height    = (int32)e[F_H];
    }

    atl->frame_count = frame_count;
    atl->plut_blob   = plut_blob;
    atl->plut_total  = plut_total;
    atl->pilot_plut  = NULL;
    printf("AtlasLoad %s: %d frames, file=%d bytes\n",
           path, (int)frame_count, (int)size);
    return 0;
}

int
AtlasApplyPilotPlut(har_atlas_t *atl, const char *pal_path)
{
    int32   size = 0;
    uint8  *pal;
    uint32  magic, plut_total;
    uint8  *new_blob;
    int32   i;

    if (atl == NULL || atl->ccbs == NULL || atl->plut_blob == NULL) return -1;

    pal = (uint8 *)LoadFile((char *)pal_path, &size, MEMTYPE_ANY);
    if (pal == NULL) {
        printf("AtlasApplyPilotPlut: LoadFile(%s) failed\n", pal_path);
        return -1;
    }
    /* ".PAL" = "PPAL" | plut_total | frame_count | 0, then the PLUT blob. */
    if (size < 16) { printf("AtlasApplyPilotPlut: %s too small\n", pal_path);
                     UnloadFile(pal); return -1; }
    magic      = ((uint32)pal[0] << 24) | ((uint32)pal[1] << 16) |
                 ((uint32)pal[2] << 8)  |  (uint32)pal[3];
    plut_total = ((uint32)pal[4] << 24) | ((uint32)pal[5] << 16) |
                 ((uint32)pal[6] << 8)  |  (uint32)pal[7];
    if (magic != 0x5050414Cu) {     /* "PPAL" */
        printf("AtlasApplyPilotPlut: %s bad magic 0x%lx\n", pal_path, (long)magic);
        UnloadFile(pal); return -1;
    }
    if ((int32)plut_total != atl->plut_total) {
        printf("AtlasApplyPilotPlut: %s plut_total %d != atlas %d\n",
               pal_path, (int)plut_total, (int)atl->plut_total);
        UnloadFile(pal); return -1;
    }

    /* Repoint each CCB's PLUT into the new blob, preserving its per-frame offset
     * (offset == old ccb_PLUTPtr - neutral plut_blob). */
    new_blob = pal + 16;
    for (i = 0; i < atl->frame_count; i++) {
        CCB  *c = &atl->ccbs[i];
        long  off = (uint8 *)c->ccb_PLUTPtr - atl->plut_blob;
        if (off < 0 || off >= atl->plut_total) continue;   /* paranoia */
        c->ccb_PLUTPtr = (void *)(new_blob + off);
    }

    /* If a previous pilot PLUT was loaded, free it; keep the new one alive for
     * the duration of the battle (AtlasFree releases it). */
    if (atl->pilot_plut != NULL) UnloadFile(atl->pilot_plut);
    atl->pilot_plut = (void *)pal;
    /* From now on the CCBs reference new_blob; the neutral blob stays in atl->buf
     * but is no longer used for rendering until AtlasFree. */
    atl->plut_blob = new_blob;   /* so a second apply re-derives offsets right */
    printf("AtlasApplyPilotPlut %s: repointed %d PLUTs\n",
           pal_path, (int)atl->frame_count);
    return 0;
}

void
AtlasFree(har_atlas_t *atl)
{
    if (atl->ccbs != NULL) {
        FreeMem(atl->ccbs, atl->frame_count * (int32)sizeof(CCB));
        atl->ccbs = NULL;
    }
    if (atl->pilot_plut != NULL) {
        UnloadFile(atl->pilot_plut);
        atl->pilot_plut = NULL;
    }
    if (atl->buf != NULL) {
        UnloadFile(atl->buf);
        atl->buf = NULL;
    }
    atl->frame_count = 0;
    atl->plut_blob   = NULL;
    atl->plut_total  = 0;
}
