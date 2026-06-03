/*
 * scene.h -- 3DO-OMF scene callback contract.
 *
 * Mirrors the subset of openomf's `scene_t` (game/protos/scene.h) that
 * applies to 3DO: per-scene init/free + per-tick callbacks. Drops the
 * SDL-event, clone, palette_transform, and debug callbacks from upstream
 * because they don't apply.
 *
 * Sessione 7 step 1 (incremental refactor): the BATTLE scene defines
 * `init` as the WHOLE current main-loop body (so init does not return
 * until the user presses Start). tick/render/input stay NULL. Step 2
 * splits init into proper init + per-tick callbacks.
 *
 * See port_3do/SESSIONE7_MENU_SCOPE.md §3.1 + §4.1.
 */
#ifndef SCENE_H
#define SCENE_H

#include "types.h"
#include "graphics.h"

typedef struct scene_3do scene_3do;

typedef void (*scene_init_cb)(scene_3do *s);
typedef void (*scene_free_cb)(scene_3do *s);
typedef void (*scene_tick_cb)(scene_3do *s);
typedef void (*scene_render_cb)(scene_3do *s, Item bitmap);
typedef void (*scene_input_cb)(scene_3do *s, uint32 buttons, uint32 edges);

struct scene_3do {
    int             id;
    void           *userdata;       /* per-scene state, opaque to dispatcher */
    scene_init_cb   init;           /* called on scene entry */
    scene_free_cb   free;           /* called on scene exit  */
    scene_tick_cb   tick;           /* one logic tick   (step 2+) */
    scene_render_cb render;         /* one render pass  (step 2+) */
    scene_input_cb  input;          /* per-tick input   (step 2+) */
};

/* Scene IDs. Keep stable across builds — used as indices into the
 * dispatcher's scene table. See SESSIONE7_MENU_SCOPE.md §2. */
enum {
    SCENE_NONE     = -1,
    SCENE_OPENOMF  = 0,
    SCENE_MAIN,
    SCENE_PILOT_SELECT,
    SCENE_SELECT,         /* a.k.a. SCENE_HAR_SELECT */
    SCENE_VS,             /* pre-fight matchup screen (sessione 10) */
    SCENE_BATTLE,
    SCENE_MAX
};

#endif /* SCENE_H */
