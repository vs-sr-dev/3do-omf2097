/* Minimal rgba_image stub for probe. */
#ifndef SD_RGBA_IMAGE_H
#define SD_RGBA_IMAGE_H

#include <stdint.h>

typedef struct {
    unsigned int w;
    unsigned int h;
    unsigned int len;
    char *data;
} sd_rgba_image;

#endif
