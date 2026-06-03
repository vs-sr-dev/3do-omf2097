/*
 * atlas.h — runtime loader for the per-moveset CEL atlas (.ATL).
 *
 * WHY: loading a HAR moveset as 306 separate LoadCel() calls costs ~707 KB in
 * 3DO memory vs ~270 KB of actual pixel data — a 2.6x blow-up from per-CEL
 * page-rounding (each tiny allocation rounded to a memory-protect page). The
 * atlas packs the whole moveset's pixel data + per-frame PLUTs into ONE file,
 * loaded with ONE LoadFile() allocation; a CCB array points into that buffer.
 * Result ~272 KB/moveset, so two full symmetric fighters fit the DRAM+VRAM
 * budget with ~600 KB to spare (see memory project-dram-budget-s1-1).
 *
 * The .ATL is produced offline by host_tools/build_atlas.c. Format (all u32
 * big-endian = native on the 3DO, so fields are read straight through):
 *   "ATL1" | frame_count | plut_total | pdat_total
 *   frame table: frame_count x 16 u32
 *     {pdat_off, pdat_len, plut_off, plut_len, flags, hdx, hdy, vdx, vdy,
 *      hddx, hddy, pixc, pre0, pre1, width, height}
 *   PLUT blob (plut_total) then PDAT blob (pdat_total)
 */
#ifndef ATLAS_H
#define ATLAS_H

#include "types.h"
#include "graphics.h"

typedef struct {
    void  *buf;          /* LoadFile'd .ATL image — ONE allocation       */
    CCB   *ccbs;         /* AllocMem'd CCB array [frame_count], ptrs into buf */
    int32  frame_count;
    uint8 *plut_blob;    /* -> neutral PLUT blob inside buf (for pilot repoint) */
    int32  plut_total;   /* PLUT blob length in bytes                    */
    void  *pilot_plut;   /* LoadFile'd .PAL buffer when a pilot recolor is active,
                          * else NULL. Freed by AtlasFree.               */
} har_atlas_t;

/* Load <path> (e.g. "Art/HAR_JAGUAR.ATL") into one buffer + a CCB array whose
 * ccb_SourcePtr/ccb_PLUTPtr point into it. Returns 0 on success, -1 on failure
 * (atl is left zeroed/freed). */
int  AtlasLoad(const char *path, har_atlas_t *atl);

/* §1.4 per-pilot fight palettes: load a ".PAL" recolored PLUT blob (produced by
 * host_tools/dump_har gen-pluts) and repoint every CCB's ccb_PLUTPtr at it, so
 * the moveset renders in the pilot's colors WITHOUT touching the (shared) pixel
 * data. The .PAL frame layout is identical to the atlas PLUT blob, so each CCB's
 * existing PLUT offset is reused. Returns 0 on success, -1 on failure (atlas is
 * left rendering in its neutral colors). Safe to call once per battle init. */
int  AtlasApplyPilotPlut(har_atlas_t *atl, const char *pal_path);

/* Free the CCB array + the file buffer (+ any pilot PLUT). Safe on a zeroed atlas. */
void AtlasFree(har_atlas_t *atl);

#endif /* ATLAS_H */
