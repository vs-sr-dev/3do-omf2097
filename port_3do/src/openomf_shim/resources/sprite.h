/*
 * openomf_shim/resources/sprite.h
 *
 * Slim sprite shim. Layout-compatible with openomf-master/src/resources/sprite.h
 * (id, pos, data, owned). Only .pos and .data are read by intersect.c.
 */
#ifndef SHIM_RESOURCES_SPRITE_H
#define SHIM_RESOURCES_SPRITE_H

#include "utils/vec.h"
#include "video/surface.h"

typedef struct sprite_t {
    int id;
    vec2i pos;
    surface *data;
    int owned;   /* bool in openomf — int here (3DO toolchain has no stdbool) */
} sprite;

vec2i sprite_get_size(sprite *s);

#endif
