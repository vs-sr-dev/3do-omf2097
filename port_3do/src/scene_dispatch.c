/*
 * scene_dispatch.c -- scene dispatcher, see scene_dispatch.h.
 *
 * Step 1 (sessione 7): minimal harness that calls init / free per
 * scene, and lets the scene's init run forever until it sets a next
 * scene id or quits. Per-tick callbacks (tick/render/input) ignored
 * until step 2 refactors the battle loop into per-frame work.
 */
#include "scene_dispatch.h"
#include "debug3do.h"

static scene_3do *gScenes[SCENE_MAX] = {0};
static int        gCurId  = SCENE_NONE;
static int        gNextId = SCENE_NONE;
static int        gQuit   = 0;

void
SceneRegister(scene_3do *s)
{
    if (s == NULL) return;
    if (s->id < 0 || s->id >= SCENE_MAX) {
        printf("SceneRegister: invalid id %d\n", s->id);
        return;
    }
    gScenes[s->id] = s;
}

void
SceneSetNext(int id)
{
    gNextId = id;
}

void
SceneQuit(void)
{
    gQuit = 1;
}

int
SceneCurrentId(void)
{
    return gCurId;
}

int
SceneNextId(void)
{
    return gNextId;
}

int
SceneShouldQuit(void)
{
    return gQuit;
}

void
SceneRunLoop(void)
{
    while (!gQuit) {
        scene_3do *s;

        if (gNextId == SCENE_NONE) break;
        if (gNextId < 0 || gNextId >= SCENE_MAX || gScenes[gNextId] == NULL) {
            printf("SceneRunLoop: no scene registered for id %d\n", gNextId);
            break;
        }

        gCurId  = gNextId;
        gNextId = SCENE_NONE;
        s       = gScenes[gCurId];

        if (s->init)  s->init(s);
        /* Step 2 hook: a per-tick loop calling s->tick/s->render here. */
        if (s->free)  s->free(s);
    }
    gCurId = SCENE_NONE;
}
