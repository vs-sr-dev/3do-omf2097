/*
 * openomf_shim/video/surface.h
 *
 * Slim shim for openomf's surface. Layout-compatible with the canonical
 * struct in openomf-master/src/video/surface.h: same field order so
 * intersect.c reads .w/.h/.transparent/.data correctly.
 *
 * In our runtime, surface.data is the hitmask blob emitted by dump_har —
 * 1 byte per pixel, encoded as {0 = transparent, 50 = main HAR color,
 * 200 = accent}. The numeric values are chosen so intersect.c's checks
 * `data[hp] != transparent` and `data[hp] < 96` work unmodified (preserves
 * derive-don't-reimplement for intersect.c).
 *
 * We omit guid (unused in the bbox/hitpoint path). vga_pixel is just
 * uint8_t in openomf.
 */
#ifndef SHIM_VIDEO_SURFACE_H
#define SHIM_VIDEO_SURFACE_H

typedef unsigned char vga_pixel;

typedef struct surface {
    unsigned int guid;     /* kept for layout match; we leave it 0 */
    int w;
    int h;
    int transparent;
    vga_pixel *data;
} surface;

#endif
