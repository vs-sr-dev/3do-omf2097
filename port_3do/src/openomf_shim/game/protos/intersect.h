/*
 * openomf_shim/game/protos/intersect.h
 *
 * Mirrors the canonical openomf intersect.h. Resolved by the 3DO firmware
 * include path BEFORE -I../openomf-master/src so the ported intersect.c
 * picks up our slim object/animation/sprite/surface graph instead of the
 * full openomf one.
 */
#ifndef SHIM_GAME_PROTOS_INTERSECT_H
#define SHIM_GAME_PROTOS_INTERSECT_H

#include "game/protos/object.h"

int intersect_object_object(object *a, object *b);
int intersect_object_point(object *obj, vec2i point);
int intersect_sprite_hitpoint(object *obj, object *target, int level, vec2i *point);
int intersect_har_sprite_hitpoint(object *obj, object *target, int level, vec2i *point);

#endif
