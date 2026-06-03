/*
 * port_3do/src/intersect.c
 *
 * VERBATIM MIRROR of openomf-master/src/game/protos/intersect.c with the
 * minimum modifications needed to compile under ARM SDT 2.51 armcc, which
 * is strict C90 (no mid-block declarations, no C99 `for(int k = 0; ...)`
 * init scope).
 *
 * Changes from upstream — algorithm/logic UNCHANGED except where noted (#5),
 * mechanical only otherwise:
 *   1. Function-local declarations hoisted to the top of their enclosing
 *      block, per C90.
 *   2. C99 `for(int k = 0; ...)` rewritten as plain `for(k = 0; ...)`
 *      with `int k` declared at function top.
 *   3. `assert()` left in place, but `<assert.h>` swapped for a no-op
 *      stub (the devkit's <assert.h> drags in stdio/abort which we don't
 *      want for a leaf math function). The check `level == 1 || level == 2`
 *      is anyway always-true at all current call sites.
 *   4. `vec2f sum` → `vec2i sum` in the hitpoint averaging at intersect():
 *      the 3DO devkit libs ship no softfp helpers and the math is integer
 *      pixel coords anyway. `sum.x / level` is plain int division yielding
 *      the same result openomf produces (level is 1 or 2; floor of int
 *      avg matches truncation of float avg for these magnitudes).
 *   5. **Hitmask is bit-packed 1 bpp on 3DO**: sfc->data is (w*h+7)/8 bytes
 *      with bit set = opaque, cleared = transparent. Upstream stores 1 byte
 *      per pixel encoding transparent/main/accent (0 / <96 / >=96). Bit-
 *      packing saves ~800 KB in DRAM (~898 KB Jaguar hitmasks → ~115 KB).
 *      Semantic change: HAR-vs-HAR collisions now silhouette-based — accent
 *      pixels count as hits like main pixels do. For 2D-fighter intuition
 *      this is fine; `is_har` becomes effectively redundant but kept in the
 *      signature for upstream ABI compat. See [[project-battle-load-model]].
 *
 * `diff -u` against upstream stays small + readable.
 *
 * Upstream: openomf-master/src/game/protos/intersect.c
 * Synced: 2026-05-20 (sessione 5; bit-pack added sessione 6)
 *
 * Rationale: see [[feedback-derive-dont-reimplement]]. "Adapt where the
 * target forces it" — C90 declaration ordering IS a target constraint.
 * The shim plumbing (Makefile rule for build/openomf/%.o, openomf_shim/
 * include path) stays in place for other openomf .c files that don't
 * trip C90 strict — see project-host-tools-strategy for the same pattern
 * on the host side.
 */

#define assert(x) ((void)0)   /* see header note #3 */

#include "game/protos/intersect.h"

/* --- intersect_object_object: bbox vs bbox (no decls-after-stmt issue). --- */
int intersect_object_object(object *a, object *b) {
    sprite *cur_sprite_a;
    sprite *cur_sprite_b;
    vec2i pos_a, pos_b, size_a, size_b;

    if(a->cur_sprite_id < 0 || b->cur_sprite_id < 0) {
        return 0;
    }
    cur_sprite_a = animation_get_sprite(a->cur_animation, a->cur_sprite_id);
    cur_sprite_b = animation_get_sprite(b->cur_animation, b->cur_sprite_id);
    pos_a = vec2i_add(object_get_pos(a), cur_sprite_a->pos);
    pos_b = vec2i_add(object_get_pos(b), cur_sprite_b->pos);
    size_a = object_get_size(a);
    size_b = object_get_size(b);
    return !(pos_a.x > (pos_b.x + size_b.x) || pos_a.y > (pos_b.y + size_b.y) || (pos_a.x + size_a.x) < pos_b.x ||
             (pos_a.y + size_a.y) < pos_b.y);
}

int intersect_object_point(object *obj, vec2i point) {
    sprite *cur_sprite;
    vec2i pos, size;

    if(obj->cur_sprite_id < 0) {
        return 0;
    }
    cur_sprite = animation_get_sprite(obj->cur_animation, obj->cur_sprite_id);
    pos = vec2i_add(object_get_pos(obj), cur_sprite->pos);
    size = object_get_size(obj);
    return (point.x < (pos.x + size.x) && point.y < (pos.y + size.y) && point.x > pos.x && point.y > pos.y);
}

int intersect(object *obj, object *target, int level, vec2i *point, bool is_har) {
    int object_dir;
    int target_dir;
    sprite *cur_sprite;
    sprite *target_sprite;
    vec2i pos_a, pos_b, size_a, size_b;
    vec2i hcoords[2];
    int found;
    iterator it;
    collision_coord *cc;
    int t, xcoord, ycoord;
    surface *sfc;
    int hitpoint;
    vec2i sum;   /* upstream uses vec2f; see header note #4 */
    int k;

    /* Make sure both objects have sprites going */
    if(obj->cur_sprite_id < 0 || target->cur_sprite_id < 0) {
        return 0;
    }
    /* Make sure there are hitpoints to check. */
    if(vector_size(&obj->cur_animation->collision_coords) == 0) {
        return 0;
    }

    object_dir = OBJECT_FACE_RIGHT;
    target_dir = OBJECT_FACE_RIGHT;

    cur_sprite = animation_get_sprite(obj->cur_animation, obj->cur_sprite_id);
    target_sprite = animation_get_sprite(target->cur_animation, target->cur_sprite_id);
    if(target_sprite == NULL) {
        return 0;
    }
    pos_a = vec2i_add(object_get_pos(obj), cur_sprite->pos);
    pos_b = vec2i_add(object_get_pos(target), target_sprite->pos);
    size_a = object_get_size(obj);
    size_b = object_get_size(target);

    if((object_get_direction(obj) == OBJECT_FACE_LEFT && !player_frame_isset(obj, "r")) ||
       (object_get_direction(obj) == OBJECT_FACE_RIGHT && player_frame_isset(obj, "r"))) {
        object_dir = OBJECT_FACE_LEFT;
        pos_a.x = object_get_pos(obj).x + ((cur_sprite->pos.x * -1) - size_a.x);
    }

    if((object_get_direction(target) == OBJECT_FACE_LEFT && !player_frame_isset(target, "r")) ||
       (object_get_direction(target) == OBJECT_FACE_RIGHT && player_frame_isset(target, "r"))) {
        target_dir = OBJECT_FACE_LEFT;
        pos_b.x = object_get_pos(target).x + ((target_sprite->pos.x * -1) - size_b.x);
    }

    /* Iterate through hitpoints */
    assert(level == 1 || level == 2);
    (void)is_har;   /* see diff #5 — bit-packed mask is opacity-only */
    found = 0;
    vector_iter_begin(&obj->cur_animation->collision_coords, &it);
    foreach(it, cc) {
        /* Skip coords that don't belong to the frame we are checking */
        if(cc->frame_index != obj->cur_sprite_id) {
            continue;
        }

        /* Convert coords to target sprite local space */
        t = (object_dir == OBJECT_FACE_RIGHT) ? (pos_a.x + cc->pos.x - cur_sprite->pos.x)
                                              : (pos_a.x + (size_a.x - cc->pos.x) + cur_sprite->pos.x);

        /* convert global coordinates to local coordinates by compensating for the other player's position */
        xcoord = t - pos_b.x;
        /* Also note that the hit pixel position during jumps is innacurate because hacks */
        ycoord = (pos_a.y + size_a.y + cc->pos.y) - pos_b.y;

        ycoord -= (cur_sprite->pos.y + size_a.y);

        /* Make sure that the hitpixel is within the area of the target sprite */
        if(xcoord < 0 || xcoord >= size_b.x) {
            continue;
        } else if(ycoord < 0 || ycoord >= size_b.y) {
            continue;
        }

        /* Get hitpixel */
        sfc = target_sprite->data;
        hitpoint = (ycoord * sfc->w) + xcoord;
        if(target_dir == OBJECT_FACE_LEFT) {
            hitpoint = (ycoord * sfc->w) + (sfc->w - xcoord);
        }
        /* Data bounds check */
        if(hitpoint >= sfc->w * sfc->h) {
            continue;
        }
        /* Per diff #5: 1-bpp bit-packed hitmask (set = opaque). Upstream did
         * `data[hp] != transparent && (data[hp] < 96 || !is_har)`. */
        if(sfc->data[hitpoint >> 3] & (unsigned char)(1u << (hitpoint & 7))) {
            hcoords[found++] = vec2i_create(xcoord, ycoord);
            if(found >= level) {
                sum.x = 0;
                sum.y = 0;
                for(k = 0; k < level; k++) {
                    sum.x += hcoords[k].x;
                    sum.y += hcoords[k].y;
                }
                point->x = (sum.x / level) + pos_b.x;
                point->y = (sum.y / level) + pos_b.y;
                return 1;
            }
        }
    }

    return 0;
}

int intersect_sprite_hitpoint(object *obj, object *target, int level, vec2i *point) {
    return intersect(obj, target, level, point, false);
}

int intersect_har_sprite_hitpoint(object *obj, object *target, int level, vec2i *point) {
    return intersect(obj, target, level, point, true);
}
