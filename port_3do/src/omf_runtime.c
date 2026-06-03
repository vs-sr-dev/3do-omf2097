/*
 * omf_runtime.c — backing implementations for the openomf shim.
 *
 * Three groups of symbols are provided here, all referenced by intersect.c
 * (linked verbatim from openomf-master/src/game/protos/intersect.c):
 *
 *   1. Accessor shims  — object_get_pos/size/direction, sprite_get_size,
 *                        animation_get_sprite, player_frame_isset.
 *   2. Vector iter     — vector_size, vector_iter_begin, iter_next, plus
 *                        shim_vector_view for setting up a static view.
 *   3. View bridge     — omf_view_init, omf_view_update — used by hello.c
 *                        to wrap a fighter_t into an OMF object every tick.
 *
 * No malloc, no SDL, no logging. ARM SDT 2.51 compiler-compatible (no C99
 * compound literals, no designated initializers).
 */
#include "omf_runtime.h"

/* Local zeroer — avoid <string.h> per the same rationale as hello.c's
 * om_strlen helpers ([[project-session4-milestone]]): ARM SDT 2.51 libc
 * vs 3DO devkit <3do/string.h> have memchr signature mismatches and we
 * keep this code path independent. */
static void shim_bzero(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* ---------------- 1. Accessors ---------------- */

vec2i object_get_pos(const object *obj)
{
    return obj->pos;   /* shim narrowed to vec2i — see object.h note */
}

int object_get_direction(const object *obj)
{
    return obj->direction;
}

vec2i sprite_get_size(sprite *s)
{
    vec2i r;
    if (s && s->data) {
        r.x = s->data->w;
        r.y = s->data->h;
    } else {
        r.x = 0;
        r.y = 0;
    }
    return r;
}

vec2i object_get_size(const object *obj)
{
    sprite *cs = animation_get_sprite(obj->cur_animation, obj->cur_sprite_id);
    if (cs) return sprite_get_size(cs);
    {
        vec2i z;
        z.x = 0; z.y = 0;
        return z;
    }
}

sprite *animation_get_sprite(animation *ani, int sprite_id)
{
    if (ani == 0 || ani->sprite_table == 0) return 0;
    /* Single-frame view shim: the sprite_table only contains the currently
     * displayed frame, regardless of how many frames the anim logically
     * has. intersect.c calls this with obj->cur_sprite_id, but the same
     * cur_sprite_id is ALSO used to filter collision_coords by frame_index
     * — so the caller MUST set cur_sprite_id to the real OMF frame index
     * (p1Step->sprite_idx) for the coord filter to work. We ignore
     * sprite_id here and always return slot 0. */
    (void)sprite_id;
    return ((sprite *const *)ani->sprite_table)[0];
}

/* intersect.c calls player_frame_isset(obj, "r") to check horizontal flip.
 * Our shim_anim_state caches the relevant tags as bools set by the caller
 * (omf_view_update) from jaguar_anim_step_t.flip_r. Stage B will extend
 * the set if more tags become relevant (e.g. "h", "l"). */
int player_frame_isset(const object *obj, const char *tag)
{
    if (tag == 0 || tag[0] == 0) return 0;
    switch (tag[0]) {
    case 'r': return obj->animation_state.tag_r ? 1 : 0;
    case 'h': return obj->animation_state.tag_h ? 1 : 0;
    case 'l': return obj->animation_state.tag_l ? 1 : 0;
    default:  return 0;
    }
}

/* ---------------- 2. Vector + iterator shim ---------------- */

unsigned int vector_size(const vector *v)
{
    return v ? v->blocks : 0;
}

/* Forward iterator next-fn for our shim vectors. */
static void *shim_vec_iter_next(iterator *it)
{
    const vector *v = (const vector *)it->data;
    if (it->ended || v == 0) return 0;
    if (it->inow + 1 >= (int)v->blocks) {
        if (it->inow + 1 == (int)v->blocks) {
            /* return last and mark ended on next call */
            it->inow++;
            it->ended = 1;
            return v->data + (it->inow - 1) * v->block_size;
        }
        it->ended = 1;
        return 0;
    }
    it->inow++;
    return v->data + it->inow * v->block_size;
}

void vector_iter_begin(const vector *v, iterator *it)
{
    if (it == 0) return;
    it->data = v;
    it->vnow = 0;
    it->inow = -1;
    it->ended = (v == 0 || v->blocks == 0) ? 1 : 0;
    it->next = shim_vec_iter_next;
    it->prev = 0;
    it->peek = 0;
}

void *iter_next(iterator *it)
{
    if (it == 0 || it->next == 0 || it->ended) return 0;
    return it->next(it);
}

void shim_vector_view(vector *out, const void *data, unsigned int block_size,
                      unsigned int blocks)
{
    out->data = (char *)data;
    out->block_size = block_size;
    out->blocks = blocks;
    out->reserved = blocks;
    out->free_cb = 0;
}

/* ---------------- 3. View bridge ---------------- */

void omf_view_init(fighter_omf_view *v)
{
    shim_bzero(v, sizeof(*v));
    v->sprite_table_storage[0] = &v->cur_sprite;
    v->cur_sprite.data = &v->cur_surface;
    v->anim.sprite_table = (sprite *const *)v->sprite_table_storage;
    v->anim.sprite_count = 1;
    /* collision_coords stays empty in Stage A. shim_vector_view it later
     * (Stage B) when caller has a real coord array for this frame. */
    shim_vector_view(&v->anim.collision_coords, 0, sizeof(collision_coord), 0);
    v->obj.cur_animation = &v->anim;
    v->obj.cur_sprite_id = 0;
}

void omf_view_update(fighter_omf_view *v,
                     int world_x, int world_y, int facing,
                     int sprite_w, int sprite_h,
                     int sprite_px, int sprite_py,
                     int flip_r,
                     unsigned char *hitmask,
                     const void *coords_data, int coords_count,
                     int cur_sprite_id)
{
    v->obj.pos.x = world_x;
    v->obj.pos.y = world_y;
    v->obj.direction = facing;
    /* cur_sprite_id is the OMF frame index in the current animation.
     * intersect.c uses it BOTH for animation_get_sprite (we ignore) AND
     * for filtering collision_coords (cc->frame_index == cur_sprite_id). */
    v->obj.cur_sprite_id = cur_sprite_id;
    v->obj.animation_state.tag_r = (unsigned char)(flip_r ? 1 : 0);
    /* tag_h/tag_l left for caller to set if needed. */

    v->cur_sprite.pos.x = sprite_px;
    v->cur_sprite.pos.y = sprite_py;

    v->cur_surface.w = sprite_w;
    v->cur_surface.h = sprite_h;
    v->cur_surface.transparent = 0;
    v->cur_surface.data = hitmask;

    /* Point the animation's collision_coords vector view at the caller's
     * packed (x, y, frame_index) array. Layout matches collision_coord
     * (12 bytes/entry); intersect() walks via vector_iter_begin + foreach. */
    if (coords_data != 0 && coords_count > 0) {
        shim_vector_view(&v->anim.collision_coords, coords_data,
                         sizeof(collision_coord), (unsigned int)coords_count);
    } else {
        shim_vector_view(&v->anim.collision_coords, 0,
                         sizeof(collision_coord), 0);
    }
}

/* ---------------- 4. Logic-port shim helpers ---------------- */

/* af_get_move — linear scan by OMF move id. Mirrors openomf af_get_move
 * (which hashmaps); a ~43-entry linear scan is fine since callers hit it
 * only on attack input, not per-frame. Returns 0 if absent. */
af_move *af_get_move(const af *a, int id)
{
    int i;
    if (a == 0) return 0;
    for (i = 0; i < a->count; i++) {
        if (a->moves[i].id == id) return &a->moves[i];
    }
    return 0;
}

/* Integer square root (Newton-free bit method) — backs object_distance
 * without the FPU openomf's vec2f_dist assumes. */
static int shim_isqrt(int n)
{
    int x, b;
    if (n <= 0) return 0;
    x = 0;
    b = 1 << 30;
    while (b > n) b >>= 2;
    while (b != 0) {
        if (n >= x + b) {
            n -= x + b;
            x = (x >> 1) + b;
        } else {
            x >>= 1;
        }
        b >>= 2;
    }
    return x;
}

/* object_distance — integer Euclidean distance between two object positions.
 * Upstream (object.c:490) is vec2f_dist; our positions are integer OMF world
 * units (hello.c JAG_BASE_X / P2_ANCHOR_X), so this is exact for the grounded
 * case (dy == 0) and a faithful approximation otherwise. */
int object_distance(const object *a, const object *b)
{
    int dx, dy;
    if (a == 0 || b == 0) return 0x7fffffff;
    dx = a->pos.x - b->pos.x;
    dy = a->pos.y - b->pos.y;
    return shim_isqrt(dx * dx + dy * dy);
}

void *object_get_userdata(const object *obj)
{
    return obj ? obj->userdata : 0;
}

game_player *game_state_get_player(game_state *gs, int player_id)
{
    return &gs->players[player_id & 1];
}

/* "k" damage-multiplier frame tag is not baked into our anim steps yet.
 * player_frame_isset("k") returns 0 (omf_runtime.c only tags r/h/l), so
 * calc_damage_and_stun never actually calls this — present for link only. */
int player_frame_get(const object *obj, const char *tag)
{
    (void)obj; (void)tag;
    return 0;
}

/* Populate an af + af_move[] view from the baked har_move_meta_t table. */
void omf_build_af(af *out_af, af_move *move_storage,
                  const har_move_meta_t *metas, int meta_count)
{
    int i;
    for (i = 0; i < meta_count; i++) {
        af_move *m = &move_storage[i];
        m->id             = metas[i].id;
        m->category       = (unsigned char)metas[i].category;
        m->damage         = metas[i].damage;
        m->next_move      = metas[i].next_move;
        m->successor_id   = metas[i].successor_id;
        m->block_damage   = metas[i].block_damage;
        m->block_stun     = metas[i].block_stun;
        m->throw_duration = metas[i].throw_duration;
        m->points         = metas[i].points;
        m->hit_sound      = metas[i].hit_sound;
        m->knockback_x    = metas[i].knockback_x;
        m->recoil_ticks   = metas[i].recoil_ticks;
        m->stun           = 0;   /* af_move.stun starts 0 (runtime-computed) */
    }
    out_af->moves = move_storage;
    out_af->count = meta_count;
}
