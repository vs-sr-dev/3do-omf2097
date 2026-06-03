/* Minimal palette stub for probe. */
#ifndef SD_PALETTE_H
#define SD_PALETTE_H

#include <stdint.h>

typedef struct { uint8_t r, g, b; } vga_color;

typedef struct {
    vga_color colors[256];
    uint8_t   remap[19][256];
} vga_palette;

#endif
