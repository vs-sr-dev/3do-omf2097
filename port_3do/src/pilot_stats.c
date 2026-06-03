/*
 * pilot_stats.c — pilot_get_info() ported VERBATIM from
 * openomf-master/src/resources/pilots.c.
 *
 * That function is the canonical hardcoded per-pilot stat table (openomf's own
 * comment: "TODO read this from MASTER.DAT // XXX the colors are eyeballed").
 * It is self-contained (no engine deps), so it ports directly.
 *
 * Diffs from upstream:
 *   - sd_pilot -> omf_pilot (slim shim struct, omf_runtime.h).
 *   - color_1/2/3 assignments DROPPED: pilot colors are owned by
 *     pilots_data.h (generated from ALTPALS) and used by the altpal PLUT
 *     injection path; duplicating them here would risk divergence. We keep
 *     only the fields the damage model consumes (power/agility/endurance/sex).
 *   - photo/armor zeroed (single-player; no tournament armor mitigation).
 *   - a default case added (upstream leaves out-of-range ids uninitialised);
 *     we fall back to mid-range 10/10/10 so a bad id can't read garbage power.
 */
#include "omf_runtime.h"

void pilot_get_info(omf_pilot *pilot, int id)
{
    pilot->sex   = PILOT_SEX_MALE;
    pilot->photo = 0;
    pilot->armor = 0;
    switch (id) {
    case PILOT_CRYSTAL:
        pilot->power = 5;  pilot->agility = 16; pilot->endurance = 9;
        pilot->sex = PILOT_SEX_FEMALE;
        break;
    case PILOT_STEFFAN:
        pilot->power = 13; pilot->agility = 9;  pilot->endurance = 8;
        break;
    case PILOT_MILANO:
        pilot->power = 7;  pilot->agility = 20; pilot->endurance = 4;
        break;
    case PILOT_CHRISTIAN:
        pilot->power = 9;  pilot->agility = 7;  pilot->endurance = 15;
        break;
    case PILOT_SHIRRO:
        pilot->power = 20; pilot->agility = 1;  pilot->endurance = 8;
        break;
    case PILOT_JEANPAUL:
        pilot->power = 9;  pilot->agility = 10; pilot->endurance = 11;
        break;
    case PILOT_IBRAHIM:
        pilot->power = 10; pilot->agility = 1;  pilot->endurance = 20;
        break;
    case PILOT_ANGEL:
        pilot->power = 7;  pilot->agility = 10; pilot->endurance = 13;
        pilot->sex = PILOT_SEX_FEMALE;
        break;
    case PILOT_COSSETTE:
        pilot->power = 14; pilot->agility = 8;  pilot->endurance = 8;
        pilot->sex = PILOT_SEX_FEMALE;
        break;
    case PILOT_RAVEN:
        pilot->power = 14; pilot->agility = 4;  pilot->endurance = 12;
        break;
    case PILOT_KREISSACK:
        pilot->power = 16; pilot->agility = 15; pilot->endurance = 16;
        break;
    default:                                   /* diff: safe fallback */
        pilot->power = 10; pilot->agility = 10; pilot->endurance = 10;
        break;
    }
}
