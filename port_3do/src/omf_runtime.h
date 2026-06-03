/*
 * omf_runtime.h
 *
 * Shim layer that lets us link openomf .c files (starting with intersect.c)
 * verbatim into the 3DO firmware without bringing in SDL/render/hashmap.
 *
 * Strategy: see [[feedback-derive-dont-reimplement]] + [[project-host-tools-strategy]].
 * The openomf_shim/ headers provide slim object/animation/sprite/surface
 * type declarations; this header adds a tiny "view" struct that wraps a
 * 3DO-side fighter (hello.c's fighter_t etc.) into the OMF object graph
 * each tick. Caller-owned storage, no malloc.
 *
 * Usage from hello.c (Stage A — bbox only):
 *
 *   static fighter_omf_view gJagView, gShdView;
 *   ...
 *   omf_view_update(&gJagView, jag_world_x, jag_world_y, facing,
 *                   frame->w, frame->h, frame->pos_x, frame->pos_y,
 *                   step->flip_r);
 *   omf_view_update(&gShdView, ...);
 *   if (intersect_object_object(&gJagView.obj, &gShdView.obj)) {
 *       // bbox overlap detected
 *   }
 */
#ifndef OMF_RUNTIME_H
#define OMF_RUNTIME_H

#include "game/protos/object.h"      /* shim object */
#include "resources/animation.h"     /* shim animation, collision_coord */
#include "resources/sprite.h"        /* shim sprite */
#include "video/surface.h"           /* shim surface */
#include "utils/vector.h"            /* shim vector */
#include "har_packs.h"               /* har_move_meta_t (logic-port source) */

/* ====================================================================
 * openomf logic-port shim (slice 1: is_in_range).
 *
 * These slim "view" types mirror the openomf field names so functions
 * ported verbatim from game/objects/har.c compile unchanged (see
 * src/har_logic.c). They are NOT the full openomf structs — only the
 * fields the ported logic touches. They grow per slice.
 * See [[project-harc-port-decision]] + [[feedback-derive-dont-reimplement]].
 * ==================================================================== */

/* Move categories — values MUST match openomf game/objects/har.h. */
enum {
    CAT_MISC        = 0,
    CAT_CLOSE       = 2,
    CAT_LOW         = 4,
    CAT_MEDIUM      = 5,
    CAT_HIGH        = 6,
    CAT_JUMPING     = 7,
    CAT_PROJECTILE  = 8,
    CAT_BASIC       = 9,
    CAT_VICTORY     = 10,
    CAT_FIRE_ICE    = 11,
    CAT_SCRAP       = 12,
    CAT_DESTRUCTION = 13
};

/* Slim af_move — only scalar fields the ported har.c logic reads. Built from
 * har_move_meta_t at battle start. Field names mirror openomf af_move.h.
 * NOTE: upstream `damage`/`stun` are float; we keep them int (3DO has no FPU,
 * and OMF damage values are whole bytes) — adapt per the intersect.c model. */
typedef struct af_move_t {
    int           id;
    unsigned char next_move;
    unsigned char successor_id;
    unsigned char category;
    unsigned char block_damage;
    unsigned char block_stun;
    unsigned char throw_duration;
    int           points;
    int           damage;
    int           stun;
    int           hit_sound;    /* footer impact sound param; -1 = none */
    int           knockback_x;  /* net footer x-displacement (OMF px) */
    int           recoil_ticks; /* footer duration in ticks */
} af_move;

/* Slim af — the move table for one HAR. af_get_move(af, id) scans it. */
typedef struct af_t {
    af_move *moves;
    int      count;
} af;

/* Slim har — hung off object.userdata. Grows per slice. slice 1 needs af_data
 * for is_in_range's next_move chain lookup; slice 2 adds player_id so
 * calc_damage_and_stun can fetch this fighter's pilot. */
typedef struct har_t {
    const af     *af_data;
    unsigned char player_id;   /* 0 or 1 */
} har;

/* Canonical OMF pilot ids — values match openomf common_defines.h:80. */
enum {
    PILOT_CRYSTAL = 0, PILOT_STEFFAN, PILOT_MILANO,  PILOT_CHRISTIAN,
    PILOT_SHIRRO,      PILOT_JEANPAUL, PILOT_IBRAHIM, PILOT_ANGEL,
    PILOT_COSSETTE,    PILOT_RAVEN,    PILOT_KREISSACK
};
enum { PILOT_SEX_MALE = 0, PILOT_SEX_FEMALE = 1 };

/* Slim pilot — subset of openomf formats/pilot.h the ported logic reads.
 * pilot_get_info (pilot_stats.c) fills power/agility/endurance/sex; pilot
 * COLORS are owned by pilots_data.h (from ALTPALS) and not duplicated here. */
typedef struct omf_pilot_t {
    int power;       /* 1..25 */
    int agility;     /* 1..25 */
    int endurance;   /* 1..25 */
    int sex;
    int photo;       /* 0 = single-player (no tournament armor mitigation) */
    int armor;
} omf_pilot;

typedef struct game_player_t {
    omf_pilot *pilot;
    int        god;
} game_player;

/* Slim game_state + match_settings. */
typedef struct match_settings_t {
    int throw_range;   /* percent; OMF default 100 == 1.0x (game_state.c:255) */
} match_settings;

typedef struct game_state_t {
    match_settings match_settings;
    game_player    players[2];
} game_state;

/* Shim helpers backing the ported har.c functions (impl in omf_runtime.c). */
af_move *af_get_move(const af *a, int id);
int      object_distance(const object *a, const object *b);
void    *object_get_userdata(const object *obj);

/* Build an af + its af_move[] from a har_move_meta_t table. Caller owns
 * move_storage (>= meta_count entries). Call once per battle after pack select. */
void omf_build_af(af *out_af, af_move *move_storage,
                  const har_move_meta_t *metas, int meta_count);

/* Ported from openomf har.c (src/har_logic.c). Returns false if `move` is a
 * range-limited throw and `enemy_obj` is beyond reach of `obj`. */
bool is_in_range(object *obj, object *enemy_obj, af_move *move);

/* ---- slice 2: pilot stats + canon damage formula ---- */

/* &gs->players[player_id]. */
game_player *game_state_get_player(game_state *gs, int player_id);

/* Shim stub for the "k" damage-multiplier frame tag (not baked into our steps
 * yet; player_frame_isset("k") returns 0 so this is never actually reached). */
int player_frame_get(const object *obj, const char *tag);

/* Ported verbatim from openomf resources/pilots.c — canonical per-pilot stats. */
void pilot_get_info(omf_pilot *pilot, int id);

/* Ported from openomf har.c:829 (src/har_logic.c). Single-player damage+stun
 * formula; writes *damage and *stun for `move` performed by attacker `obj`. */
void calc_damage_and_stun(object *obj, af_move *move, int *damage, int *stun);

typedef struct fighter_omf_view_t {
    /* Order matters: obj is the public field; the rest is private backing
     * storage referenced by obj.cur_animation / animation.sprite_table /
     * sprite.data. */
    object   obj;
    animation anim;
    sprite   cur_sprite;
    surface  cur_surface;
    /* Storage backing animation.sprite_table (single-entry table since the
     * view only describes the currently-displayed frame). */
    sprite  *sprite_table_storage[1];
    /* No hitmask in Stage A. Stage B will add a `const unsigned char *data`
     * pointer the caller sets per-frame, then cur_surface.data references it. */
} fighter_omf_view;

void omf_view_init(fighter_omf_view *v);

/* Update the view to reflect the fighter's current frame.
 *
 *   world_x/world_y: pixel anchor where the fighter is drawn (typically the
 *                    "feet" reference + HAR_ANCHOR_Y).
 *   facing:          OBJECT_FACE_LEFT (-1) or OBJECT_FACE_RIGHT (+1).
 *   sprite_w/h:      current frame dimensions.
 *   sprite_px/py:    current frame OMF-side pos offset.
 *   flip_r:          frame has 'r' (horizontal flip) tag active.
 *
 *   hitmask:         pointer to (w*h + 7) / 8 bytes — bit-packed 1 bpp,
 *                    set = opaque, clear = transparent (sessione 6 pack;
 *                    see intersect.c diff #5). NULL if this view is only
 *                    an attacker (target sampling not needed). Sampled by
 *                    intersect_har_sprite_hitpoint when this view is the
 *                    TARGET in a hitpoint call.
 *
 *   coords_data/count: pointer to a packed array of (x, y, frame_index)
 *                    int triples = per-move collision coordinates (matches
 *                    openomf collision_coord layout — 12 bytes/entry).
 *                    Pass NULL/0 if this view is only a target. Sampled by
 *                    intersect_har_sprite_hitpoint when this view is the
 *                    ATTACKER.
 */
void omf_view_update(fighter_omf_view *v,
                     int world_x, int world_y, int facing,
                     int sprite_w, int sprite_h,
                     int sprite_px, int sprite_py,
                     int flip_r,
                     unsigned char *hitmask,
                     const void *coords_data, int coords_count,
                     int cur_sprite_id);

#endif
