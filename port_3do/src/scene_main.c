/*
 * scene_main.c -- SCENE_MAIN, the main-menu scene.
 *
 * Parity rewrite (sessione 11): replaces the 2-item stub with the FULL
 * canon OMF:2097 main menu, derived verbatim from
 * openomf-master/src/game/scenes/mainmenu/menu_main.c (item set + order +
 * labels) and openomf-master/src/game/scenes/mainmenu.c (the gui_frame
 * dialog box at (165,5) 151x119). Rationale: the menu is on the path of
 * every boot, so making it look canon "abbellisce" every future test run.
 *
 * Dispatch wiring follows SESSIONE7_MENU_SCOPE.md: only ONE PLAYER GAME and
 * QUIT are live this session. The remaining canon items are present for
 * structural parity but stubbed (on_todo) -- trimming/disabling the ones the
 * 3DO port won't ship is an explicit LATER phase, not this one.
 *
 *   canon item        canon target (menu_main.c)   our wiring
 *   ----------------   --------------------------   ------------------------
 *   ONE PLAYER GAME    SCENE_MELEE (pilot first)    SCENE_PILOT_SELECT  [live]
 *   TWO PLAYER GAME    SCENE_MELEE (2 humans)       stub (no P2 input yet)
 *   TOURNAMENT PLAY    SCENE_MECHLAB                stub (deferred)
 *   NETWORK PLAY       net submenu                  stub (cut on 3DO)
 *   CONFIGURATION      config submenu               stub (cut on 3DO)
 *   GAMEPLAY           gameplay submenu             stub (speed slider TBD)
 *   HELP               help submenu                 stub (deferred)
 *   DEMO               SCENE_VS (demo)              stub (deferred)
 *   SCOREBOARD         SCENE_SCOREBOARD             stub (deferred)
 *   QUIT               SCENE_CREDITS                SceneQuit()         [live]
 */
#include "scene_main.h"
#include "scene.h"
#include "scene_dispatch.h"
#include "menu_widget.h"
#include "blit_text.h"
#include "bgm.h"

#include "types.h"
#include "graphics.h"
#include "displayutils.h"
#include "celutils.h"
#include "controlpad.h"
#include "event.h"
#include "debug3do.h"

/* Platform globals owned by hello.c. */
extern ScreenContext *gScreenContext;
extern Item gVBLIOReq;
extern Item gVRAMIOReq;

/* Canon dialog frame, openomf mainmenu.c:123 gui_frame(165, 5, 151, 119).
 * Our canvas is 320x240 (200-tall OMF canvas centered with a +20 Y offset,
 * the same mapping the arena uses), so the box sits at screen (165, 25). The
 * CEL is baked at exact 151x119 by host_tools/gen_menu_frame_cels.sh. */
#define FRAME_X   165
#define FRAME_Y   25
#define FRAME_CEL "Art/menu_frame.CEL"

/* Item layout INSIDE the frame: left padding for labels, caret 10px further
 * left (just inside the border), 11px line pitch fits all 10 items in the
 * 119px-tall box. */
#define MENU_LABEL_X    176
#define MENU_FIRST_Y    33
#define MENU_LINE_H     11

static CCB *gFrameCel = NULL;

/* ---- item callbacks ---- */

static void
on_one_player(scene_3do *owner)
{
    (void)owner;
    /* Canon ONE PLAYER GAME -> SCENE_MELEE; our melee is pilot-first then
     * HAR (per [[project-pilot-har-two-page-select]]). */
    SceneSetNext(SCENE_PILOT_SELECT);
}

static void
on_quit(scene_3do *owner)
{
    (void)owner;
    /* Canon QUIT -> SCENE_CREDITS; we have no credits scene yet, so exit. */
    SceneQuit();
}

/* Structural-parity items not wired this session. Honest no-op: log so the
 * dev build shows the press registered, but take no action. */
static void
on_todo(scene_3do *owner)
{
    (void)owner;
    printf("scene_main: menu item not implemented yet\n");
}

static const menu_item gMainItems[] = {
    { "ONE PLAYER GAME", on_one_player, 1 },
    { "TWO PLAYER GAME", on_todo,       1 },
    { "TOURNAMENT PLAY", on_todo,       1 },
    { "NETWORK PLAY",    on_todo,       1 },
    { "CONFIGURATION",   on_todo,       1 },
    { "GAMEPLAY",        on_todo,       1 },
    { "HELP",            on_todo,       1 },
    { "DEMO",            on_todo,       1 },
    { "SCOREBOARD",      on_todo,       1 },
    { "QUIT",            on_quit,       1 },
};

static menu_widget gMainMenu = {
    gMainItems,
    sizeof(gMainItems) / sizeof(gMainItems[0]),
    0,                              /* selected */
    MENU_LABEL_X, MENU_FIRST_Y,     /* x, y */
    MENU_LINE_H                     /* line_height */
};

static void
MainSceneInit(scene_3do *s)
{
    Item                 currentBitmapItem;
    int32                whichScreen;
    uint32               padBtns;
    uint32               lastPad;
    int32                padId;
    ControlPadEventData  cped;

    whichScreen = 0;

    /* Canon dialog box behind the menu. Loaded per-scene-entry, freed when
     * the menu loop exits (this init owns the whole loop). */
    gFrameCel = LoadCel(FRAME_CEL, MEMTYPE_ANY);
    if (gFrameCel != NULL) {
        gFrameCel->ccb_XPos = FRAME_X << 16;
        gFrameCel->ccb_YPos = FRAME_Y << 16;
        gFrameCel->ccb_Flags |= CCB_LAST;   /* standalone 1-CCB list */
    } else {
        printf("scene_main: LoadCel %s failed\n", FRAME_CEL);
    }

    /* Seed lastPad with the CURRENT pad state so any button still held from
     * the previous scene's exit press (typically A from the splash) does NOT
     * register as a fresh edge in the first frame of this menu. */
    padId   = GetControlPad(1, FALSE, &cped);
    lastPad = (padId >= 0) ? cped.cped_ButtonBits : 0;
    /* Reset cursor each entry to the menu. */
    gMainMenu.selected = 0;

    while (SceneNextId() == SCENE_NONE && !SceneShouldQuit()) {
        CCB *titleHead;

        currentBitmapItem = gScreenContext->sc_BitmapItems[whichScreen];
        SetVRAMPages(gVRAMIOReq,
                     gScreenContext->sc_Bitmaps[whichScreen]->bm_Buffer,
                     0x80108010,
                     gScreenContext->sc_nFrameBufferPages, -1);

        /* Dialog box first (background), then text on top. */
        if (gFrameCel != NULL) DrawCels(currentBitmapItem, gFrameCel);

        BlitTextBeginFrame();

        /* Branding on the left, clear of the right-hand dialog box. */
        titleHead = BlitTextChain(24, 104, "3DO-OMF 2097");
        if (titleHead != NULL) DrawCels(currentBitmapItem, titleHead);

        menu_widget_render(&gMainMenu, currentBitmapItem);

        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;

        padId   = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;
        menu_widget_input(&gMainMenu, padBtns & ~lastPad, s);
        lastPad = padBtns;
    }

    if (gFrameCel != NULL) {
        UnloadCel(gFrameCel);
        gFrameCel = NULL;
    }

    printf("MainSceneInit -> next=%d quit=%d\n",
           (int)SceneNextId(), (int)SceneShouldQuit());
}

static scene_3do gMainSceneInst = {
    SCENE_MAIN,
    NULL,                    /* userdata */
    MainSceneInit,           /* init owns the menu loop */
    NULL,                    /* free */
    NULL, NULL, NULL         /* tick / render / input */
};

void
MainSceneRegister(void)
{
    SceneRegister(&gMainSceneInst);
}
