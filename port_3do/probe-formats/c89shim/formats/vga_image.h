/* Minimal sd_vga_image stub for probe. */
#ifndef SD_VGA_IMAGE_H
#define SD_VGA_IMAGE_H

#include "formats/palette.h"
#include "formats/rgba_image.h"

typedef struct {
    unsigned int w;
    unsigned int h;
    unsigned int len;
    char *data;
} sd_vga_image;

int  sd_vga_image_create(sd_vga_image *img, unsigned int w, unsigned int h);
void sd_vga_image_free(sd_vga_image *img);

#endif
