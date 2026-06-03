/*
 * openomf_shim/game/protos/object.h
 *
 * Slim object shim. Keeps only the fields intersect.c reads:
 *   - cur_animation (animation*)
 *   - cur_sprite_id (int)
 *   - direction (int)  — accessed via object_get_direction
 *   - pos (vec2f)      — accessed via object_get_pos
 *
 * The OMF object also carries velocity, gravity, hashmaps, callbacks,
 * player_*_state, vga_palette_transform, serial buffers — all stripped.
 * Adding them back later happens piecewise as we port more openomf .c files
 * (har.c, projectile.c, etc.).
 *
 * shim_anim_state: substitute for openomf's player_animation_state. We don't
 * carry sd_script_parser; player_frame_isset (in omf_runtime.c) reads
 * cur_r_tag/cur_h_tag etc. directly from booleans set by the caller from
 * the pre-parsed jaguar_anim_step_t flags.
 */
#ifndef SHIM_GAME_PROTOS_OBJECT_H
#define SHIM_GAME_PROTOS_OBJECT_H

/* NULL — openomf .c files compare pointers to NULL but <stddef.h> on the
 * 3DO devkit conflicts with the kernel's offsetof macro. Define it locally
 * only if not already defined by 3DO/types.h. */
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "resources/animation.h"
#include "resources/sprite.h"
#include "utils/vec.h"
#include "video/surface.h"

/* On 3DO/ARM, route to the devkit's types_boolean.h for bool/TRUE/FALSE/
 * true/false. armcc treats `bool` as quasi-reserved and won't accept a
 * duplicate typedef even with matching type. On host compiles we fall
 * back to a minimal C90-compatible typedef. */
#ifdef __arm
#include "types_boolean.h"
#else
#ifndef SHIM_BOOL_DEFINED
#define SHIM_BOOL_DEFINED 1
#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif
typedef int bool;
#endif
#endif

enum {
    OBJECT_FACE_LEFT  = -1,
    OBJECT_FACE_NONE  = 0,
    OBJECT_FACE_RIGHT = 1
};

typedef struct shim_anim_state_t {
    unsigned char tag_r;   /* current frame has 'r' (horizontal flip) */
    unsigned char tag_h;   /* current frame is a hit frame */
    unsigned char tag_l;   /* lands on ground (unused yet) */
    unsigned char _pad;
} shim_anim_state;

/* Forward decls for the logic-port shim (full types in omf_runtime.h).
 * object carries these so ported har.c bodies can do obj->gs->... and
 * object_get_userdata(obj) verbatim. */
struct game_state_t;

typedef struct object_t {
    /* openomf uses vec2f here for sub-pixel physics, but the 3DO devkit
     * libs ship no softfp helpers (ARM6 with no FPU) — any float op forces
     * unresolved _fadd/_fmul/etc. We render at integer pixels anyway, so
     * the shim narrows pos to vec2i. Adapt where the target forces it. */
    vec2i pos;
    int direction;
    animation *cur_animation;
    int cur_sprite_id;
    shim_anim_state animation_state;
    /* Logic-port additions (slice 1). userdata -> har*, gs -> game_state*.
     * Set up by the caller (hello.c) once per battle. */
    void *userdata;
    struct game_state_t *gs;
} object;

vec2i object_get_pos(const object *obj);
vec2i object_get_size(const object *obj);
int object_get_direction(const object *obj);

int player_frame_isset(const object *obj, const char *tag);

#endif
