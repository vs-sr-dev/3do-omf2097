/*
 * openomf_shim/resources/animation.h
 *
 * Slim animation shim. intersect.c reads:
 *   - ani->collision_coords (vector of collision_coord)
 * and calls animation_get_sprite(ani, id) which we route to a per-fighter
 * sprite table (see omf_runtime.c).
 *
 * We deliberately omit ani->sprites (replaced by the shim's table-driven
 * animation_get_sprite) and the str/anim_string/extra_strings members
 * (not touched by intersect.c).
 */
#ifndef SHIM_RESOURCES_ANIMATION_H
#define SHIM_RESOURCES_ANIMATION_H

#include "resources/sprite.h"
#include "utils/vec.h"
#include "utils/vector.h"

typedef struct collision_coord_t {
    vec2i pos;
    int frame_index;
} collision_coord;

typedef struct animation_t {
    int id;
    vec2i start_pos;
    vector collision_coords;
    /* Shim-only: a flat sprite table indexed by sprite_id, owned by the
     * caller (no malloc at runtime). animation_get_sprite reads from here. */
    sprite *const *sprite_table;
    int sprite_count;
} animation;

sprite *animation_get_sprite(animation *ani, int sprite_id);

#endif
