/*
 * har_logic.c — openomf combat LOGIC ported (near-)verbatim from
 * openomf-master/src/game/objects/har.c.
 *
 * Strategy (see [[project-harc-port-decision]] + [[feedback-derive-dont-reimplement]]):
 * we do NOT compile har.c wholesale — it drags in object.c / player.c /
 * game_state / hashmap / list / controller / damage_tracker. Instead each
 * ported function is copied here and operates on the slim shim "views"
 * (af_move / af / har / object / game_state in omf_runtime.h) that wrap our
 * baked fighter state. This is the intersect.c model, scaled up: the LOGIC
 * stays canonical, the platform plumbing is stubbed.
 *
 * Mirrored upstream functions and their diffs from har.c:
 *
 *   is_in_range  (har.c:421)  —  VERBATIM except:
 *     1. throw_range float math
 *          throw_range = match_settings.throw_range / 100.0f;
 *          if (object_distance(...) > successor_id * throw_range) return false;
 *        is refactored to integer (3DO has no FPU) by multiplying both sides
 *        by 100:
 *          if (object_distance(...) * 100 > successor_id * throw_range_pct)
 *        Identical result for integer distances (throw_range_pct default 100).
 *     2. af_get_move() can return 0 in our slim af (a chained next_move id may
 *        not exist in the baked table); upstream assumes it never does, so we
 *        add a NULL guard before dereferencing next_move->category.
 *
 *   calc_damage_and_stun (har.c:829)  —  near-verbatim, with:
 *     1. sd_pilot -> omf_pilot (slim shim).
 *     2. the tournament branch (is_tournament(gs) == true) is DROPPED: this
 *        port is single-player only, and that branch uses FP limb-power math
 *        the 3DO (no FPU) cannot link. The single-player path is verbatim.
 *     3. the "k" frame-tag multiplier path is preserved verbatim but inert:
 *        player_frame_isset(obj,"k") returns 0 (we don't bake the "k" tag), so
 *        multiplier stays 100 — exact for moves without a k tag.
 */
#include "omf_runtime.h"

bool is_in_range(object *obj, object *enemy_obj, af_move *move)
{
    har *h = object_get_userdata(obj);
    af_move *check_move = move;
    if (move->next_move) {
        af_move *next_move = af_get_move(h->af_data, move->next_move);
        if (next_move != 0 && next_move->category == CAT_CLOSE) { /* diff #2 */
            check_move = next_move;
        }
    }

    if (check_move->successor_id) { /* This is a throw with limited range */
        /* CLOSE moves use the successor id field as a distance requirement.
         * diff #1: integer form of `dist > successor_id * (throw_range/100)`. */
        int throw_range_pct = obj->gs->match_settings.throw_range;
        if (object_distance(obj, enemy_obj) * 100 >
            check_move->successor_id * throw_range_pct) {
            return false;
        }
    }
    return true;
}

void calc_damage_and_stun(object *obj, af_move *move, int *damage, int *stun)
{
    har *h = object_get_userdata(obj);
    game_player *gp = game_state_get_player(obj->gs, h->player_id);
    omf_pilot *pilot = gp->pilot;   /* diff #1: sd_pilot -> omf_pilot */
    int multiplier;

    if (move->category == CAT_VICTORY) {
        *damage = move->damage;
        *stun = *damage;
        return;
    }

    multiplier = 100;
    if (player_frame_isset(obj, "k")) {              /* diff #3: inert (returns 0) */
        multiplier = player_frame_get(obj, "k") + 10;
    }

    *damage = move->damage * multiplier / 100;
    if (multiplier < 100) {
        *damage = *damage + 1;
    }

    /* diff #2: tournament branch dropped (single-player port, no FPU).
     * Single Player:
     *   Damage = Base Damage * (20 + Power) / 30 + 1
     *   Stun   = Base Damage (pre-power) */
    *stun = *damage;
    *damage = *damage * (20 + pilot->power) / 30 + 1;
}
