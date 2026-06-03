/*
 * 3DO-OMF2097 — Battle MVP (Sessione 4).
 *
 * Single-fighter playable demo: Jaguar with a small state machine driven by
 * the joypad. Shadow stays as a static backdrop in his jump-flip anim.
 *
 * Fighter state machine:
 *   STANCE     play move 11 (idle) on loop
 *   WALK_L/R   play move 10 (full-body walk gait); back-walk anim + X both
 *              50% slower (mimics OMF reverse_speed:forward_speed differential)
 *   CROUCH    snap directly to last frame of move 2 (held crouch pose);
 *              persists through and after crouch attacks while Down held
 *   ATTACK     play a one-shot attack move, return to STANCE/CROUCH
 *
 * Controls:
 *   D-pad        8-way direction (feeds OMF numpad input buffer)
 *   A            punch  — data-driven via OMF move_string matcher
 *   B            kick   — same
 *   C            cycle arena background
 *   X (LShift)   toggle music
 *   Start        quit
 *
 * Button → move resolution replicates OpenOMF's match_move ([har.c:2149]):
 * a 10-deep FIFO of numpad chars (newest at [0]) collects directions; on
 * P/K press we scan gCurHarPack->moves[] for the first whose move_string's first
 * char matches the button and whose remainder matches the buffer prefix.
 * So 'P' alone → standing punch (move 42), '2' then P → crouch punch (P2,
 * move 30), '4' then P → back-punch (P4, move 27), etc.
 *
 * SFX is wired (EffectsHandler) but inaudible on Opera as of 2026-05-19 — see
 * [[project-sfx-deferred]]; left in for revisit during port of openomf har.c.
 */

#include "types.h"
#include "displayutils.h"
#include "graphics.h"
#include "celutils.h"
#include "mem.h"
#include "debug3do.h"
#include "controlpad.h"
#include "event.h"
#include "audio.h"
#include "soundplayer.h"
#include "effectshandler.h"
#include "kernel.h"
#include "time.h"

#include "har_packs.h"
#include "atlas.h"                   /* CEL-atlas loader (§1.1 memory fix) */
#include "shadow_data.h"
#include "shadow_dmg_data.h"        /* Shadow ANIM_DAMAGE (move 9) frames */

#include "scene_pilot_select.h"      /* gSelectedPilotId */
#include "scene_select.h"            /* gSelectedHarId, SELECT_HAR_* */
#include "scene_vs.h"                /* VsSceneRegister, gOpponentPilotId */
#include "intro_data.h"              /* INTRO_READY_CEL, INTRO_FIGHT_CEL, ... */

/* OMF runtime shim — wraps fighter state into the openomf object graph so
 * we can link ../openomf-master/src/game/protos/intersect.c verbatim.
 * Strategy: see [[feedback-derive-dont-reimplement]] + omf_runtime.h. */
#include "omf_runtime.h"
#include "game/protos/intersect.h"

#include "scene_dispatch.h"
#include "blit_text.h"
#include "scene_main.h"
#include "scene_pilot_select.h"
#include "scene_select.h"

/* Tiny local string helpers — avoid <string.h> because ARM SDT 2.51's libc
 * has signature mismatches with the 3DO devkit's <3do/string.h> (memchr in
 * particular). These are only called from the input matcher, not a hot
 * path. */
static void
om_memmove(char *dst, const char *src, int n)
{
    int i;
    if (dst > src) {
        for (i = n - 1; i >= 0; i--) dst[i] = src[i];
    } else {
        for (i = 0; i < n; i++) dst[i] = src[i];
    }
}

static int
om_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int
om_strncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

/* Sessione 8 altpal PoC: when (gSelectedHarId == JAGUAR &&
 * gSelectedPilotId == CRYSTAL), the host pipeline pre-generates a
 * CRYSTAL-recolored CEL set under Art/HAR_JAGUAR_CRYSTAL/. The runtime
 * rewrites cel_paths at LoadCel time so the chosen variant gets loaded.
 * Other pilot x HAR combos use the default CEL set unchanged.
 *
 * Buffer is shared across invocations (one-shot, caller must use the
 * returned pointer before the next call) -- LoadAllJaguarMoves does so. */
static char gPilotPathBuf[128];

/* PILOT_CRYSTAL (and the rest of the PILOT_* ids) now come from the canonical
 * enum in omf_runtime.h (slice 2) — local #define removed to avoid shadowing. */

static const char *
RewriteCelPathForPilot(const char *src)
{
    static const char prefix_old[] = "Art/HAR_JAGUAR/";
    static const char prefix_new[] = "Art/HAR_JAGUAR_CRYSTAL/";
    const int p_old_len = 15;        /* sizeof(prefix_old) - 1 */
    const int p_new_len = 23;        /* sizeof(prefix_new) - 1 */
    int i, src_len;
    if (src == NULL) return NULL;
    if (gSelectedHarId   != SELECT_HAR_JAGUAR) return src;
    if (gSelectedPilotId != PILOT_CRYSTAL)     return src;
    for (i = 0; i < p_old_len; i++) {
        if (src[i] != prefix_old[i]) return src;     /* not the JAGUAR set */
    }
    src_len = om_strlen(src);
    if (src_len - p_old_len + p_new_len + 1 >
        (int)sizeof(gPilotPathBuf)) {
        return src;                                   /* would overflow */
    }
    for (i = 0; i < p_new_len; i++) gPilotPathBuf[i] = prefix_new[i];
    om_memmove(&gPilotPathBuf[p_new_len], (char *)&src[p_old_len],
               src_len - p_old_len + 1);
    return gPilotPathBuf;
}

/* xorshift32 RNG -- seeded lazily on first OmfRand() from SampleSystemTime()
 * (3DO kernel monotonic counter). Sufficient for arena picking + future
 * AI dithering; not cryptographic. */
static uint32 gRngState = 0;
static uint32
OmfRand(void)
{
    uint32 x;
    if (gRngState == 0) {
        gRngState = SampleSystemTime();
        if (gRngState == 0) gRngState = 0xDEADBEEF;
    }
    x = gRngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gRngState = x;
    return x;
}

/* Format a signed int into a fixed buffer, return pointer to written start.
 * Caller-supplied buf must be at least 12 chars. Returns end-of-string ptr. */
static char *
om_itoa(char *buf, int v)
{
    char tmp[12];
    int  n = 0;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[n++] = '0';
    else while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) tmp[n++] = '-';
    /* reverse into buf */
    {
        int i;
        for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
        buf[n] = 0;
        return buf + n;
    }
}

/* Append "<label><int> " to buf, returns new end. */
static char *
om_appendkv(char *p, const char *label, int v)
{
    while (*label) *p++ = *label++;
    p = om_itoa(p, v);
    *p++ = ' ';
    *p = 0;
    return p;
}

ScreenContext *gScreenContext;
Item gDisplayItem  = -1;
Item gVBLIOReq     = -1;
Item gVRAMIOReq    = -1;
int32 gDisplayType = DI_TYPE_DEFAULT;

/* Streaming music. */
Item        gMusicSampler = 0;
Item        gMusicOutput  = 0;
SPPlayer   *gMusicPlayer  = NULL;
SPSound    *gMusicSound   = NULL;
int32       gMusicSigMask = 0;
int32       gMusicOn      = 0;

#define MUSIC_NUMBUFFERS 4
#define MUSIC_NUMBLOCKS  16
#define MUSIC_BUFSIZE    (MUSIC_NUMBLOCKS * 2048)

/* EffectsHandler SFX. mixer12x2.dsp gives us 12 input channels; each
 * non-NULL gCurHarPack->sfx[] entry gets exactly ONE channel (canonical 3DO
 * pattern verified against bounce_sound.c + silicon_relics_3do/audio.c).
 * If we have more than 12 unique effects (jaguar has 15), the extras
 * are silently dropped — gSfxEffects[param] stays NULL and SfxTrigger
 * is a no-op for that param.
 *
 * S10.6 reverted the channel-reservation attempt: intro voice now uses
 * a standalone sampler+directout chain (silicon_relics pattern) so HAR
 * sfx gets all 12 channels back. */
#define SFX_MIXER_CHANNELS 12
pTMixerInfo  gSfxMixer = NULL;
pTSampleInfo gSfxEffects[30] = {0};

/* Sessione 9 diagnostics — on-screen HUD reports SFX subsystem state so we
 * can tell from Opera (without serial printf) whether SfxStart loaded the
 * mixer/effects and whether SfxTrigger is firing during play. Updated
 * inside SfxStart / SfxTrigger and rendered in the battle main loop. */
static int  gSfxMixerErr     = 0;   /* return code from ehNewMixerInfo */
static int  gSfxLastLoadErr  = 0;   /* last failed ehLoadSoundEffect rc */
static int  gSfxLoadedCount  = 0;   /* effects successfully loaded */
static int  gSfxTrigCount    = 0;   /* SfxTrigger calls total */
static int  gSfxLastParam    = -1;  /* most-recent triggered sound_param */
static int  gSfxStartCount   = 0;   /* successful StartInstrument calls */

/* Per-move-frame CCB pool — flat, indexed via gMoveCelBase[move_idx]+frame.
 * 264 CELs total; we add slack for future expansion. */
#define JAGUAR_MAX_CELS 320
static CCB  *gJaguarCels[JAGUAR_MAX_CELS];
static int32 gMoveCelBase[HAR_MAX_MOVES];
/* Native HDX/VDY captured at load time — needed to restore after r/f flip
 * temporarily negates them. */
static int32 gJaguarHdxAbs[JAGUAR_MAX_CELS];
static int32 gJaguarVdyAbs[JAGUAR_MAX_CELS];

/* Second full HAR CEL pool — the P2 fighter (MILESTONE_1V1 §1.1 symmetric
 * fighters). Mirrors the P1 pool layout exactly; LoadHarMovesInto fills
 * either. For now it is exercised only by the §1.1 DRAM budget probe (does
 * a SECOND full moveset fit alongside P1's?); it becomes P2's live pool once
 * Shadow is promoted from dummy to a real fighter. */
static CCB  *gP2Cels[JAGUAR_MAX_CELS];
static int32 gP2MoveCelBase[HAR_MAX_MOVES];
static int32 gP2HdxAbs[JAGUAR_MAX_CELS];
static int32 gP2VdyAbs[JAGUAR_MAX_CELS];

/* CEL-atlas state (§1.1): the whole moveset loads as ONE LoadFile buffer +
 * a CCB array (atlas.c), instead of 306 page-rounded LoadCel allocations.
 * gP1IsAtlas tracks which path P1 used so unload frees correctly. */
static har_atlas_t gP1Atlas;
static har_atlas_t gP2Atlas;   /* used by the §1.1 probe re-measure */
static int         gP1IsAtlas;

/* Shadow (P2) single-move idle, same scheme as before. */
static CCB  *gShadowCels[SHADOW_FRAME_COUNT];
static int32 gShadowHdxAbs[SHADOW_FRAME_COUNT];

/* Shadow ANIM_DAMAGE (move 9) CEL pool — the hurt/stagger sprites P2 plays
 * while in the DAMAGE state. Loaded alongside the idle pool at battle init. */
static CCB  *gShadowDmgCels[SHADOW_DMG_FRAME_COUNT];
static int32 gShadowDmgHdxAbs[SHADOW_DMG_FRAME_COUNT];

/* P2 hit reaction — a faithful replay of openomf's knockback (see
 * [[project-harc-hitfeel-knockback]]). On a connecting hit, har_take_damage()
 * sets the victim to ANIM_DAMAGE and plays the ATTACKER move's footer_string
 * over it as a "custom string" (har.c:980/1032). Each footer frame picks an
 * ANIM_DAMAGE sprite, holds tick_len ticks, and either:
 *   - is_vel=0: shifts the victim by a ONE-SHOT world x delta (player.c:446),
 *   - is_vel=1: sets a VELOCITY (px/tick) the object integrates with friction
 *               (-1/tick, snap to 0 below 2 — har.c:744-755).
 * That authored trajectory IS the knockback: light hits = a 3-frame in-place
 * stagger (dx 0); heavy hits set a small backward velocity that slides ~15px
 * smoothly then a knockdown sprite; throws use big NEGATIVE deltas that pull
 * the victim toward the attacker (the overhead-lift grab). We bake the track
 * per move (har_recoil_move_t) and replay it here verbatim — NO synthesised
 * decay. Two liberties for our fixed dummy: (1) long knockdown holds (300-500
 * ticks) are clamped to P2_RECOIL_TICK_MAX; (2) since P2 has no walk logic to
 * carry it home, a RECOVER phase eases the residual offset back to its anchor
 * rather than snapping. */
/* Hit-reaction sub-FSM states (per-fighter, stored in fighter.hitState). The
 * P2_* names are kept for continuity with [[project-harc-hitfeel-knockback]];
 * the actual recoil cursor/velocity fields now live in the fighter struct so
 * EITHER fighter can be the victim (§1.1 bidirectional). The footer 'v' launch
 * + LAUNCHER vertical arc + ground friction are unchanged — see FighterAdvance /
 * FighterRecoilEnterFrame. */
enum p2_state { P2_IDLE = 0, P2_DAMAGE = 1, P2_RECOVER = 2 };
#define P2_RECOIL_TICK_MAX 24        /* clamp one track frame (knockdown holds are huge) */
#define P2_RECOVER_STEP     2        /* px/tick the victim eases back to its anchor */
#define P2_NOTRACK_TICKS    6        /* in-place stagger length if a move has no track */

/* Sessione 9 health bars. Two 100x6 CELs (red FG + dark-grey BG) live in
 * takeme/Art/hpbar_{fg,bg}.CEL (generated once by host_tools/gen_hpbar_cels.sh).
 * At render time we scale the FG horizontally via ccb_HDX so visible bar
 * width = 100 * hp/max. P1's bar shrinks from the right; P2's shrinks from
 * the left -- both anchor toward the center, matching the OMF DOS HUD.
 *
 * We LoadCel both CELs twice (P1/P2 instances) so we can chain all four
 * bars into a single CCB list and emit one DrawCels call instead of four. */
static CCB  *gHpBarBgP1 = NULL;
static CCB  *gHpBarBgP2 = NULL;
static CCB  *gHpBarFgP1 = NULL;
static CCB  *gHpBarFgP2 = NULL;
/* 3it-packed CELs don't always come back with ccb_HDX==0x10000; the value
 * depends on the packing mode + bpp. Capture it once at LoadCel so the
 * per-frame scale = (hp * nativeHdx) / hpMax produces the correct screen
 * width without assuming any specific encoding. */
static int32 gHpBarFgNativeHdx = 0x10000;
#define HP_BAR_W       100        /* CEL source width, px */
#define HP_BAR_H         6
#define HP_BAR_Y         4
#define HP_BAR_P1_X     10        /* left edge of P1 bar */
#define HP_BAR_P2_RIGHT 310       /* right edge of P2 bar (anchor) */
/* Both fighters' health now lives in gF[i].hp/hpMax. */

/* K.O. flow: when either HP hits 0, freeze fight, show banner for KO_FIELDS.
 * §1.3 (MILESTONE_1V1): the first KO now ENDS the match (single round for 0.1)
 * — the banner becomes the result and at countdown end we return to the menu
 * instead of resetting and resuming. gMatchOver latches the round-end so the KO
 * handler takes the exit path; gWinnerIsP1 picks the YOU WIN / YOU LOSE text.
 * Best-of-3 rounds are a 1.0 item. */
#define KO_FIELDS  (60 * 3)       /* 3 seconds at NTSC field rate */
static int32 gKoCountdown = 0;
static const char *gKoBanner = NULL;
static int32 gMatchOver  = 0;     /* set on the match-ending KO */
static int32 gWinnerIsP1 = 0;     /* 1 = human won, 0 = AI won */

/* Sessione 10.6 intro lock: READY + FIGHT sprites (BK anim[10/11]).
 * FIGHT voice (SOUNDS.DAT[10]) uses the silicon_relics "standalone
 * sampler" pattern (fixedstereosample.dsp + directout.dsp + LoadSample
 * + AttachSample) -- avoids the ehLoadSoundEffect-on-running-mixer
 * issue that returned 0xD52BF113 in our first iteration. */
static CCB  *gIntroReadyCel    = NULL;
static CCB  *gIntroFightCel    = NULL;
static Item  gIntroFightSampler = 0;
static Item  gIntroFightOutput  = 0;
static Item  gIntroFightSample  = 0;
static Item  gIntroFightAttach  = 0;

/* Arena selector — 5 OMF arenas pre-loaded, L/R shoulder cycles. */
#define ARENA_COUNT 5
static CCB *gArenaCels[ARENA_COUNT];
static const char *gArenaPaths[ARENA_COUNT] = {
    "Art/ARENA0.CEL", "Art/ARENA1.CEL", "Art/ARENA2.CEL",
    "Art/ARENA3.CEL", "Art/ARENA4.CEL"
};
static int32 gCurArenaIdx = 0;
#define gArenaCel (gArenaCels[gCurArenaIdx])

/* OMF logical tick rate vs 3DO video field rate. 2 fields/tick = ~30 Hz. */
static int32 gOmfFieldsPerTick = 2;

/* Fighter state machine. */
enum fighter_state {
    ST_STANCE = 0,
    ST_WALK_L,
    ST_WALK_R,
    ST_CROUCH,
    ST_ATTACK,
    ST_JUMP
};

/* The former per-fighter state (fighter_t gJaguar, gIdx* anim slots, jump
 * physics, input buffer, OMF logic backing, hp) is now folded into the
 * `fighter` struct defined further down (after INPUT_BUF_LEN), instantiated as
 * gF[2] (gF[0]=P1, gF[1]=P2) — MILESTONE_1V1 §1.1 symmetric fighters. */

/* P2's opponent pack (the other HAR). Its CELs live in gP2Cels (atlas-loaded);
 * its anim-slot indices + recoil state are now in gF[1] (the unified fighter).
 * gP2HarPack is kept because the atlas load + unload reference it directly. */
static const har_pack_t *gP2HarPack = NULL;
static int   gP2IsAtlas = 0;   /* P2 moveset loaded via atlas (vs Shadow fallback) */

/* ---------------- Jump physics (ported from openomf har.c) ----------------
 * OMF runs physics per TICK in floats: an upward jump sets vel.y negative,
 * gravity adds back each tick (terminal velocity 13), pos.y integrates until
 * it returns to ARENA_FLOOR (landing). We mirror that here in fixed-point; the
 * one-shot per-fight derivation of the scaled speeds happens in
 * BattleSceneInit from the active pack's AF scalars + pilot agility, using
 * the canon formula at har.c:2716. See [[project-harc-port-decision]].
 *
 * Geometry: gJumpY is the FEET offset above ARENA_FLOOR (0 = grounded, <0 =
 * airborne; screen Y adds it). gJumpXAbs is the live world X while airborne,
 * mirrored back into gJaguar.x_offset each tick so the existing anchorX /
 * hit-detection paths keep working unchanged.
 *
 * All quantities are FIXED-POINT x256 (8 fractional bits). The 3DO has no
 * FPU and the firmware links with -noscanlib (no float runtime), so the
 * physics stays integer -- the same convention as the AF scalars above. */
#define FP_SHIFT          8
#define FP_ONE            256
#define TERMINAL_VY_X256  (13 * FP_ONE)   /* har.c terminal velocity = 13 px/tick */

/* openomf shifts EVERY ANIM_JUMPING frame's pos_y by -60 at AF-load time
 * (af_move.c:23, animation_fixup_coordinates(ani,0,-JUMP_COORD_ADJUSTMENT);
 * JUMP_COORD_ADJUSTMENT=60). Our pipeline baked the RAW jump frames, so the
 * jump pose sits 60px too low. We re-apply the same constant only while
 * airborne (the jump anim has no hit-coords here, so the render-only shift
 * is equivalent to the load-time fixup). */
#define JUMP_COORD_ADJUSTMENT  60

/* Jump physics state (gJumpVy/Vx/Y/XAbs/Field/Delay), the per-fight scaled
 * speeds (jump/superjump/gravity/fwd/back), the per-frame sprite Y offset, and
 * the air-attack flags (jumpAttacking/airAttacked) are now per-fighter members
 * of the `fighter` struct. Air attack: CAT_JUMPING requires is_har_idle_air &&
 * !air_attacked (openomf is_move_chain_allowed) — one swing per jump. */

/* OMF-style input buffer (numpad notation, newest at [0]). Mirrors
 * openomf-master/src/game/objects/har.c h->inputs[10]. Direction chars are
 * '1'..'9' (numpad layout, '5' = neutral). Idle state pushes '5'. */
#define INPUT_BUF_LEN       10
#define INPUT_STALE_LIMIT   18   /* fields; ≈ 9 OMF ticks at gOmfFieldsPerTick=2 */
/* Input buffer (inBuf), staleness (inStale) and facing (facingRight) are now
 * per-fighter members of the `fighter` struct. */

/* Battle-stage geometry derived from openomf canon (src/game/scenes/arena.c:466
 * and src/game/objects/arena_constraints.h):
 *   P1 start  = (110, 190)
 *   P2 start  = (210, 190)
 *   ARENA_FLOOR Y = 190 in the OMF 320x200 canvas.
 * On 3DO (320x240 NTSC) the arena CEL is drawn at screen Y=20 to center the
 * 200-tall canvas, so the OMF feet-Y 190 maps to screen Y 210. obj->pos is a
 * FEET anchor: rendering offsets sprite.pos relative to it.
 *
 * JAG_MAX_X picked so Jaguar walking forward gets bbox overlap with Shadow at
 * the canonical 100-pixel separation. The hard wall ARENA_RIGHT_WALL=300 is
 * not relevant since P2 blocks first. */
#define JAG_BASE_X      110   /* OMF P1 spawn X */
#define JAG_MAX_X       190   /* close enough to P2's bbox to overlap */
#define JAG_WALK_SPEED  1     /* pixels per video field */
#define P2_ANCHOR_X     210   /* OMF P2 spawn X */
#define HAR_ANCHOR_Y    210   /* OMF ARENA_FLOOR(190) + screen offset(20) */


/* ---------------- Per-fighter state (MILESTONE_1V1 §1.1) ----------------
 * One struct, two instances: gF[0]=P1, gF[1]=P2. This folds every former
 * P1-specific global (the old fighter_t gJaguar, gIdx anim slots, gJump
 * physics, gInput/gFacing input, gAf/gJagView OMF backing, gP1Hp) PLUS the
 * hit-reaction recoil sub-FSM (former gP2* recoil globals) into a single value
 * so the SAME update code drives both fighters. The large per-fighter CEL
 * pools (gJaguarCels / gP2Cels) stay module statics; the struct points into
 * them. The shared game_state (throw range + both players' pilots) and the af
 * move-table storage live in module statics below, referenced by the struct.
 * See plan + [[project-harc-port-decision]] / [[feedback-derive-dont-reimplement]]. */
typedef struct fighter {
    /* normal FSM (was fighter_t gJaguar) */
    enum fighter_state state;
    int32 move_idx;          /* index into pack->moves[] */
    int32 cur_step;
    int32 field_in_step;
    int32 x_offset;          /* px offset from baseX; >0 = right of anchor */
    int32 walk_phase;

    /* jump physics (was gJump*), all x256 fixed-point */
    int32 jVy, jVx, jY, jXAbs, jField, jDelay, yOff;
    int32 jumpAttacking, airAttacked;
    int32 jumpSpeed, superjumpSpeed, gravity, fwdSpeed, backSpeed;

    /* hit-reaction sub-FSM (was gP2* recoil globals). hitState ==
     * P2_IDLE(0)/P2_DAMAGE/P2_RECOVER. */
    int32 hitState;
    const har_recoil_move_t *track;
    int32 trackFrame, tickInFrame, fieldInTick;
    int32 recoilOff, recoilVel, recoilY, recoilVy, dmgSprite;

    /* input seam (was gInputBuf/gInputStale/gFacingRight). pad/lastPad are the
     * abstract controller bits this fighter consumes — real ControlPad for P1,
     * synthesised by the driver/AI for P2. */
    char   inBuf[INPUT_BUF_LEN + 1];
    int32  inStale;
    int    facingRight;
    uint32 pad, lastPad;

    /* combat */
    int32 attackHasHit;      /* single-hit-per-attack gate */
    int32 hp, hpMax;
    int32 baseX;             /* world anchor X (110 P1 / 210 P2) */

    /* per-pack bindings */
    const har_pack_t *pack;
    int32 idxIdle, idxWalk, idxCrouch, idxJump, idxDmg;
    CCB  **cels;             /* -> gJaguarCels / gP2Cels */
    int32 *moveCelBase, *hdxAbs, *vdyAbs;

    /* OMF logic backing (was gAf, gAfMoves, gOmfHar, gP1Pilot, gJagView). afData +
     * afMoves point into the shared pools below; gs is the shared game_state. */
    af        *afData;
    af_move   *afMoves;
    har        omfHar;
    omf_pilot  pilot;
    game_state *gs;
    fighter_omf_view view;
} fighter;

static fighter gF[2];

/* Shared OMF logic storage. The af move-table is per-fighter (each HAR has its
 * own moves) so we keep two; the game_state is shared (it holds BOTH players'
 * pilots + the match-wide throw range), exactly like openomf. */
static af_move    gAfMovesPool[2][HAR_MAX_MOVES];
static af         gAfPool[2];
static game_state gOmfGameState;

/* Hit-pause ("impact freeze"): on a connecting hit, hold both fighters'
 * poses for a few fields so the blow reads as meaty. Canon-inspired by
 * openomf's game_state_slowdown on hits (har.c:968); here it's a simple
 * local freeze of the timeline advancement, not the full physics slowdown. */
#define HIT_PAUSE_FIELDS 4
static int32 gHitPauseFields = 0;

/* ---------------- Audio init ---------------- */
static int32
Initialize(void)
{
    int32 err;
    gScreenContext = (ScreenContext *)AllocMem(sizeof(ScreenContext), MEMTYPE_ANY);
    if (!gScreenContext) { printf("AllocMem fail\n"); return -1; }
    err = OpenGraphicsFolio();
    if (err < 0) { printf("OpenGraphicsFolio fail: %d\n", err); return err; }
    gDisplayType = GetDisplayType();
    if (gDisplayType < 0) { printf("GetDisplayType fail\n"); return -1; }
    if (gDisplayType == DI_TYPE_PAL1 || gDisplayType == DI_TYPE_PAL2) {
        gDisplayType = DI_TYPE_PAL2;
    } else {
        gDisplayType = DI_TYPE_NTSC;
    }
    gDisplayItem = CreateBasicDisplay(gScreenContext, gDisplayType, 2);
    if (gDisplayItem < 0) { printf("CreateBasicDisplay fail\n"); return -1; }
    if (InitControlPad(1) < 0) { printf("InitControlPad fail\n"); return -1; }
    gVBLIOReq  = GetVBLIOReq();
    gVRAMIOReq = GetVRAMIOReq();
    if (gVBLIOReq < 0 || gVRAMIOReq < 0) { printf("IOReq fail\n"); return -1; }
    err = OpenAudioFolio();
    if (err < 0) { printf("OpenAudioFolio fail: %d\n", err); return err; }
    return 0;
}

void
MusicStart(const char *filename)
{
    Err err;
    gMusicSampler = LoadInstrument("fixedstereosample.dsp", 0, 100);
    if (gMusicSampler < 0) { gMusicSampler = 0; return; }
    gMusicOutput = LoadInstrument("directout.dsp", 0, 100);
    if (gMusicOutput < 0) goto fail_output;
    err = ConnectInstruments(gMusicSampler, "LeftOutput",  gMusicOutput, "InputLeft");
    if (err < 0) goto fail_connect;
    err = ConnectInstruments(gMusicSampler, "RightOutput", gMusicOutput, "InputRight");
    if (err < 0) goto fail_connect;
    err = StartInstrument(gMusicOutput, NULL);
    if (err < 0) goto fail_connect;
    err = spCreatePlayer(&gMusicPlayer, gMusicSampler, MUSIC_NUMBUFFERS, MUSIC_BUFSIZE, NULL);
    if (err < 0) goto fail_connect;
    err = spAddSoundFile(&gMusicSound, gMusicPlayer, filename);
    if (err < 0) goto fail_sound;
    spLoopSound(gMusicSound);
    gMusicSigMask = spGetPlayerSignalMask(gMusicPlayer);
    err = spStartReading(gMusicSound, SP_MARKER_NAME_BEGIN);
    if (err < 0) goto fail_sound;
    err = spStartPlaying(gMusicPlayer, NULL);
    if (err < 0) goto fail_sound;
    spService(gMusicPlayer, 0);
    gMusicOn = 1;
    return;
fail_sound:
    spDeletePlayer(gMusicPlayer); gMusicPlayer = NULL; gMusicSound = NULL;
fail_connect:
    UnloadInstrument(gMusicOutput); gMusicOutput = 0;
fail_output:
    UnloadInstrument(gMusicSampler); gMusicSampler = 0;
}

void
MusicStop(void)
{
    if (!gMusicOn) return;
    spStop(gMusicPlayer);
    spDeletePlayer(gMusicPlayer);
    gMusicPlayer = NULL; gMusicSound = NULL;
    UnloadInstrument(gMusicOutput); gMusicOutput = 0;
    UnloadInstrument(gMusicSampler); gMusicSampler = 0;
    gMusicSigMask = 0;
    gMusicOn = 0;
}

/* Halt DSP playback but leave the player/instruments/buffer allocated.
 * After this, MusicService is effectively a no-op (no signal pending)
 * and the user hears silence -- but the 128 KB ring buffer stays in
 * place, so a later MusicStop+MusicStart can reuse the same memory
 * without the audio_load_ordering trap (see [[project-audio-load-ordering]]). */
void
MusicPause(void)
{
    if (!gMusicOn || gMusicPlayer == NULL) return;
    spStop(gMusicPlayer);
    /* Leave gMusicOn = 1 so MusicService still gates correctly; but
     * the DSP is halted so no buffer drain occurs. We also leave
     * gMusicSigMask alone -- the next MusicService call will see no
     * pending signals and return immediately. */
}

void
MusicService(void)
{
    int32 pending;
    if (!gMusicOn) return;
    pending = GetCurrentSignals() & gMusicSigMask;
    if (pending) {
        WaitSignal(pending);
        spService(gMusicPlayer, pending);
    }
}

/* Initialize the EffectsHandler mixer + assign each non-NULL Jaguar SFX
 * its own dedicated mixer channel (canonical 3DO pattern — multiple
 * effects on the same channel just doesn't work). */
static void
SfxStart(void)
{
    int i;
    int next_channel = 0;
    Err err;
    gSfxLoadedCount = 0;
    gSfxLastLoadErr = 0;
    err = ehNewMixerInfo(&gSfxMixer, SFX_MIXER_CHANNELS, "mixer12x2.dsp");
    gSfxMixerErr = (int)err;
    if (err < 0 || gSfxMixer == NULL) {
        printf("ehNewMixerInfo failed: %d\n", (int)err);
        gSfxMixer = NULL;
        return;
    }
    for (i = 0; i < 30; i++) {
        const har_sfx_t *e = &gCurHarPack->sfx[i];
        if (e->aiff_path == NULL) continue;
        if (next_channel >= SFX_MIXER_CHANNELS) {
            printf("SFX channel exhausted, dropping param=%d (%s)\n",
                   i, e->aiff_path);
            continue;
        }
        err = ehLoadSoundEffect(&gSfxEffects[i], gSfxMixer,
                                (char *)e->aiff_path, next_channel);
        if (err < 0 || gSfxEffects[i] == NULL) {
            printf("ehLoadSoundEffect(%s) ch=%d failed: %d\n",
                   e->aiff_path, next_channel, (int)err);
            gSfxLastLoadErr = (int)err;
            gSfxEffects[i] = NULL;
            continue;
        }
        /* Volume passed to ehSetChannelLevels is internally divided by
         * mi_ChannelsUsed (12 here) -> per-channel gain max = 0x7FFF/12
         * = ~2700 = -22 dB. Compensate by pre-multiplying so each channel
         * actually hits MAXDSPAMPLITUDE when triggered. */
        ehSetChannelLevels(gSfxMixer,
                           gSfxEffects[i]->si_LeftGainKnob,
                           gSfxEffects[i]->si_RightGainKnob,
                           0x7FFF * SFX_MIXER_CHANNELS, kEqualBalance);
        next_channel++;
        gSfxLoadedCount++;
    }
    printf("SFX: mixer12x2 ready, %d effects on channels 0..%d\n",
           next_channel, next_channel - 1);
}

/* Convert OMF anim_string `sf` pitch (-128..+128, clamped to -20..+20 in
 * canonical openomf range) to a 3DO AF_TAG_PITCH MIDI-note value.
 *
 * Canonical openomf formula (src/audio/audio.c:272):
 *   pitch > 0: effective_freq = src_freq * (100 + 3*pitch) / 100   -> ratio up to ~1.6
 *   pitch < 0: effective_freq = src_freq * (100 + 2*pitch) / 100   -> ratio down to ~0.6
 *   clamp pitch >= -20
 *
 * 3DO AF_TAG_PITCH is MIDI-note (60 = native rate = middle C, +12 = 1 octave).
 * 12 * log2(1.30) = 4.5 semitones, 12 * log2(0.94) = -1.07, etc.
 * Linear approximation across the OMF range: ~0.4 semitones per pitch unit. */
static int
OmfPitchToMidi(int omf_pitch)
{
    int semi;
    if (omf_pitch < -20) omf_pitch = -20;
    if (omf_pitch >  20) omf_pitch =  20;
    semi = (omf_pitch * 4) / 10;
    return 60 + semi;
}

static void
SfxTrigger(int sound_param, int omf_pitch)
{
    pTSampleInfo eff;
    Err err;
    TagArg startTags[2];
    gSfxTrigCount++;
    gSfxLastParam = sound_param;
    if (gSfxMixer == NULL) return;
    if (sound_param < 0 || sound_param >= 30) return;
    eff = gSfxEffects[sound_param];
    if (eff == NULL) return;
    /* Fire the sample at the OMF-specified pitch. StartInstrument on a
     * still-playing player just restarts it — fine for back-to-back hits. */
    startTags[0].ta_Tag = AF_TAG_PITCH;
    startTags[0].ta_Arg = (void *)OmfPitchToMidi(omf_pitch);
    startTags[1].ta_Tag = TAG_END;
    startTags[1].ta_Arg = NULL;
    err = StartInstrument(eff->si_Player, startTags);
    if (err >= 0) gSfxStartCount++;
}

/* ---------------- S10.6 intro lock helpers ---------------- */

/* S10.6 audio TODO: three approaches tried, all silent on Opera.
 * Voice plays NOWHERE despite valid AIFF + working visual lock + working
 * BGM. Need a fresh investigation in S10.7+.
 *
 * Attempts tried (all failed silently or with no audible output):
 *   1. Dedicated 2x2 mixer + ehLoadSoundEffect (mixer2x2.dsp, ch 0).
 *      No load-error, no sound. Untested whether StartInstrument fires.
 *   2. Piggy-back on existing 12x2 HAR mixer at next-free channel.
 *      ehLoadSoundEffect returned 0xD52BF113 (non-std EH/user error;
 *      not in AF_ERR_* / EHNO* tables). Also killed arena BGM until
 *      we reordered IntroSfxStart to BEFORE MusicStart.
 *   3. Reserve channel 11 explicitly (SFX_HAR_CHANNELS=11). Same load
 *      error 0xD52BF113. Identical with a known-working Jaguar AIFF
 *      path, so the issue is NOT the source file -- it's calling
 *      ehLoadSoundEffect AFTER SfxStart already finished its loop.
 *   4. Standalone sampler+directout (silicon_relics bip pattern,
 *      audio.c L169-196). Silent on Opera. The chain initializes
 *      without errors but StartInstrument(sampler) produces no audio.
 *
 * Visual lock (READY + gap + FIGHT) is fully working and ships in S10.6.
 * FIGHT.aiff + sprites are on the ISO. The pipeline produces both.
 * What needs investigation next time:
 *   - Compare against working silicon_relics output for missing init
 *     (does it need OpenAudioFolio explicitly? Some priority arg?)
 *   - Check if directout is conflicting with the BGM's directout
 *     (only one directout instance per system maybe?)
 *   - Try LoadSoundFile (sf_*) family used by SP for one-shot. */
static void
IntroSfxStart(void)
{
    Err err;

    gIntroFightSampler = LoadInstrument("fixedstereosample.dsp", 0, 50);
    if (gIntroFightSampler < 0) { gIntroFightSampler = 0; return; }

    gIntroFightOutput = LoadInstrument("directout.dsp", 0, 50);
    if (gIntroFightOutput < 0) goto fail_output;

    err = ConnectInstruments(gIntroFightSampler, "LeftOutput",
                             gIntroFightOutput, "InputLeft");
    if (err < 0) goto fail_connect;
    err = ConnectInstruments(gIntroFightSampler, "RightOutput",
                             gIntroFightOutput, "InputRight");
    if (err < 0) goto fail_connect;

    gIntroFightSample = LoadSample((char *)INTRO_FIGHT_AIFF);
    if (gIntroFightSample < 0) goto fail_connect;

    gIntroFightAttach = AttachSample(gIntroFightSampler,
                                     gIntroFightSample, NULL);
    if (gIntroFightAttach < 0) goto fail_sample;

    err = StartInstrument(gIntroFightOutput, NULL);
    if (err < 0) goto fail_sample;

    printf("IntroSfx: FIGHT voice ready (standalone sampler+output)\n");
    return;

fail_sample:
    UnloadSample(gIntroFightSample); gIntroFightSample = 0;
fail_connect:
    UnloadInstrument(gIntroFightOutput); gIntroFightOutput = 0;
fail_output:
    UnloadInstrument(gIntroFightSampler); gIntroFightSampler = 0;
}

static void
IntroSfxTriggerFight(void)
{
    if (gIntroFightSampler == 0) return;
    /* StartInstrument on the sampler restarts the sample from t=0.
     * NULL tag list -- default pitch (= native sample rate, 8 kHz). */
    StartInstrument(gIntroFightSampler, NULL);
}

static void
IntroSfxShutdown(void)
{
    if (gIntroFightSample) { UnloadSample(gIntroFightSample); gIntroFightSample = 0; }
    if (gIntroFightOutput) { UnloadInstrument(gIntroFightOutput); gIntroFightOutput = 0; }
    if (gIntroFightSampler){ UnloadInstrument(gIntroFightSampler); gIntroFightSampler = 0; }
    gIntroFightAttach = 0;
}

/* ---------------- CEL setup ---------------- */

/* Generalized HAR moveset loader — fills any pool (P1 or P2) from any pack.
 * Returns the number of CEL slots consumed, or -1 on failure. Missing frames
 * share the previous CCB pointer (cross-move pixel-data refs); their HDX/VDY
 * mirror the previous owner's. base[m] records each move's first slot.
 * This is the §1.1 seam: one loader serves both fighters. */
static int32
LoadHarMovesInto(const har_pack_t *pack, CCB **pool, int32 *base,
                 int32 *hdx, int32 *vdy, int32 poolMax)
{
    int32 m, f, cursor = 0;
    CCB *prev;

    for (m = 0; m < pack->moves_count; m++) {
        const har_move_t *mv = &pack->moves[m];
        base[m] = cursor;
        prev = NULL;
        for (f = 0; f < mv->frame_count; f++) {
            const har_frame_t *fr = &mv->frames[f];
            if (cursor >= poolMax) {
                printf("CEL pool exhausted at move=%d frame=%d\n", mv->id, f);
                return -1;
            }
            if (fr->missing) {
                pool[cursor] = prev;
                hdx[cursor] = (prev != NULL) ? prev->ccb_HDX : 0;
                vdy[cursor] = (prev != NULL) ? prev->ccb_VDY : 0;
            } else {
                const char *cel_path = RewriteCelPathForPilot(fr->cel_path);
                pool[cursor] = LoadCel((char *)cel_path, MEMTYPE_ANY);
                if (pool[cursor] == NULL) {
                    printf("LoadCel(%s) failed\n", cel_path);
                    return -1;
                }
                hdx[cursor] = pool[cursor]->ccb_HDX;
                vdy[cursor] = pool[cursor]->ccb_VDY;
                prev = pool[cursor];
            }
            cursor++;
            /* Keep the BGM streamer fed during the multi-second CEL
             * load storm (~300 LoadCel calls). Without this, the menu
             * BGM buffer drains halfway through and goes silent until
             * we re-enter the battle main loop. */
            if ((cursor & 7) == 0) MusicService();
        }
    }
    return cursor;
}

/* Symmetric unload: walk the same frame table and call UnloadCel ONLY on
 * slots that actually own a CEL (non-missing). Missing frames share the
 * previous CCB pointer -- unloading them twice would corrupt the heap. */
static void
UnloadHarMovesFrom(const har_pack_t *pack, CCB **pool)
{
    int32 m, f, cursor = 0;
    for (m = 0; m < pack->moves_count; m++) {
        const har_move_t *mv = &pack->moves[m];
        for (f = 0; f < mv->frame_count; f++) {
            const har_frame_t *fr = &mv->frames[f];
            if (!fr->missing && pool[cursor] != NULL) {
                UnloadCel(pool[cursor]);
            }
            pool[cursor] = NULL;
            cursor++;
        }
    }
}

/* Map a HAR pack to its atlas file on the ISO. */
static const char *
HarPackAtlasPath(const har_pack_t *pack)
{
    if (pack == &gThornPack) return "Art/HAR_THORN.ATL";
    return "Art/HAR_JAGUAR.ATL";   /* Jaguar default / fallback */
}

/* §1.4: build the per-pilot PLUT path "Art/pluts/HAR_<NAME>_<pid>.PAL" for a
 * (pack, pilot_id). pilot_id must be 0..9 (the 10 playable pilots). Returned in
 * a shared static buffer — use before the next call. */
static const char *
HarPilotPlutPath(const har_pack_t *pack, int32 pilot_id)
{
    static char buf[48];
    const char *name = (pack == &gThornPack) ? "THORN" : "JAGUAR";
    const char *pfx  = "Art/pluts/HAR_";
    int i = 0;
    while (*pfx)  buf[i++] = *pfx++;
    while (*name) buf[i++] = *name++;
    buf[i++] = '_';
    buf[i++] = (char)('0' + (pilot_id % 10));
    buf[i++] = '.'; buf[i++] = 'P'; buf[i++] = 'A'; buf[i++] = 'L';
    buf[i]   = 0;
    return buf;
}

/* §1.4: apply a fighter's pilot palette to its atlas, if the pilot id is one of
 * the 10 playable pilots that have baked .PAL blobs. Out-of-range (e.g. -1 or
 * NOVA=10) leaves the moveset in its neutral colors. */
static void
ApplyFighterPilotPlut(har_atlas_t *atl, const har_pack_t *pack, int32 pilot_id)
{
    if (pilot_id < 0 || pilot_id > 9) {
        printf("pilot plut: pilot %d out of range -> neutral colors\n",
               (int)pilot_id);
        return;
    }
    AtlasApplyPilotPlut(atl, HarPilotPlutPath(pack, pilot_id));
}

/* Load a moveset from its .ATL into `pool`, mapping atlas frames onto the pack's
 * move/frame walk (missing frames reuse the previous CCB, exactly like the
 * per-cel loader). The atlas holds only non-missing frames, in this same walk
 * order, so a simple advancing index lines them up. Returns cels consumed, or
 * -1 on failure (atlas left freed). */
static int32
LoadHarFromAtlas(const har_pack_t *pack, har_atlas_t *atl, const char *path,
                 CCB **pool, int32 *base, int32 *hdx, int32 *vdy, int32 poolMax)
{
    int32 m, f, cursor = 0, aidx = 0;
    CCB *prev;

    if (AtlasLoad(path, atl) < 0) return -1;

    for (m = 0; m < pack->moves_count; m++) {
        const har_move_t *mv = &pack->moves[m];
        base[m] = cursor;
        prev = NULL;
        for (f = 0; f < mv->frame_count; f++) {
            const har_frame_t *fr = &mv->frames[f];
            if (cursor >= poolMax) {
                printf("atlas pool exhausted at move=%d frame=%d\n", mv->id, f);
                AtlasFree(atl);
                return -1;
            }
            if (fr->missing) {
                pool[cursor] = prev;
                hdx[cursor] = (prev != NULL) ? prev->ccb_HDX : 0;
                vdy[cursor] = (prev != NULL) ? prev->ccb_VDY : 0;
            } else {
                CCB *c;
                if (aidx >= atl->frame_count) {
                    printf("atlas %s idx overflow %d/%d (table mismatch?)\n",
                           path, (int)aidx, (int)atl->frame_count);
                    AtlasFree(atl);
                    return -1;
                }
                c = &atl->ccbs[aidx++];
                pool[cursor] = c;
                hdx[cursor] = c->ccb_HDX;
                vdy[cursor] = c->ccb_VDY;
                prev = c;
            }
            cursor++;
        }
    }
    if (aidx != atl->frame_count) {
        printf("atlas %s WARN: walk used %d of %d frames\n",
               path, (int)aidx, (int)atl->frame_count);
    }
    return cursor;
}

/* P1 wrappers — prefer the atlas (ONE allocation), fall back to the per-cel
 * loader if the .ATL is absent/bad so the game still runs. */
static int32
LoadAllJaguarMoves(void)
{
    int32 n = LoadHarFromAtlas(gCurHarPack, &gP1Atlas,
                               HarPackAtlasPath(gCurHarPack),
                               gJaguarCels, gMoveCelBase,
                               gJaguarHdxAbs, gJaguarVdyAbs, JAGUAR_MAX_CELS);
    if (n >= 0) {
        gP1IsAtlas = 1;
        printf("Loaded %s via ATLAS: %d frames -> %d cels\n",
               gCurHarPack->name, (int)gP1Atlas.frame_count, (int)n);
        return 0;
    }
    printf("ATLAS load failed for %s -> per-cel fallback\n", gCurHarPack->name);
    gP1IsAtlas = 0;
    n = LoadHarMovesInto(gCurHarPack, gJaguarCels, gMoveCelBase,
                         gJaguarHdxAbs, gJaguarVdyAbs, JAGUAR_MAX_CELS);
    if (n < 0) return -1;
    printf("Loaded %d HAR CELs (%s) across %d moves\n",
           (int)n, gCurHarPack->name, (int)gCurHarPack->moves_count);
    return 0;
}

static void
UnloadAllJaguarMoves(void)
{
    if (gP1IsAtlas) {
        int32 m, f, cursor = 0;
        /* Pool entries pointed into the atlas CCB array; just NULL them, then
         * free the atlas (buffer + CCB array) in one shot — NOT UnloadCel. */
        for (m = 0; m < gCurHarPack->moves_count; m++) {
            const har_move_t *mv = &gCurHarPack->moves[m];
            for (f = 0; f < mv->frame_count; f++) gJaguarCels[cursor++] = NULL;
        }
        AtlasFree(&gP1Atlas);
        gP1IsAtlas = 0;
    } else {
        UnloadHarMovesFrom(gCurHarPack, gJaguarCels);
    }
}

static int32
LoadShadow(void)
{
    int32 i;
    CCB *prev = NULL;
    int32 prevHdx = 0;
    for (i = 0; i < (int32)SHADOW_FRAME_COUNT; i++) {
        if (shadow_frames[i].missing) {
            gShadowCels[i] = prev;
            gShadowHdxAbs[i] = prevHdx;
            continue;
        }
        gShadowCels[i] = LoadCel((char *)shadow_frames[i].cel_path, MEMTYPE_ANY);
        if (!gShadowCels[i]) return -1;
        gShadowHdxAbs[i] = gShadowCels[i]->ccb_HDX;
        prev = gShadowCels[i];
        prevHdx = gShadowHdxAbs[i];
    }
    return 0;
}

/* Symmetric unload for Shadow CELs. Same missing-frame caveat as
 * UnloadAllJaguarMoves(). */
static void
UnloadShadow(void)
{
    int32 i;
    for (i = 0; i < (int32)SHADOW_FRAME_COUNT; i++) {
        if (!shadow_frames[i].missing && gShadowCels[i] != NULL) {
            UnloadCel(gShadowCels[i]);
        }
        gShadowCels[i] = NULL;
    }
}

/* ANIM_DAMAGE pool load/unload — mirror of LoadShadow/UnloadShadow over the
 * shadow_dmg_frames table (same missing-frame reuse-previous scheme). */
static int32
LoadShadowDmg(void)
{
    int32 i;
    CCB *prev = NULL;
    int32 prevHdx = 0;
    for (i = 0; i < (int32)SHADOW_DMG_FRAME_COUNT; i++) {
        if (shadow_dmg_frames[i].missing) {
            gShadowDmgCels[i] = prev;
            gShadowDmgHdxAbs[i] = prevHdx;
            continue;
        }
        gShadowDmgCels[i] = LoadCel((char *)shadow_dmg_frames[i].cel_path, MEMTYPE_ANY);
        if (!gShadowDmgCels[i]) return -1;
        gShadowDmgHdxAbs[i] = gShadowDmgCels[i]->ccb_HDX;
        prev = gShadowDmgCels[i];
        prevHdx = gShadowDmgHdxAbs[i];
    }
    return 0;
}

static void
UnloadShadowDmg(void)
{
    int32 i;
    for (i = 0; i < (int32)SHADOW_DMG_FRAME_COUNT; i++) {
        if (!shadow_dmg_frames[i].missing && gShadowDmgCels[i] != NULL) {
            UnloadCel(gShadowDmgCels[i]);
        }
        gShadowDmgCels[i] = NULL;
    }
}

/* Prepare Jaguar CCB for one render frame.
 * - sprite_idx selects which frame from the current move
 * - anchor_x is the fighter's current world X (state machine tracks it)
 * - flip_r negates HDX and shifts XPos to the right edge of the unflipped sprite
 * - flip_f negates VDY and shifts YPos to the bottom edge of the unflipped sprite
 */
/* Unified per-fighter CCB renderer (MILESTONE_1V1 §1.1). Replaces the old
 * PrepareJaguarCel / PrepareShadowCel / PrepareShadowDmgCel trio: the ONLY
 * difference between them was the base horizontal flip, which is exactly the
 * fighter's facing — eff_r = step_flip_r XOR facing_left. x_extra/y_extra carry
 * the knockback slide / airborne launch offsets for the damage render (0 for a
 * normal pose). f->yOff carries the live jump lift. */
static CCB *
PrepareCel(fighter *f, int32 moveIdx, int32 sprite_idx, int32 anchor_x,
           int flip_r, int flip_f, int32 x_extra, int32 y_extra)
{
    const har_move_t *mv = &f->pack->moves[moveIdx];
    const har_frame_t *fr;
    CCB *c;
    int32 pool_idx;
    int32 px, py;
    int   eff_r;

    if (sprite_idx < 0 || sprite_idx >= mv->frame_count) sprite_idx = 0;
    fr = &mv->frames[sprite_idx];
    pool_idx = f->moveCelBase[moveIdx] + sprite_idx;
    c = f->cels[pool_idx];
    if (c == NULL) return NULL;

    eff_r = f->facingRight ? flip_r : !flip_r;

    px = anchor_x + fr->pos_x + x_extra;
    py = HAR_ANCHOR_Y + fr->pos_y + f->yOff + y_extra;   /* f->yOff<0 = airborne */
    if (eff_r) {
        c->ccb_HDX = -f->hdxAbs[pool_idx];
        px += fr->w - 1;
    } else {
        c->ccb_HDX =  f->hdxAbs[pool_idx];
    }
    if (flip_f) {
        c->ccb_VDY = -f->vdyAbs[pool_idx];
        py += fr->h - 1;
    } else {
        c->ccb_VDY =  f->vdyAbs[pool_idx];
    }
    c->ccb_XPos = px << 16;
    c->ccb_YPos = py << 16;
    return c;
}

/* (PrepareShadowCel / PrepareShadowDmgCel retired in §1.1 Stage 2 — P2 now
 * renders through the unified PrepareCel like P1, facing handled generically.) */

/* Apply a recoil-track frame's entry effects to victim `vic`: select its
 * ANIM_DAMAGE sprite and either set a velocity (is_vel) or apply its one-shot
 * position delta. Now per-fighter (either fighter can be the victim). */
static void
FighterRecoilEnterFrame(fighter *vic, int32 i)
{
    const har_recoil_frame_t *rf;
    if (vic->track == NULL || i < 0 || i >= vic->track->count) return;
    rf = &vic->track->frames[i];
    vic->dmgSprite = rf->sprite;
    if (vic->dmgSprite < 0) vic->dmgSprite = 0;
    {
        int32 dmgFrames = vic->pack->moves[vic->idxDmg].frame_count;
        if (vic->dmgSprite >= dmgFrames) vic->dmgSprite = dmgFrames - 1;
    }
    if (rf->is_vel) {
        vic->recoilVel = rf->dx;            /* px/tick, integrated with friction */
        /* Vertical launch: dy<0 pops the victim up. x256, integrated with
         * gravity in the victim's tick. Only (re)seed when a frame actually
         * sets a vertical velocity, so later track frames don't cancel the arc. */
        if (rf->dy != 0) vic->recoilVy = rf->dy * FP_ONE;
    } else {
        vic->recoilOff += rf->dx;           /* one-shot world position delta */
        vic->recoilY   += rf->dy * FP_ONE;  /* one-shot vertical delta (rare) */
    }
    vic->tickInFrame = 0;
}

/* Ground friction on the recoil velocity — verbatim from openomf
 * har.c:744-755: shed 1 px/tick, snap to 0 once |vel| drops below 2. */
static void
FighterRecoilFriction(fighter *vic)
{
    if (vic->recoilVel > 0)      vic->recoilVel = (vic->recoilVel < 2)  ? 0 : vic->recoilVel - 1;
    else if (vic->recoilVel < 0) vic->recoilVel = (vic->recoilVel > -2) ? 0 : vic->recoilVel + 1;
}

/* Lookup helper — finds the array index of a move by its OMF id (1..69) in an
 * arbitrary pack (used for both P1's current pack and P2's opponent pack). */
static int32
FindMoveByIdIn(const har_pack_t *pack, int id)
{
    int32 i;
    for (i = 0; i < (int32)pack->moves_count; i++) {
        if (pack->moves[i].id == id) return i;
    }
    return -1;
}

/* Translate current D-pad state into a single OMF numpad char.
 * Diagonals win over cardinals. Neutral = '5'. */
static char
DpadToNumpad(uint32 padBtns)
{
    int up    = (padBtns & ControlUp)    ? 1 : 0;
    int down  = (padBtns & ControlDown)  ? 1 : 0;
    int left  = (padBtns & ControlLeft)  ? 1 : 0;
    int right = (padBtns & ControlRight) ? 1 : 0;
    if (up   && right) return '9';
    if (up   && left)  return '7';
    if (down && right) return '3';
    if (down && left)  return '1';
    if (up)    return '8';
    if (down)  return '2';
    if (left)  return '4';
    if (right) return '6';
    return '5';
}

/* Push a direction char to the front of the input buffer. Drops duplicates
 * (same as openomf-master/src/game/objects/har.c add_input_to_buffer). */
static int
InputBufPush(fighter *f, char c)
{
    if (f->inBuf[0] == c) return 0;
    om_memmove(f->inBuf + 1, f->inBuf, INPUT_BUF_LEN - 1);
    f->inBuf[0] = c;
    return 1;
}

/* Mirror back/forward when facing left so move_strings can be authored
 * "as if facing right" (matches openomf's flip_input at har.c:356). */
static char
FlipInput(char c, int facing_right)
{
    if (facing_right) return c;
    switch (c) {
        case '1': return '3';
        case '3': return '1';
        case '4': return '6';
        case '6': return '4';
        case '7': return '9';
        case '9': return '7';
        default:  return c;
    }
}

/* Replicates openomf match_move (har.c:2149). Walks gCurHarPack->moves[] for the
 * first move whose `input` (move_string) starts with `prefix` ('P' or 'K')
 * and whose remainder matches the input buffer prefix (flipped for facing).
 * Buffer is collapsed to just the head if input is "stale" (>9 OMF ticks
 * since last directional change) — same staleness rule as openomf. */
static int32
MatchMove(fighter *f, fighter_omf_view *enemyView, char prefix)
{
    char flipped[INPUT_BUF_LEN + 1];
    int32 i;
    int   j;
    int   buf_len;

    buf_len = (f->inStale > INPUT_STALE_LIMIT) ? 1 : INPUT_BUF_LEN;
    for (j = 0; j < buf_len; j++) {
        flipped[j] = FlipInput(f->inBuf[j], f->facingRight);
    }
    flipped[buf_len] = 0;

    for (i = 0; i < (int32)f->pack->moves_count; i++) {
        const char *ms = f->pack->moves[i].input;
        int         len;
        if (ms == NULL) continue;
        if (ms[0] != prefix) continue;
        len = om_strlen(ms);
        if (len == 1 || om_strncmp(ms + 1, flipped, len - 1) == 0) {
            /* Canon throw gate (openomf har.c is_in_range + the CAT_CLOSE
             * allow-branch, har.c:2101): a close/throw move is only
             * selectable when the enemy is within throw range. Out of range,
             * skip it so a plain strike further down the table matches
             * instead -- far = punch/kick, close = throw, exactly like DOS
             * OMF. (Slice 1 of the har.c logic-port; the rest of the
             * allow-branch -- idle-grounded, enemy on floor, invincibility --
             * lands with later slices.) */
            if (f->pack->moves[i].category == CAT_CLOSE) {
                af_move *afmv = af_get_move(f->afData, f->pack->moves[i].id);
                if (afmv != NULL &&
                    !is_in_range(&f->view.obj, &enemyView->obj, afmv)) {
                    continue;
                }
            }
            return i;
        }
    }
    return -1;
}

/* Air-attack variant of MatchMove. While airborne, openomf only allows
 * CAT_JUMPING moves (har.c is_move_chain_allowed); the jumping kick/punch
 * share the plain 'K'/'P' move_strings with their grounded cousins but carry
 * category CAT_JUMPING (Jaguar move 43=air kick, 44=air punch), so the same
 * prefix selects a DIFFERENT, dedicated animation in the air. We mirror the
 * move_string matching but require category == CAT_JUMPING. */
static int32
MatchAirMove(fighter *f, char prefix)
{
    char flipped[INPUT_BUF_LEN + 1];
    int32 i;
    int   j;
    int   buf_len;

    buf_len = (f->inStale > INPUT_STALE_LIMIT) ? 1 : INPUT_BUF_LEN;
    for (j = 0; j < buf_len; j++) {
        flipped[j] = FlipInput(f->inBuf[j], f->facingRight);
    }
    flipped[buf_len] = 0;

    for (i = 0; i < (int32)f->pack->moves_count; i++) {
        const char *ms = f->pack->moves[i].input;
        int         len;
        if (ms == NULL || ms[0] != prefix) continue;
        if (f->pack->moves[i].category != CAT_JUMPING) continue;
        len = om_strlen(ms);
        if (len == 1 || om_strncmp(ms + 1, flipped, len - 1) == 0) {
            return i;
        }
    }
    return -1;
}

/* Reset Jaguar to a particular move, optionally setting the high-level state.
 * Fires the SFX for step 0 if present. */
static void
FighterSetMove(fighter *f, int32 moveIdx, enum fighter_state new_state)
{
    const har_move_t *mv = &f->pack->moves[moveIdx];
    f->state = new_state;
    f->move_idx = moveIdx;
    f->cur_step = 0;
    f->field_in_step = 0;
    f->attackHasHit = 0;
    if (mv->step_count > 0 && mv->steps[0].sound_param >= 0) {
        SfxTrigger(mv->steps[0].sound_param, mv->steps[0].sound_freq_pitch);
    }
}

/* Crouch enters directly at the last frame (held pose). DOS OMF does NOT
 * play the standing→crouch transition — it just snaps to the bottom pose
 * while Down is held. */
static void
FighterEnterCrouch(fighter *f)
{
    const har_move_t *cmv = &f->pack->moves[f->idxCrouch];
    FighterSetMove(f, f->idxCrouch, ST_CROUCH);
    if (cmv->step_count > 0) {
        f->cur_step = cmv->step_count - 1;
        f->field_in_step = 0;
    }
}

static void
FighterInit(fighter *f)
{
    f->x_offset = 0;
    f->walk_phase = 0;
    f->jVy = f->jVx = f->jY = 0;
    f->jField = 0;
    f->jDelay = 0;
    f->yOff = 0;
    f->jumpAttacking = 0;
    f->airAttacked = 0;
    /* Clear any in-flight hit reaction (matters on a KO reset). */
    f->hitState = P2_IDLE;
    f->track = NULL;
    f->trackFrame = f->tickInFrame = f->fieldInTick = 0;
    f->recoilOff = f->recoilVel = f->recoilY = f->recoilVy = 0;
    f->dmgSprite = 0;
    FighterSetMove(f, f->idxIdle, ST_STANCE);
}

/* One-time per-battle wiring of a fighter to its HAR pack: anim-slot indices,
 * CEL pool pointers, anchor/facing, hp, the OMF logic backing (af table +
 * pilot + view), and the jump/walk physics speeds. Reusable for both fighters
 * (MILESTONE_1V1 §1.1) — P1 and P2 differ only by the arguments. Must run
 * BEFORE FighterInit (which plays the idle move off these bindings). The shared
 * game_state's throw_range is set once by the caller. */
static void
FighterSetupPack(fighter *f, const har_pack_t *pack, int player_id,
                 CCB **cels, int32 *base, int32 *hdx, int32 *vdy,
                 int32 baseX, int facingRight, int pilotId)
{
    int32 agi, hnum, vnum, i;

    f->pack        = pack;
    f->cels        = cels;
    f->moveCelBase = base;
    f->hdxAbs      = hdx;
    f->vdyAbs      = vdy;
    f->baseX       = baseX;
    f->facingRight = facingRight;

    f->idxIdle   = FindMoveByIdIn(pack, 11); if (f->idxIdle   < 0) f->idxIdle   = 0;
    f->idxWalk   = FindMoveByIdIn(pack, 10); if (f->idxWalk   < 0) f->idxWalk   = f->idxIdle;
    f->idxCrouch = FindMoveByIdIn(pack, 4);  if (f->idxCrouch < 0) f->idxCrouch = f->idxIdle;
    f->idxJump   = FindMoveByIdIn(pack, 1);  if (f->idxJump   < 0) f->idxJump   = f->idxIdle;
    f->idxDmg    = FindMoveByIdIn(pack, 9);  if (f->idxDmg    < 0) f->idxDmg    = f->idxIdle;

    f->hpMax = pack->base_health;
    f->hp    = f->hpMax;

    /* OMF logic backing: per-fighter af table + pilot, shared game_state. */
    f->afData  = &gAfPool[player_id];
    f->afMoves = gAfMovesPool[player_id];
    omf_view_init(&f->view);
    omf_build_af(f->afData, f->afMoves, pack->meta, pack->meta_count);
    f->omfHar.af_data   = f->afData;
    f->omfHar.player_id = (unsigned char)player_id;
    f->gs = &gOmfGameState;
    pilot_get_info(&f->pilot, pilotId);
    gOmfGameState.players[player_id].pilot = &f->pilot;
    gOmfGameState.players[player_id].god   = 0;
    f->view.obj.userdata = &f->omfHar;
    f->view.obj.gs       = &gOmfGameState;

    /* Jump/walk physics speeds, x256 fixed-point — VERBATIM har.c:2716 scaled
     * by pilot agility (jump_multiplier=1.0). See [[project-harc-port-decision]]. */
    agi  = f->pilot.agility;
    hnum = agi + 35;   /* horizontal modifier numerator (/45) */
    vnum = agi + 20;   /* vertical   modifier numerator (/30) */
    f->jumpSpeed      = (hnum * pack->af_jump_x256 * 216) / (45 * 256);
    f->superjumpSpeed = (hnum * pack->af_jump_x256 * 266) / (45 * 256);
    f->gravity        = (vnum * pack->af_fall_x256) / 30;
    f->fwdSpeed       = (vnum * pack->af_fwd_x256)  / 30;
    f->backSpeed      = (vnum * pack->af_bwd_x256)  / 30;

    /* Seed input buffer with neutral so a fresh P/K press cleanly matches the
     * 1-char move_strings ('P'/'K'). */
    for (i = 0; i < INPUT_BUF_LEN; i++) f->inBuf[i] = '5';
    f->inBuf[INPUT_BUF_LEN] = 0;
    f->inStale = 0;
    f->pad = f->lastPad = 0;
    f->hitState = P2_IDLE;

    printf("fighter P%d %s: idle=%d walk=%d crouch=%d jump=%d dmg=%d hp=%d "
           "pilot pow=%d agi=%d | jump x256 j=%d s=%d g=%d f=%d b=%d\n",
           player_id + 1, pack->name, (int)f->idxIdle, (int)f->idxWalk,
           (int)f->idxCrouch, (int)f->idxJump, (int)f->idxDmg, (int)f->hp,
           f->pilot.power, f->pilot.agility, (int)f->jumpSpeed,
           (int)f->superjumpSpeed, (int)f->gravity, (int)f->fwdSpeed,
           (int)f->backSpeed);
}

/* Advance Jaguar one video field. Handles: step dwell expiry, end-of-move
 * → return to STANCE for ATTACK, position translation in WALK. */
static void
FighterTick(fighter *f, uint32 pad)
{
    const har_move_t *mv = &f->pack->moves[f->move_idx];
    const har_anim_step_t *step = &mv->steps[f->cur_step];
    int32 limit = step->dwell_ticks * gOmfFieldsPerTick;
    int32 last_step = (mv->step_count - 1);
    if (limit < 1) limit = 1;

    /* Re-jump lockout counts down while grounded (har.c:702). */
    if (f->jDelay > 0) f->jDelay--;

    /* Backward walk plays anim at half speed for the OMF-style fwd/back
     * visual differentiation (user-confirmed 2026-05-20). */
    if (f->state == ST_WALK_L) {
        limit *= 2;
    }

    /* Movement (only while in WALK states and not attacking).
     * Forward = full 1 px / field; backward = 1 px every 2 fields. Bounds are
     * relative to the fighter's anchor: x_offset in [-60, +80] (= the canon P1
     * [50,190] range). */
    if (f->state == ST_WALK_L) {
        f->walk_phase ^= 1;
        if (f->walk_phase) {
            f->x_offset -= JAG_WALK_SPEED;
            if (f->x_offset < -60) {
                f->x_offset = -60;
            }
        }
    } else if (f->state == ST_WALK_R) {
        f->x_offset += JAG_WALK_SPEED;
        if (f->x_offset > (JAG_MAX_X - JAG_BASE_X)) {
            f->x_offset = JAG_MAX_X - JAG_BASE_X;
        }
    } else {
        f->walk_phase = 0;
    }

    /* Advance current step's dwell. */
    if (++f->field_in_step < limit) return;
    f->field_in_step = 0;

    /* Step finished — decide next. */
    if (f->cur_step >= last_step) {
        /* Reached end of move. */
        if (f->state == ST_ATTACK) {
            /* Attack complete: honor still-held inputs. Down wins over L/R
             * so Down+attack chain stays crouched at the held pose. */
            if (pad & ControlDown) {
                FighterEnterCrouch(f);
            } else {
                int32 next_move = f->idxIdle;
                enum fighter_state next_state = ST_STANCE;
                if (pad & ControlLeft)       { next_state = ST_WALK_L; next_move = f->idxWalk; }
                else if (pad & ControlRight) { next_state = ST_WALK_R; next_move = f->idxWalk; }
                FighterSetMove(f, next_move, next_state);
            }
        } else if (f->state == ST_CROUCH) {
            /* Hold last frame (fully crouched pose) while Down stays held;
             * don't loop the transition. */
            f->field_in_step = 0;
        } else {
            /* STANCE / WALK both use idle anim — loop it. */
            f->cur_step = 0;
            if (mv->steps[0].sound_param >= 0) {
                SfxTrigger(mv->steps[0].sound_param, mv->steps[0].sound_freq_pitch);
            }
        }
    } else {
        f->cur_step++;
        if (mv->steps[f->cur_step].sound_param >= 0) {
            SfxTrigger(mv->steps[f->cur_step].sound_param,
                       mv->steps[f->cur_step].sound_freq_pitch);
        }
    }
}

/* Enter the jump state. dir_h: +1 forward (input '9'), -1 backward ('7'),
 * 0 straight up ('8'). superjump: previous buffered input was a crouch dir,
 * so jump 25% higher (har.c:2459). Mirrors the STATE_JUMPING branch of
 * har_act (har.c:2434): seed vel.y from jump/superjump speed, vel.x per
 * direction. We snapshot the current world X into the x256 accumulator so
 * the arc translates smoothly. */
static void
FighterEnterJump(fighter *f, int dir_h, int superjump)
{
    f->jVy = superjump ? f->superjumpSpeed : f->jumpSpeed;  /* negative = up */
    if (dir_h > 0)      f->jVx =  f->fwdSpeed;              /* '9' fwd*dir(+1) */
    else if (dir_h < 0) f->jVx = -f->backSpeed;            /* '7' back*dir*-1 */
    else                f->jVx =  0;                        /* '8' straight up */
    f->jY     = 0;
    f->jXAbs  = (f->baseX + f->x_offset) << FP_SHIFT;
    f->jField = 0;
    FighterSetMove(f, f->idxJump, ST_JUMP);
}

/* Advance Jaguar one video field while airborne. Anim frames advance per
 * field (held at the last frame once exhausted -- the arc is driven by
 * physics, not the anim). Physics integrates once per OMF tick
 * (gOmfFieldsPerTick fields), exactly as har.c:768: vel.y += gravity, clamp
 * to terminal velocity 13, pos += vel. Landing (har.c:681) reverts to
 * idle/walk per held input and arms jump_delay. */
static void
FighterTickJump(fighter *f, uint32 pad)
{
    const har_move_t      *mv   = &f->pack->moves[f->move_idx];
    const har_anim_step_t *step = &mv->steps[f->cur_step];
    int32 limit     = step->dwell_ticks * gOmfFieldsPerTick;
    int32 last_step = mv->step_count - 1;
    int32 loX = (f->baseX - 60) << FP_SHIFT;
    int32 hiX = (f->baseX + (JAG_MAX_X - JAG_BASE_X)) << FP_SHIFT;
    if (limit < 1) limit = 1;

    /* Anim advance only (no loop-to-idle, no walk translation). */
    if (++f->field_in_step >= limit) {
        f->field_in_step = 0;
        if (f->cur_step < last_step) {
            f->cur_step++;
            if (mv->steps[f->cur_step].sound_param >= 0) {
                SfxTrigger(mv->steps[f->cur_step].sound_param,
                           mv->steps[f->cur_step].sound_freq_pitch);
            }
        } else if (f->jumpAttacking) {
            /* Air swing finished: drop back to the jump pose for the rest of
             * the fall (move_idx swaps back so the -60 fixup re-applies). */
            f->jumpAttacking = 0;
            f->move_idx = f->idxJump;
            f->cur_step = 0;
        }
        /* else: hold the last frame until landing. */
    }

    /* One physics step per OMF tick (matches the P2-recoil gate). */
    if (++f->jField < gOmfFieldsPerTick) return;
    f->jField = 0;

    f->jVy += f->gravity;                             /* har.c:768 */
    if (f->jVy > TERMINAL_VY_X256) f->jVy = TERMINAL_VY_X256;
    f->jY    += f->jVy;                               /* <0 = above floor */
    f->jXAbs += f->jVx;

    /* Stay within the same horizontal bounds the walk uses (x256). */
    if (f->jXAbs < loX) f->jXAbs = loX;
    if (f->jXAbs > hiX) f->jXAbs = hiX;
    f->x_offset = (f->jXAbs >> FP_SHIFT) - f->baseX;

    /* Landed: snap to floor, arm re-jump delay, resume grounded state by
     * held input (har.c:681). jump_delay is 3 ticks -> fields. */
    if (f->jY >= 0) {
        f->jY    = 0;
        f->jVy   = 0;
        f->jVx   = 0;
        f->jDelay = 3 * gOmfFieldsPerTick;
        f->jumpAttacking = 0;
        f->airAttacked   = 0;
        if (pad & ControlRight)     FighterSetMove(f, f->idxWalk, ST_WALK_R);
        else if (pad & ControlLeft) FighterSetMove(f, f->idxWalk, ST_WALK_L);
        else                        FighterSetMove(f, f->idxIdle, ST_STANCE);
    }
}

/* Put `vic` into the hit-reaction sub-FSM, loading the ATTACKER move's authored
 * recoil track (footer_string baked per move). The track drives the victim's
 * ANIM_DAMAGE sprites + knockback motion verbatim; a move with no footer falls
 * back to a brief in-place stagger. Generalised from the old P2-only path so
 * EITHER fighter can be the victim (MILESTONE_1V1 §1.1 bidirectional). */
static void
FighterEnterDamage(fighter *vic, fighter *att)
{
    vic->hitState    = P2_DAMAGE;
    vic->track       = HarPackFindRecoil(att->pack,
                                         att->pack->moves[att->move_idx].id);
    vic->trackFrame  = 0;
    vic->fieldInTick = 0;
    vic->tickInFrame = 0;
    vic->recoilOff   = 0;
    vic->recoilVel   = 0;
    vic->recoilY     = 0;
    vic->recoilVy    = 0;
    vic->dmgSprite   = 0;
    if (vic->track != NULL && vic->track->count > 0)
        FighterRecoilEnterFrame(vic, 0);
}

/* Advance one fighter by one video field. When in the hit-reaction sub-FSM
 * (DAMAGE/RECOVER) it replays the recoil track / eases back to anchor; else it
 * runs the normal grounded or airborne timeline. The same code path serves
 * both fighters (P1's pad is the real controller, P2's is the driver/AI). */
static void
FighterAdvance(fighter *f)
{
    if (f->hitState == P2_DAMAGE) {
        /* One physics step per OMF tick: integrate velocity + friction, then
         * advance the track frame when its (clamped) tick_len elapses. */
        if (++f->fieldInTick >= gOmfFieldsPerTick) {
            f->fieldInTick = 0;
            f->recoilOff += f->recoilVel;
            /* LAUNCHER arc: while airborne, integrate gravity (no ground
             * friction on the horizontal — projectile motion); land at the
             * floor. Mirrors the jump physics + openomf har.c:768. */
            if (f->recoilY < 0 || f->recoilVy < 0) {
                f->recoilVy += f->gravity;
                if (f->recoilVy > TERMINAL_VY_X256) f->recoilVy = TERMINAL_VY_X256;
                f->recoilY += f->recoilVy;
                if (f->recoilY >= 0) { f->recoilY = 0; f->recoilVy = 0; }
            } else {
                FighterRecoilFriction(f);
            }
            f->tickInFrame++;
            if (f->track == NULL) {
                if (f->tickInFrame >= P2_NOTRACK_TICKS) f->hitState = P2_RECOVER;
            } else {
                int32 cap = f->track->frames[f->trackFrame].ticks;
                if (cap > P2_RECOIL_TICK_MAX) cap = P2_RECOIL_TICK_MAX;
                if (cap < 1) cap = 1;
                if (f->tickInFrame >= cap) {
                    f->trackFrame++;
                    if (f->trackFrame >= f->track->count) f->hitState = P2_RECOVER;
                    else FighterRecoilEnterFrame(f, f->trackFrame);
                }
            }
        }
    } else if (f->hitState == P2_RECOVER) {
        /* Ease the residual knockback offset back to the anchor before resuming
         * the idle loop. (Simplification carried over from the dummy P2 — a
         * fully-canon model would leave the fighter at its knocked-back X;
         * revisit once both fighters walk freely.) */
        if (++f->fieldInTick >= gOmfFieldsPerTick) {
            f->fieldInTick = 0;
            if (f->recoilOff > 0)
                f->recoilOff = (f->recoilOff <= P2_RECOVER_STEP) ? 0 : f->recoilOff - P2_RECOVER_STEP;
            else if (f->recoilOff < 0)
                f->recoilOff = (f->recoilOff >= -P2_RECOVER_STEP) ? 0 : f->recoilOff + P2_RECOVER_STEP;
            if (f->recoilOff == 0) {
                f->hitState = P2_IDLE;
                FighterSetMove(f, f->idxIdle, ST_STANCE);
            }
        }
    } else if (f->state == ST_JUMP) {
        FighterTickJump(f, f->pad);
    } else {
        FighterTick(f, f->pad);
    }
}

/* This field's display frame for a fighter — moveIdx/sprite/flips + world
 * anchor + the knockback x/y extras. Shared by the view refresh and the CCB
 * prep so both describe the exact same frame. Also sets f->yOff (jump lift). */
typedef struct {
    int32 moveIdx, spriteIdx;
    int   flip_r, flip_f;
    int32 anchorX, xExtra, yExtra;
} frame_desc;

static void
FighterComputeFrame(fighter *f, frame_desc *d)
{
    d->anchorX = f->baseX + f->x_offset;
    d->xExtra  = 0;
    d->yExtra  = 0;
    d->flip_r  = 0;
    d->flip_f  = 0;

    if (f->hitState == P2_DAMAGE || f->hitState == P2_RECOVER) {
        int32 dmgFrames = f->pack->moves[f->idxDmg].frame_count;
        d->moveIdx   = f->idxDmg;
        d->spriteIdx = f->dmgSprite;
        if (d->spriteIdx < 0) d->spriteIdx = 0;
        if (d->spriteIdx >= dmgFrames) d->spriteIdx = dmgFrames - 1;
        d->xExtra = f->recoilOff;
        d->yExtra = f->recoilY / FP_ONE;
        f->yOff = 0;
    } else {
        const har_move_t      *mv   = &f->pack->moves[f->move_idx];
        const har_anim_step_t *step = &mv->steps[f->cur_step];
        d->moveIdx   = f->move_idx;
        d->spriteIdx = step->sprite_idx;
        d->flip_r    = step->flip_r;
        d->flip_f    = step->flip_f;
        /* Airborne lift: the -60 JUMP_COORD_ADJUSTMENT applies only to the jump
         * anim's frames (baked +60 low); an air-attack anim gets the physics
         * offset alone. */
        f->yOff = (f->state == ST_JUMP)
                  ? ((f->jY / FP_ONE)
                     - (f->move_idx == f->idxJump ? JUMP_COORD_ADJUSTMENT : 0))
                  : 0;
    }
}

/* Refresh the OMF runtime view (mask as TARGET + collision coords as ATTACKER)
 * to the computed frame, so intersect_har_sprite_hitpoint can read this fighter. */
static void
FighterRefreshView(fighter *f, const frame_desc *d)
{
    const har_move_t  *mv = &f->pack->moves[d->moveIdx];
    int32 si = d->spriteIdx;
    const har_frame_t *fr;
    if (si < 0 || si >= mv->frame_count) si = 0;
    fr = &mv->frames[si];
    omf_view_update(&f->view, d->anchorX, HAR_ANCHOR_Y,
                    f->facingRight ? OBJECT_FACE_RIGHT : OBJECT_FACE_LEFT,
                    fr->w, fr->h, fr->pos_x, fr->pos_y,
                    d->flip_r,
                    (unsigned char *)fr->mask,
                    mv->coords, mv->coord_count,
                    si);
}

static CCB *
FighterPrepareCel(fighter *f, const frame_desc *d)
{
    return PrepareCel(f, d->moveIdx, d->spriteIdx, d->anchorX,
                      d->flip_r, d->flip_f, d->xExtra, d->yExtra);
}

/* One-directional hit test: if `att` is mid-attack and its collision coords
 * overlap `tgt`'s hitmask, apply canon damage + knockback to `tgt`. Called for
 * BOTH ordered pairs each field (bidirectional). The single-hit gate is
 * per-attacker. Requires both views already refreshed by FighterRefreshView. */
static void
HitCheck(fighter *att, fighter *tgt)
{
    const har_move_t *mv = &att->pack->moves[att->move_idx];
    vec2i contact;

    if (att->state != ST_ATTACK) return;       /* only grounded attacks deal dmg */
    if (att->hitState != P2_IDLE) return;       /* attacker itself is reeling */
    if (att->attackHasHit) return;              /* one hit per attack */
    if (tgt->hitState != P2_IDLE) return;       /* target already reeling */
    if (mv->coord_count <= 0) return;
    if (gKoCountdown != 0) return;

    contact.x = 0; contact.y = 0;
    if (!intersect_har_sprite_hitpoint(&att->view.obj, &tgt->view.obj, 1, &contact))
        return;

    att->attackHasHit = 1;
    {
        af_move *afmv = af_get_move(att->afData, mv->id);
        int dmg = mv->damage;   /* fallback = raw if no af_move */
        int stun = 0;
        if (afmv != NULL) {
            /* Canon single-player damage (har.c:829) using the ATTACKER's pilot
             * power, + the footer "clang" impact sound on connect. */
            calc_damage_and_stun(&att->view.obj, afmv, &dmg, &stun);
            if (afmv->hit_sound >= 0) SfxTrigger(afmv->hit_sound, 0);
        }
        gHitPauseFields = HIT_PAUSE_FIELDS;     /* shared impact freeze */
        FighterEnterDamage(tgt, att);
        tgt->hp -= dmg;
        if (tgt->hp <= 0) {
            tgt->hp = 0;
            gKoCountdown = KO_FIELDS;
            /* §1.3: first KO ends the match. The LOSER is `tgt`; P1 (gF[0]) is
             * the human. Result banner shown from the human's perspective. */
            gMatchOver  = 1;
            gWinnerIsP1 = (tgt == &gF[1]);
            gKoBanner   = gWinnerIsP1 ? "YOU WIN" : "YOU LOSE";
        }
        printf("HIT P%d->P%d move=%d dmg=%d stun=%d tgthp=%d\n",
               (att == &gF[0]) ? 1 : 2, (tgt == &gF[0]) ? 1 : 2,
               (int)mv->id, dmg, stun, (int)tgt->hp);
    }
}

/* Consume one field of abstract controller input for a fighter — the INPUT SEAM
 * (MILESTONE_1V1 §1.1 / §2). `pad`/`lastPad` are the controller bits this field
 * and last; their source is the real ControlPad for P1 and the driver/AI for
 * P2, but this logic is identical for both. Handles attack-button edges (A=P,
 * B=K, grounded or air), jump entry, and held grounded transitions
 * (walk/crouch/idle). `enemy` feeds MatchMove's CAT_CLOSE range gate. */
static void
FighterHandleInput(fighter *f, fighter *enemy, uint32 pad, uint32 lastPad)
{
    /* A fighter in its hit reaction (DAMAGE/RECOVER) can't act — the recoil FSM
     * owns it until it recovers. (Critical now that P1 can be a victim too.) */
    if (f->hitState != P2_IDLE) return;

    if ((pad & ControlA) && !(lastPad & ControlA) && gKoCountdown == 0) {
        if (f->state == ST_JUMP && !f->jumpAttacking && !f->airAttacked) {
            int32 mv = MatchAirMove(f, 'P');
            if (mv >= 0) { FighterSetMove(f, mv, ST_JUMP); f->jumpAttacking = 1; f->airAttacked = 1; }
        } else if (f->state != ST_ATTACK && f->state != ST_JUMP) {
            int32 mv = MatchMove(f, &enemy->view, 'P');
            if (mv >= 0) FighterSetMove(f, mv, ST_ATTACK);
        }
    }
    if ((pad & ControlB) && !(lastPad & ControlB) && gKoCountdown == 0) {
        if (f->state == ST_JUMP && !f->jumpAttacking && !f->airAttacked) {
            int32 mv = MatchAirMove(f, 'K');
            if (mv >= 0) { FighterSetMove(f, mv, ST_JUMP); f->jumpAttacking = 1; f->airAttacked = 1; }
        } else if (f->state != ST_ATTACK && f->state != ST_JUMP) {
            int32 mv = MatchMove(f, &enemy->view, 'K');
            if (mv >= 0) FighterSetMove(f, mv, ST_ATTACK);
        }
    }

    /* Jump entry (canon har.c:2434: 7/8/9 grounded + !jump_delay). dir_h is in
     * WORLD terms (+1 right / -1 left), so it is facing-independent. */
    if ((pad & ControlUp)
        && f->state != ST_ATTACK && f->state != ST_JUMP
        && f->jDelay == 0 && gKoCountdown == 0 && gHitPauseFields == 0) {
        int dir_h  = (pad & ControlRight) ? 1 : (pad & ControlLeft) ? -1 : 0;
        int superj = (f->inBuf[1] == '1' || f->inBuf[1] == '2' || f->inBuf[1] == '3');
        FighterEnterJump(f, dir_h, superj);
    }

    /* Held-input grounded transitions. Down beats L/R (can't crouch-walk). */
    if (f->state != ST_ATTACK && f->state != ST_JUMP) {
        enum fighter_state desired;
        int32              desired_move;
        if (pad & ControlDown)       { desired = ST_CROUCH; desired_move = f->idxCrouch; }
        else if (pad & ControlLeft)  { desired = ST_WALK_L; desired_move = f->idxWalk; }
        else if (pad & ControlRight) { desired = ST_WALK_R; desired_move = f->idxWalk; }
        else                         { desired = ST_STANCE; desired_move = f->idxIdle; }
        if (f->move_idx != desired_move) {
            if (desired == ST_CROUCH) FighterEnterCrouch(f);
            else                      FighterSetMove(f, desired_move, desired);
        } else {
            f->state = desired;
        }
    }
}

/* §1.2 Heuristic AI driver for P2 (MILESTONE_1V1). Replaces the §1.1 relentless
 * placeholder. It synthesises the SAME controller bitmask the human feeds P1, so
 * the downstream fighter logic is identical (the input seam stays the only
 * difference between human and AI — MILESTONE_1V1 §2 "Input abstraction").
 *
 * Design goal (user-confirmed 2026-06-03): "medium, beatable" — feels like a
 * real opponent but loses to technique. NOT the full ai_controller.c (~2400
 * lines, deferred to 1.0). Behaviours:
 *   - DISTANCE management: approach when far, hold spacing in the mid zone,
 *     back off when too close.
 *   - TIMED attacks: only swing in range, gated by a RANDOMISED cooldown AND a
 *     reaction delay (it doesn't punish frame-perfectly — that delay is the
 *     window the player exploits to feel "skilled").
 *   - JUMP-IN: occasional approach-jump from the mid zone.
 *   - Light EVASION (not a block — blocking is a 0.1 cut): when P1 starts a
 *     swing while P2 is close, P2 biases toward backing off. Keeps it from
 *     being a stationary punching bag without adding a block state.
 *   - Reaction to being hit is FREE: the hit-reaction FSM (hitState) already
 *     locks the fighter out while reeling.
 *
 * Tuning lives in the AI_* constants. All randomness is OmfRand (the game RNG),
 * so behaviour is reproducible for smoke-tests but varied frame-to-frame. */
#define AI_ATTACK_RANGE   72    /* world px: inside this, a swing can connect    */
#define AI_APPROACH_GAP   88    /* farther than this -> close the distance       */
#define AI_TOO_CLOSE      40    /* nearer than this -> create space              */
#define AI_CD_MIN         28    /* min fields between swings (~0.47 s)           */
#define AI_CD_RAND        48    /* + up to this many (so 28..76, ~0.47..1.27 s)  */
#define AI_REACT_MIN       6    /* min reaction delay before a swing (~0.10 s)   */
#define AI_REACT_RAND     14    /* + up to this many (so 6..20, ~0.10..0.33 s)   */
#define AI_JUMP_CHANCE    10    /* 1-in-N per eligible field to jump in          */
#define AI_RETREAT_FIELDS 16    /* how long a back-off burst lasts               */
#define AI_EVADE_CHANCE    3    /* 1-in-N to back off when P1 swings up close    */

typedef struct {
    int32 cooldown;     /* fields until the next swing is allowed              */
    int32 react;        /* fields to wait (in range) before committing a swing */
    int32 retreat;      /* >0 = currently backing off for spacing              */
} ai_brain;

static ai_brain gP2Ai = { 0, 0, 0 };

static uint32
P2DriverPad(fighter *p2, fighter *p1)
{
    int32  p1x  = p1->baseX + p1->x_offset;
    int32  p2x  = p2->baseX + p2->x_offset;
    int32  dist = p2x - p1x;     /* >0 = P2 right of P1 (the normal layout) */
    uint32 pad  = 0;

    if (gP2Ai.cooldown > 0) gP2Ai.cooldown--;
    if (gP2Ai.retreat  > 0) gP2Ai.retreat--;

    /* Freeze the brain during KO/hit-pause (the loop freezes advance anyway). */
    if (gKoCountdown != 0 || gHitPauseFields != 0) { gP2Ai.react = 0; return 0; }

    /* Can't act while reeling or already committed to a swing/jump — let those
     * resolve. Reset the per-approach reaction timer so the next time we reach
     * attack range we re-arm a fresh (human-like) delay. */
    if (p2->hitState != P2_IDLE || p2->state == ST_ATTACK || p2->state == ST_JUMP) {
        gP2Ai.react = 0;
        return 0;
    }

    /* Walk directions in WORLD terms: P2 sits to P1's RIGHT and faces LEFT, so
     * ControlLeft = approach, ControlRight = retreat. */

    /* Light evasion: if P1 just started a swing and we're in its range, maybe
     * peel back instead of trading. Occasional so the fight stays winnable. */
    if (p2->state != ST_CROUCH && gP2Ai.retreat == 0 &&
        p1->state == ST_ATTACK && dist <= AI_ATTACK_RANGE &&
        (OmfRand() % AI_EVADE_CHANCE) == 0) {
        gP2Ai.retreat = AI_RETREAT_FIELDS;
    }

    if (gP2Ai.retreat > 0) {
        pad |= ControlRight;                 /* back off to reset spacing */
        gP2Ai.react = 0;
        return pad;
    }

    if (dist > AI_APPROACH_GAP) {
        /* FAR: close the gap. Occasionally jump in to mix up the approach. */
        if ((OmfRand() % AI_JUMP_CHANCE) == 0 && p2->jDelay == 0) {
            pad |= ControlUp | ControlLeft;  /* approach-jump */
        } else {
            pad |= ControlLeft;
        }
        gP2Ai.react = 0;
    } else if (dist < AI_TOO_CLOSE) {
        /* TOO CLOSE: make room so attacks have spacing to land. */
        pad |= ControlRight;
        gP2Ai.react = 0;
    } else if (dist <= AI_ATTACK_RANGE) {
        /* IN RANGE: arm a reaction delay the first eligible field, then swing
         * once it elapses and the cooldown is clear. */
        if (gP2Ai.cooldown == 0) {
            if (gP2Ai.react == 0) {
                gP2Ai.react = AI_REACT_MIN + (int32)(OmfRand() % AI_REACT_RAND);
            } else if (gP2Ai.react == 1) {
                pad |= (OmfRand() & 1) ? ControlA : ControlB;   /* commit swing */
                gP2Ai.cooldown = AI_CD_MIN + (int32)(OmfRand() % AI_CD_RAND);
                gP2Ai.react = 0;
            } else {
                gP2Ai.react--;               /* still reacting; hold position */
            }
        }
    } else {
        /* MID zone (between attack range and approach gap): edge in slowly so we
         * settle at the spacing where our swing reaches. */
        pad |= ControlLeft;
        gP2Ai.react = 0;
    }

    return pad;
}

/* Renders a one-frame LOADING screen on the back buffer + page-flips, so
 * the user sees clear feedback during the multi-second CD-ROM hit at the
 * start of BattleSceneInit (per [[project-battle-load-model]]). `what`
 * is a short label (~12 chars max) drawn under the "LOADING" header.
 *
 * Always renders to screen 0 -- we don't need double-buffering, the
 * frame just has to land before the next blocking LoadCel call. */
static void
RenderLoadingScreen(const char *what)
{
    Item  bm;
    CCB  *head;

    if (gScreenContext == NULL) return;
    bm = gScreenContext->sc_BitmapItems[0];
    SetVRAMPages(gVRAMIOReq,
                 gScreenContext->sc_Bitmaps[0]->bm_Buffer,
                 0x80108010,
                 gScreenContext->sc_nFrameBufferPages, -1);

    BlitTextBeginFrame();
    head = BlitTextChain(130, 100, "LOADING");
    if (head != NULL) DrawCels(bm, head);
    if (what != NULL) {
        head = BlitTextChain(110, 120, what);
        if (head != NULL) DrawCels(bm, head);
    }
    DisplayScreen(gScreenContext->sc_Screens[0], 0);
    WaitVBL(gVBLIOReq, 1);
    MusicService();   /* keep menu BGM alive through the loading splash */
}

/* Sessione 7 step 1: the battle scene's `init` callback IS the entire
 * legacy main loop. tick/render/free stay NULL until step 2 splits this
 * into per-frame work. Init returns when the user presses Start (the
 * break at the bottom of the while(1) below). */
static void
BattleSceneInit(scene_3do *s)
{
    Item   currentBitmapItem;
    int32  whichScreen;
    uint32 padBtns;
    uint32 lastPad;
    int32  padId;
    ControlPadEventData cped;
    CCB   *p1Ccb;
    CCB   *p2Ccb;

    (void)s;

    /* Pre-battle loads take several seconds from CD-ROM. Without
     * progressive feedback the user sees a black screen between
     * SCENE_SELECT and the first battle frame.
     *
     * Sessione 8 polish: BGM is already running (menu theme started
     * in main()). Swap to the fight theme here so the user hears the
     * mood change while the heavy LoadCel storm finishes. Canon
     * openomf binds music per-arena (game/scenes/arena.c:1714 switch
     * on bk_data->file_id); for now hardcode ARENA2 (Power Plant)
     * since it was the demo's only stage and is the most "OMF" --
     * sessione 9 will wire per-arena dispatch. */
    /* Sessione 8 polish 2026-05-21: the LoadCel storm during battle init
     * is heavy enough that BGM stutters audibly on Opera. User-chosen
     * fix: silence the BGM through the loading splash ("quiet before
     * the storm") and let ARENA2 start FRESH at fight frame 0.
     *
     * Critical: we MusicPause (just spStop) instead of MusicStop here.
     * Full MusicStop would free the menu BGM's 128 KB ring buffer
     * BEFORE the LoadCel storm fragments DRAM -- by the time we want
     * to MusicStart(ARENA2) at the bottom of init, no contiguous 128 KB
     * block remains and spCreatePlayer fails silently (per
     * [[project-audio-load-ordering]]). MusicPause keeps the memory
     * allocated, the LoadCel storm allocates above it, and the bottom
     * MusicStop+MusicStart cleanly swaps menu->arena2 in the same hole. */
    MusicPause();
    RenderLoadingScreen("AUDIO");
    SfxStart();

    /* Switch the HAR pack BEFORE any per-HAR LoadCel so the right tables
     * (Jaguar vs Thorn) drive frame loading + move matching. */
    HarPackSelect(gSelectedHarId);

    RenderLoadingScreen("FIGHTERS");

    printf("Loading %d %s moves...\n",
           (int)gCurHarPack->moves_count, gCurHarPack->name);
    if (LoadAllJaguarMoves() < 0) { SceneQuit(); return; }

    /* P2 promotion (§1.1): P2 is now a REAL HAR. Load the OPPONENT moveset via
     * its atlas into the P2 pool, replacing the Shadow idle+damage dummy.
     * Opponent SELECTION (MILESTONE_1V1 §1.1 closeout): the pack now derives
     * from gOpponentHarId — the HAR the 1P opponent actually picked in
     * SCENE_VS (ChooseOpponentHar) — instead of being forced to "the other"
     * pack. This makes the VS screen and the battle agree, and allows a mirror
     * match (P2 == P1 HAR): P2 loads its OWN atlas instance into the gP2 pool,
     * so a same-HAR pick is safe. Falls back to Jaguar if gOpponentHarId is
     * unset (-1) or unplayable (e.g. SHADOW). */
    gP2HarPack = (gOpponentHarId >= 0) ? HarPackForId(gOpponentHarId)
                                       : ((gCurHarPack == &gJaguarPack) ? &gThornPack
                                                                        : &gJaguarPack);
    {
        int32 pn = LoadHarFromAtlas(gP2HarPack, &gP2Atlas,
                                    HarPackAtlasPath(gP2HarPack),
                                    gP2Cels, gP2MoveCelBase,
                                    gP2HdxAbs, gP2VdyAbs, JAGUAR_MAX_CELS);
        if (pn < 0) { printf("P2 atlas load failed\n"); SceneQuit(); return; }
        gP2IsAtlas = 1;
        printf("P2 %s via ATLAS: %d cels\n", gP2HarPack->name, (int)pn);
    }

    /* Jaguar anim slot mapping — these are the CANONICAL OMF animation IDs,
     * fixed for ALL HARs per openomf resources/animation.h ("All HARs have
     * these predefined animations"):
     *   id 2  = ANIM_STANDUP   (rise from crouch — NOT the crouch hold)
     *   id 3  = ANIM_STUNNED   (dazed wobble)
     *   id 4  = ANIM_CROUCHING (held crouch pose; anim_string "A5")
     *   id 10 = ANIM_WALKING
     *   id 11 = ANIM_IDLE
     * (Verified 2026-05-30: ids 10/11/3 match the enum exactly; the earlier
     * "identified visually, enum doesn't match" note was wrong — and led to
     * crouch being bound to id 2/STANDUP instead of id 4/CROUCHING.)
     * Punch/kick are picked dynamically by MatchMove via move_string. */
    /* §1.1: wire BOTH fighters to their packs — anim slots, CEL pool, OMF
     * backing, pilot, jump physics — in one call each. P1 (gF[0]) faces right
     * from the left anchor; P2 (gF[1]) faces left from the right anchor and is
     * driven by the AI/driver. The shared throw range is the OMF default 100
     * (game_state.c:255). gOpponentPilotId comes from the VS scene; fall back to
     * P1's pilot if unset. */
    gOmfGameState.match_settings.throw_range = 100;
    FighterSetupPack(&gF[0], gCurHarPack, 0,
                     gJaguarCels, gMoveCelBase, gJaguarHdxAbs, gJaguarVdyAbs,
                     JAG_BASE_X, 1, gSelectedPilotId);
    FighterSetupPack(&gF[1], gP2HarPack, 1,
                     gP2Cels, gP2MoveCelBase, gP2HdxAbs, gP2VdyAbs,
                     P2_ANCHOR_X, 0,
                     (gOpponentPilotId >= 0) ? gOpponentPilotId : gSelectedPilotId);

    /* §1.4 per-pilot fight palettes — LANDED 2026-06-03 (PRIMARY-zone recolor).
     * The shipped atlases are now SENTINEL-extracted: the PRIMARY armor zone
     * (OMF indices 32..47) carries DISTINCT colors per frame so the runtime
     * PLUT pointer-swap can recolor it to each pilot's color_1 ramp; secondary
     * + tertiary stay on the shared grey ramp. Full 3-zone fidelity is blocked
     * by a HARD 3DO limit — a coded CEL's PLUT caps at 32 colors, but 3 distinct
     * zones push most frames to 33..48 colors (~2.4 MB for two fighters vs a
     * ~1.2 MB budget). Primary-only fits (~791 KB resident, measured). The grey
     * ramp is left untouched at recolor time so body metal is never mis-tinted.
     * See [[project-pilot-plut-swap]] / [[project-palette-session-todo]].
     * pilot id out of 0..9 (e.g. NOVA=10, -1) -> stays neutral-sentinel; in 1v1
     * both fighters always have a pilot, so a .PAL is always applied. */
    ApplyFighterPilotPlut(&gP1Atlas, gCurHarPack, gSelectedPilotId);
    ApplyFighterPilotPlut(&gP2Atlas, gP2HarPack,
                          (gOpponentPilotId >= 0) ? gOpponentPilotId
                                                  : gSelectedPilotId);

    RenderLoadingScreen("ARENAS");
    /* Structural reclaim (MILESTONE_1V1 §2): load ONLY the picked arena, not
     * all 5. Saves ~174 KB (4 × ~43 KB) and is more canon — each fight is one
     * arena. The pick (OmfRand % ARENA_COUNT) moved here from below so we know
     * which one to load; the rest of gArenaCels[] stays NULL (the unload loop
     * and gArenaCel macro both tolerate NULL). No runtime arena switching
     * exists, so a single resident arena is sufficient. */
    {
        int32 a;
        for (a = 0; a < ARENA_COUNT; a++) gArenaCels[a] = NULL;
        gCurArenaIdx = (int32)(OmfRand() % ARENA_COUNT);
        printf("arena pick: %s\n", gArenaPaths[gCurArenaIdx]);
        gArenaCels[gCurArenaIdx] =
            LoadCel((char *)gArenaPaths[gCurArenaIdx], MEMTYPE_ANY);
        if (gArenaCels[gCurArenaIdx] != NULL) {
            gArenaCels[gCurArenaIdx]->ccb_XPos = 0 << 16;
            gArenaCels[gCurArenaIdx]->ccb_YPos = 20 << 16;
        } else {
            printf("LoadCel(%s) NULL\n", gArenaPaths[gCurArenaIdx]);
        }
    }

    /* Sessione 9 health bars: one-time CEL load. Both CELs are 100x6 solid
     * color blocks generated by host_tools/gen_hpbar_cels.sh. We load each
     * twice so the P1 + P2 bars get their own CCBs and can chain into a
     * single DrawCels list. */
    gHpBarBgP1 = LoadCel("Art/hpbar_bg.CEL", MEMTYPE_ANY);
    gHpBarBgP2 = LoadCel("Art/hpbar_bg.CEL", MEMTYPE_ANY);
    gHpBarFgP1 = LoadCel("Art/hpbar_fg.CEL", MEMTYPE_ANY);
    gHpBarFgP2 = LoadCel("Art/hpbar_fg.CEL", MEMTYPE_ANY);

    /* Sessione 10.6 intro lock: READY + FIGHT sprites + announcer voice.
     * Loaded here so they're available before the main battle loop, then
     * unloaded by RunIntroLock() once the intro state machine completes. */
    gIntroReadyCel = LoadCel(INTRO_READY_CEL, MEMTYPE_ANY);
    gIntroFightCel = LoadCel(INTRO_FIGHT_CEL, MEMTYPE_ANY);
    if (gHpBarBgP1 == NULL || gHpBarBgP2 == NULL ||
        gHpBarFgP1 == NULL || gHpBarFgP2 == NULL) {
        printf("hpbar CEL load failed\n");
    } else {
        gHpBarFgNativeHdx = gHpBarFgP1->ccb_HDX;
        printf("hpbar FG native HDX=0x%lx\n", (long)gHpBarFgNativeHdx);
    }

    /* Both fighters' hp/hpMax were set by FighterSetupPack. */
    gKoCountdown = 0;
    gKoBanner = NULL;
    gHitPauseFields = 0;
    gMatchOver = 0;
    gWinnerIsP1 = 0;

    /* §1.2: clear the P2 AI brain so cooldown/reaction/retreat from a previous
     * match don't leak into this one's first frames. */
    gP2Ai.cooldown = 0; gP2Ai.react = 0; gP2Ai.retreat = 0;

    whichScreen = 0;
    /* Seed lastPad with current pad state so a still-held A from the
     * menu doesn't trigger an immediate punch on frame 0. */
    padId   = GetControlPad(1, FALSE, &cped);
    lastPad = (padId >= 0) ? cped.cped_ButtonBits : 0;

    /* Reset both fighters to their idle pose (views + OMF backing already wired
     * by FighterSetupPack). */
    FighterInit(&gF[0]);
    FighterInit(&gF[1]);

    /* Sessione 9 random arena: the pick (OmfRand % ARENA_COUNT) now happens up
     * in the ARENAS load block so we load only the chosen arena. gCurArenaIdx
     * is already set here and drives the per-arena BGM path below. */

    /* S10.6 audio ordering fix: load the intro voice into the HAR
     * mixer BEFORE the BGM swap. Doing it AFTER (S10.6 v2 attempt)
     * silently killed arena BGM -- likely ehLoadSoundEffect's
     * LoadInstrument + LoadSample disturbed the DRAM hole left by
     * MusicStop, blocking spCreatePlayer's 128 KB allocation. */
    IntroSfxStart();

    /* All assets loaded -- swap from menu BGM to the per-arena fight theme.
     * Canonical mapping (openomf game/scenes/arena.c:1714 switch on
     * bk_data->file_id) is 1:1 with our ARENA*.CEL index, so we can
     * compose the filename directly. */
    MusicStop();
    {
        char bgm_path[32];
        bgm_path[0] = 'M'; bgm_path[1] = 'u'; bgm_path[2] = 's'; bgm_path[3] = 'i';
        bgm_path[4] = 'c'; bgm_path[5] = '/'; bgm_path[6] = 'A'; bgm_path[7] = 'R';
        bgm_path[8] = 'E'; bgm_path[9] = 'N'; bgm_path[10] = 'A';
        bgm_path[11] = (char)('0' + gCurArenaIdx);
        bgm_path[12] = '.'; bgm_path[13] = 'A'; bgm_path[14] = 'I'; bgm_path[15] = 'F';
        bgm_path[16] = 'F'; bgm_path[17] = 0;
        MusicStart(bgm_path);
    }

    printf("Controls: D-pad L/R = walk, D-pad Down = crouch, A = punch, B = kick, X = music, Start = quit\n");

    /* Sessione 10.6: Round 1 / FIGHT! intro lock. Canon flow (openomf
     * arena.c L417-451 + L189-195):
     *   READY anim -> 10-tick gap -> FIGHT anim (s11 -> SOUNDS.DAT[10] voice)
     * We hold the bg + idle HAR poses + sprite overlay for ~2 seconds total
     * with no input processing; voice fires when entering the FIGHT phase.
     * Then UnloadCel the intro sprites + tear down the announcer mixer to
     * reclaim the ~2 KB DRAM for normal battle.
     *
     * S10.6 audio: IntroSfxStart was moved BEFORE MusicStart (above)
     * to preserve the BGM ring buffer DRAM hole; the trigger call lives
     * inside the phase loop below. */
    {
        int32 phase_ticks;
        int32 phase;        /* 0=READY 1=GAP 2=FIGHT */
        int32 i_screen = whichScreen;
        int32 fight_voice_fired = 0;

        for (phase = 0; phase < 3; phase++) {
            int32 hold_ticks = (phase == 0) ? INTRO_READY_TICKS
                              : (phase == 1) ? INTRO_GAP_TICKS
                              : INTRO_FIGHT_TICKS;
            for (phase_ticks = 0; phase_ticks < hold_ticks; phase_ticks++) {
                CCB *bgCcb;
                CCB *introOverlay = NULL;
                Item bm = gScreenContext->sc_BitmapItems[i_screen];

                /* Trigger FIGHT voice once at the start of phase 2. */
                if (phase == 2 && !fight_voice_fired) {
                    IntroSfxTriggerFight();
                    fight_voice_fired = 1;
                }

                SetVRAMPages(gVRAMIOReq,
                             gScreenContext->sc_Bitmaps[i_screen]->bm_Buffer,
                             0x80108010,
                             gScreenContext->sc_nFrameBufferPages, -1);

                bgCcb = gArenaCels[gCurArenaIdx];
                if (bgCcb != NULL) {
                    bgCcb->ccb_Flags |= CCB_LAST;
                    DrawCels(bm, bgCcb);
                }

                /* Pick the overlay sprite for current phase. */
                if (phase == 0)      introOverlay = gIntroReadyCel;
                else if (phase == 2) introOverlay = gIntroFightCel;
                /* phase==1 (gap): no overlay */

                if (introOverlay != NULL) {
                    int32 px, py;
                    if (phase == 0) {
                        px = INTRO_ANCHOR_X + INTRO_READY_PX;
                        py = INTRO_ANCHOR_Y + INTRO_READY_PY;
                    } else {
                        px = INTRO_ANCHOR_X + INTRO_FIGHT_PX;
                        py = INTRO_ANCHOR_Y + INTRO_FIGHT_PY;
                    }
                    introOverlay->ccb_XPos = px << 16;
                    introOverlay->ccb_YPos = py << 16;
                    introOverlay->ccb_Flags |= CCB_LAST;
                    DrawCels(bm, introOverlay);
                }


                DisplayScreen(gScreenContext->sc_Screens[i_screen], 0);
                WaitVBL(gVBLIOReq, 1);
                MusicService();
                i_screen = (i_screen + 1) % gScreenContext->sc_nScreens;
            }
        }
        whichScreen = i_screen;     /* hand off to main loop */
    }

    /* Tear down intro assets — won't be drawn again this battle. */
    if (gIntroReadyCel) { UnloadCel(gIntroReadyCel); gIntroReadyCel = NULL; }
    if (gIntroFightCel) { UnloadCel(gIntroFightCel); gIntroFightCel = NULL; }
    IntroSfxShutdown();

    while (1) {
        fighter    *p1 = &gF[0];
        fighter    *p2 = &gF[1];
        frame_desc  d1, d2;

        currentBitmapItem = gScreenContext->sc_BitmapItems[whichScreen];
        SetVRAMPages(gVRAMIOReq,
                     gScreenContext->sc_Bitmaps[whichScreen]->bm_Buffer,
                     0x80108010,
                     gScreenContext->sc_nFrameBufferPages, -1);

        /* §1.1 SYMMETRIC per-pixel hit detection. Refresh BOTH fighters' views
         * (each carries its frame's hitmask as a target + its move's collision
         * coords as an attacker — intersect.c linked verbatim from src/intersect.c,
         * mirror of upstream game/protos/intersect.c), then run the hit test for
         * BOTH ordered pairs so either fighter can land a blow. */
        FighterComputeFrame(p1, &d1);
        FighterComputeFrame(p2, &d2);
        FighterRefreshView(p1, &d1);
        FighterRefreshView(p2, &d2);
        HitCheck(p1, p2);
        HitCheck(p2, p1);
        /* Re-derive frames AFTER hit resolution so a fighter that just got hit
         * shows its DAMAGE pose on this very field (matches the pre-§1.1
         * single-direction feel). */
        FighterComputeFrame(p1, &d1);
        FighterComputeFrame(p2, &d2);
        p1Ccb = FighterPrepareCel(p1, &d1);
        p2Ccb = FighterPrepareCel(p2, &d2);

        if (gArenaCel != NULL && p1Ccb != NULL && p2Ccb != NULL) {
            CCB *koBanner = NULL;
            CCB *barTail  = p2Ccb;
            /* Sessione 9 health bars: P1 anchored left (depletes from right),
             * P2 anchored right (depletes from left). FG bar's ccb_HDX is
             * scaled by hp/hpMax so the visible width shrinks proportionally;
             * BG stays full width as the "drained HP" indicator behind it. */
            if (gHpBarBgP1 != NULL && gHpBarFgP1 != NULL &&
                gHpBarBgP2 != NULL && gHpBarFgP2 != NULL) {
                /* LoadCel'd ccb_HDX is the CEL's native scale -- depends
                 * on the 3it packing mode + bpp. For BG bars we leave HDX
                 * untouched so they render at native HP_BAR_W. For FG
                 * bars we scale relative to the captured native HDX so
                 * visible width = HP_BAR_W * hp/hpMax regardless of the
                 * encoded scale. */
                int32 p2FillW = (HP_BAR_W * p2->hp) / p2->hpMax;
                int32 p1Hdx   = (p1->hp * gHpBarFgNativeHdx) / p1->hpMax;
                int32 p2Hdx   = (p2->hp * gHpBarFgNativeHdx) / p2->hpMax;

                gHpBarBgP1->ccb_XPos = HP_BAR_P1_X << 16;
                gHpBarBgP1->ccb_YPos = HP_BAR_Y    << 16;
                gHpBarFgP1->ccb_XPos = HP_BAR_P1_X << 16;
                gHpBarFgP1->ccb_YPos = HP_BAR_Y    << 16;
                gHpBarFgP1->ccb_HDX  = p1Hdx;

                gHpBarBgP2->ccb_XPos = (HP_BAR_P2_RIGHT - HP_BAR_W) << 16;
                gHpBarBgP2->ccb_YPos = HP_BAR_Y << 16;
                /* P2 FG shifts right as it shrinks so its right edge stays
                 * pinned to HP_BAR_P2_RIGHT, matching the OMF DOS "shrinks
                 * toward the center" feel. */
                gHpBarFgP2->ccb_XPos = (HP_BAR_P2_RIGHT - p2FillW) << 16;
                gHpBarFgP2->ccb_YPos = HP_BAR_Y << 16;
                gHpBarFgP2->ccb_HDX  = p2Hdx;

                p2Ccb->ccb_Flags &= ~CCB_LAST;
                p2Ccb->ccb_NextPtr = gHpBarBgP1;
                gHpBarBgP1->ccb_Flags &= ~CCB_LAST;
                gHpBarBgP1->ccb_NextPtr = gHpBarFgP1;
                gHpBarFgP1->ccb_Flags &= ~CCB_LAST;
                gHpBarFgP1->ccb_NextPtr = gHpBarBgP2;
                gHpBarBgP2->ccb_Flags &= ~CCB_LAST;
                gHpBarBgP2->ccb_NextPtr = gHpBarFgP2;
                gHpBarFgP2->ccb_Flags |= CCB_LAST;
                barTail = gHpBarFgP2;
            }

            /* K.O. banner — single big text in screen middle while the
             * post-K.O. freeze countdown runs. */
            BlitTextBeginFrame();
            if (gKoBanner != NULL) {
                /* Center-ish: "K.O." is narrow (~140), the §1.3 result strings
                 * (YOU WIN / YOU LOSE) are wider — nudge left so both read
                 * centered on the 320px screen. */
                int32 bx = (gKoBanner[0] == 'Y') ? 134 : 140;
                koBanner = BlitTextChain(bx, 100, gKoBanner);
            }

            gArenaCel->ccb_Flags &= ~CCB_LAST;
            gArenaCel->ccb_NextPtr = p1Ccb;
            p1Ccb->ccb_Flags &= ~CCB_LAST;
            p1Ccb->ccb_NextPtr = p2Ccb;
            if (koBanner != NULL) {
                barTail->ccb_Flags &= ~CCB_LAST;
                barTail->ccb_NextPtr = koBanner;
                /* BlitTextChain already terminates with CCB_LAST. */
            } else {
                barTail->ccb_Flags |= CCB_LAST;
            }
            DrawCels(currentBitmapItem, gArenaCel);
        }
        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();

        padId = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;

        /* Start exits the battle back to the main menu. The fall-through
         * cleanup below runs before the dispatcher reaches SCENE_MAIN. */
        if (padBtns & ControlStart) {
            SceneSetNext(SCENE_MAIN);
            break;
        }
        /* X (LShift): toggle music — free button since sessione 9 (arena no
         * longer cycles mid-fight; it's picked once at battle init). */
        if ((padBtns & ControlLeftShift) && !(lastPad & ControlLeftShift)) {
            if (gMusicOn) { MusicStop(); printf("music: off\n"); }
            else          { MusicStart("Music/MENU.AIFF"); printf("music: on\n"); }
        }

        /* §1.1 INPUT SEAM: P1 consumes the real D-pad, P2 consumes its driver's
         * synthesised pad — then BOTH go through the identical input/advance
         * path. Feed each fighter's OMF buffer, then run FighterHandleInput
         * (attacks + jump + held transitions) for each, reading the OTHER as the
         * range-gate enemy. */
        p1->pad = padBtns;
        p2->pad = P2DriverPad(p2, p1);
        {
            char d;
            d = DpadToNumpad(p1->pad);
            if (InputBufPush(p1, d)) p1->inStale = 0; else p1->inStale++;
            d = DpadToNumpad(p2->pad);
            if (InputBufPush(p2, d)) p2->inStale = 0; else p2->inStale++;
        }
        FighterHandleInput(p1, p2, p1->pad, lastPad);
        FighterHandleInput(p2, p1, p2->pad, p2->lastPad);

        /* §1.3 K.O. handling: while the post-K.O. countdown runs, freeze both
         * fighters and the input flow (Start still exits) so the result banner
         * reads cleanly. When it expires the match is OVER (single round) — head
         * back to the main menu instead of resetting and resuming. */
        if (gKoCountdown > 0) {
            gKoCountdown--;
            if (gKoCountdown == 0) {
                gKoBanner = NULL;
                if (gMatchOver) {
                    printf("match over: P%d wins -> menu\n", gWinnerIsP1 ? 1 : 2);
                    SceneSetNext(SCENE_MAIN);
                    break;
                }
                /* (No-match-over path kept for safety; unreachable in 0.1 since
                 * every KO sets gMatchOver. Would resume a fresh round.) */
                p1->hp = p1->hpMax;
                p2->hp = p2->hpMax;
                FighterInit(p1);
                FighterInit(p2);
            }
        } else if (gHitPauseFields > 0) {
            /* Impact freeze: hold both poses, advance nothing this field.
             * Input is still read above so a button held through the pause
             * doesn't double-fire on resume. */
            gHitPauseFields--;
        } else {
            /* Advance BOTH fighters through the shared per-field update
             * (normal FSM or hit-reaction recoil — FighterAdvance picks). */
            FighterAdvance(p1);
            FighterAdvance(p2);
        }

        p1->lastPad = p1->pad;
        p2->lastPad = p2->pad;
        lastPad = padBtns;
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;
    }

    /* Full unload so re-entering BATTLE works without leaks/double-free. */
    {
        int32 a;
        for (a = 0; a < ARENA_COUNT; a++) {
            if (gArenaCels[a] != NULL) { UnloadCel(gArenaCels[a]); gArenaCels[a] = NULL; }
        }
    }
    if (gHpBarBgP1 != NULL) { UnloadCel(gHpBarBgP1); gHpBarBgP1 = NULL; }
    if (gHpBarBgP2 != NULL) { UnloadCel(gHpBarBgP2); gHpBarBgP2 = NULL; }
    if (gHpBarFgP1 != NULL) { UnloadCel(gHpBarFgP1); gHpBarFgP1 = NULL; }
    if (gHpBarFgP2 != NULL) { UnloadCel(gHpBarFgP2); gHpBarFgP2 = NULL; }
    UnloadAllJaguarMoves();
    /* P2 is atlas-loaded now: free its atlas (buffer + CCB array) and NULL the
     * pool refs that pointed into it. */
    if (gP2IsAtlas) {
        int32 c = 0, mm, ff;
        for (mm = 0; mm < gP2HarPack->moves_count; mm++) {
            const har_move_t *mvp = &gP2HarPack->moves[mm];
            for (ff = 0; ff < mvp->frame_count; ff++) gP2Cels[c++] = NULL;
        }
        AtlasFree(&gP2Atlas);
        gP2IsAtlas = 0;
    }

    /* SFX cleanup: ehDisposeMixerInfo also disposes the attached effects;
     * null the gSfxEffects[] table so a future SfxStart starts clean. */
    if (gSfxMixer != NULL) {
        ehDisposeMixerInfo(gSfxMixer);
        gSfxMixer = NULL;
    }
    {
        int32 i;
        for (i = 0; i < 30; i++) gSfxEffects[i] = NULL;
    }
    /* Sessione 8 polish: swap fight BGM out, menu BGM back in so the
     * user lands on SCENE_MAIN with the menu theme already playing. */
    MusicStop();
    MusicStart("Music/MENU.AIFF");
    /* NB: CloseAudioFolio moved to main() -- closing it here would
     * prevent re-entering this scene (next MusicStart would fail with
     * no audio folio open). */
    printf("BattleSceneInit returning to dispatcher\n");
}

/* Sessione 7 step 2: SCENE_OPENOMF splash. Loads NETSET.CEL as a backdrop
 * (the OMF "Network Setup" screen reused as a banner — same asset already
 * shipped since sessione 1) and overlays text via blit_text. Exits to
 * SCENE_BATTLE on any controller button press, or auto-advances after
 * SPLASH_AUTO_FIELDS video fields (~3 s at 60 Hz). */
#define SPLASH_AUTO_FIELDS  180

static void
OpenomfSceneInit(scene_3do *s)
{
    Item   currentBitmapItem;
    int32  whichScreen;
    uint32 padBtns;
    uint32 lastPad;
    int32  padId;
    ControlPadEventData cped;
    CCB   *backdrop;
    int    fields;

    (void)s;

    backdrop = LoadCel("Art/NETSET.CEL", MEMTYPE_ANY);
    if (backdrop != NULL) {
        backdrop->ccb_XPos = 0  << 16;
        backdrop->ccb_YPos = 20 << 16;
    }

    /* Font CCBs are perma-loaded in main() -- nothing to init/free here. */

    whichScreen = 0;
    fields      = 0;
    /* Seed lastPad with the current pad state -- avoids treating a
     * held-from-previous-scene button as a fresh edge in frame 0. */
    padId   = GetControlPad(1, FALSE, &cped);
    lastPad = (padId >= 0) ? cped.cped_ButtonBits : 0;

    while (1) {
        CCB *textHead;

        currentBitmapItem = gScreenContext->sc_BitmapItems[whichScreen];
        SetVRAMPages(gVRAMIOReq,
                     gScreenContext->sc_Bitmaps[whichScreen]->bm_Buffer,
                     0x80108010,
                     gScreenContext->sc_nFrameBufferPages, -1);

        /* Reset the BlitText pool head so the two BlitTextChain calls
         * below grab fresh CCBs (otherwise repeated letters across the
         * two lines would self-collide). */
        BlitTextBeginFrame();

        /* Backdrop + text chained as a single CEL list. Text rendered
         * centered-ish on the 320x240 canvas. */
        textHead = BlitTextChain(110, 100, "3DO-OMF 2097");
        if (textHead == NULL) {
            /* No font loaded -- still draw the backdrop. */
            if (backdrop != NULL) {
                backdrop->ccb_Flags |= CCB_LAST;
                DrawCels(currentBitmapItem, backdrop);
            }
        } else {
            CCB *more = BlitTextChain(86, 140, "PRESS ANY BUTTON");
            /* Walk to the tail of textHead and chain `more` after it. */
            if (more != NULL) {
                CCB *tail = textHead;
                while (!(tail->ccb_Flags & CCB_LAST)) tail = tail->ccb_NextPtr;
                tail->ccb_Flags &= ~CCB_LAST;
                tail->ccb_NextPtr = more;
            }
            if (backdrop != NULL) {
                backdrop->ccb_Flags &= ~CCB_LAST;
                backdrop->ccb_NextPtr = textHead;
                DrawCels(currentBitmapItem, backdrop);
            } else {
                DrawCels(currentBitmapItem, textHead);
            }
        }

        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;

        padId = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;
        /* Edge-detect any new button press. */
        if (padBtns && !lastPad) break;
        lastPad = padBtns;

        if (++fields >= SPLASH_AUTO_FIELDS) break;
    }

    if (backdrop != NULL) UnloadCel(backdrop);

    /* Hand off to the main menu. */
    SceneSetNext(SCENE_MAIN);
    printf("OpenomfSceneInit -> SCENE_MAIN\n");
}

/* Scene definitions + thin main() that hands control to the dispatcher.
 * Sessione 7 step 2: OPENOMF splash precedes BATTLE; BATTLE is still the
 * terminal scene (its exit returns through main() to the OS). */
static scene_3do gOpenomfScene = {
    SCENE_OPENOMF,
    NULL,                /* userdata */
    OpenomfSceneInit,    /* init owns the splash loop */
    NULL,                /* free */
    NULL, NULL, NULL     /* tick / render / input (step 3+) */
};

static scene_3do gBattleScene = {
    SCENE_BATTLE,
    NULL,                /* userdata */
    BattleSceneInit,     /* init owns the legacy main loop for now */
    NULL,                /* free */
    NULL, NULL, NULL     /* tick / render / input (step 3+) */
};

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("3DO-OMF2097 Battle MVP starting\n");
    if (Initialize() < 0) return 1;

    /* Font glyphs are perma-loaded for the lifetime of the process so
     * every scene can use BlitText without paying a ~95 LoadCel hit on
     * each transition. ~27 KB total (16 KB heap, 11 KB BSS). */
    if (BlitTextInit() < 0) {
        printf("BlitTextInit failed -- menus will be invisible\n");
    }

    SceneRegister(&gOpenomfScene);
    MainSceneRegister();
    PilotSelectSceneRegister();
    SelectSceneRegister();
    VsSceneRegister();
    SceneRegister(&gBattleScene);

    /* Sessione 8 polish: kick off the menu BGM here so it loops
     * seamlessly through splash -> menu -> pilot -> HAR -> LOADING and
     * only switches to the per-fight theme inside BattleSceneInit. Every
     * scene's per-frame loop calls MusicService() to keep the spStreamer
     * fed (without that the music stutters then falls silent).
     * SFX (EffectsHandler mixer) stays inside BattleSceneInit since it's
     * only used by the fight (cheaper than carrying the SfxStart cost
     * for every menu screen). */
    MusicStart("Music/MENU.AIFF");

    SceneSetNext(SCENE_OPENOMF);
    SceneRunLoop();

    MusicStop();
    BlitTextShutdown();
    /* Platform shutdown: audio folio was opened in Initialize() and must
     * stay open across every scene transition (BATTLE -> MAIN -> BATTLE),
     * so we only close it now that the dispatcher loop has returned. */
    CloseAudioFolio();
    printf("clean exit (dispatcher)\n");
    return 0;
}
