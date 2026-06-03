/*
 * scene_dispatch.h -- 3DO scene dispatcher API.
 *
 * Tiny replacement for openomf's multi-thousand-line game_state. Holds
 * one CURRENT scene pointer + one NEXT scene id + a global quit flag.
 * Scenes call SceneSetNext() to request a transition; the dispatcher
 * runs the swap when control returns to it.
 *
 * Step 1 model (sessione 7): the BATTLE scene's `init` callback owns
 * its own while-loop and only returns when the user exits the scene.
 * Dispatcher then runs `free`, checks `gNextId`, and either swaps or
 * exits. Step 2 will introduce per-tick callbacks via scene_3do.tick/
 * render/input.
 */
#ifndef SCENE_DISPATCH_H
#define SCENE_DISPATCH_H

#include "scene.h"

/* Register a scene with the dispatcher. Stored by `s->id`. Each scene
 * registers itself once at startup (from main() in hello.c for step 1). */
void  SceneRegister(scene_3do *s);

/* Request a scene transition. Takes effect when control returns to
 * SceneRunLoop. Pass SCENE_NONE to signal "no next scene -> exit loop". */
void  SceneSetNext(int id);

/* Request a hard quit (exit the dispatcher loop ASAP). */
void  SceneQuit(void);

/* Returns the ID of the scene currently executing, or SCENE_NONE if
 * the dispatcher hasn't started a scene yet. */
int   SceneCurrentId(void);

/* Returns the ID queued via SceneSetNext, or SCENE_NONE if none. */
int   SceneNextId(void);

/* Returns nonzero if SceneQuit was called. */
int   SceneShouldQuit(void);

/* Main loop. Returns when either (a) a scene exits and no next scene
 * is queued, or (b) SceneQuit was called. */
void  SceneRunLoop(void);

#endif /* SCENE_DISPATCH_H */
