/*
 * scene_pilot_select.c -- SCENE_PILOT_SELECT, canon "page 1" of the
 * pilot-then-HAR flow (per [[project-pilot-har-two-page-select]] +
 * sessione 8 ground-truth DOS screenshots).
 *
 * Canon layout (matched at 90% -- skipped: stats bars, bio text,
 * non-selected dimming, pulsing red BG animation; rendered as static
 * mid-red BG instead):
 *
 *   - MELEE_BG.CEL backdrop (menu chrome, OMF coord 0,0).
 *   - Top-left: big pilot face (anim[4] sprite N) ~57x57 at OMF (8, 5),
 *     plus pilot name in glyphs underneath.
 *   - Top-right: menu logo (anim[5] sprite 1, ONE MUST FALL 2097).
 *   - Bottom: 5x2 grid of small pilot face portraits (anim[3] sprites
 *     0..9) at the canon HAR-grid coordinates. Selected cell has a
 *     red_cell.CEL drawn underneath it, plus `>` `<` brackets either side.
 *   - Bottom-right: "CHOOSE YOUR PILOT" footer.
 *
 * Controls: D-pad navigates 5x2 (wrap on edge), A confirms ->
 * SCENE_SELECT (HAR-select page), B cancels -> SCENE_MAIN.
 *
 * NOVA (pilot 10) is the boss; never selectable.
 */
#include "scene_pilot_select.h"
#include "scene.h"
#include "scene_dispatch.h"
#include "blit_text.h"

#include "types.h"
#include "graphics.h"
#include "displayutils.h"
#include "celutils.h"
#include "controlpad.h"
#include "event.h"
#include "debug3do.h"

#include "pilots_data.h"
#include "portraits_data.h"
#include "bgm.h"

extern ScreenContext *gScreenContext;
extern Item gVBLIOReq;
extern Item gVRAMIOReq;

int gSelectedPilotId = 0;

#define GRID_COLS         5
#define GRID_ROWS         2
#define SCREEN_Y_OFFSET   20
#define SMALL_W           51
#define SMALL_H           36

/* Top-left big pilot face position. anim[4] sprites have small native
 * pos_y offsets (e.g. -7 for sprite 0) so the actual rendered y is
 * (PFACE_Y + sprite.pos_y + 20). We render the sprite *literally at*
 * (PFACE_X, PFACE_Y + 20) and accept the small native offset -- gives
 * each pilot a slightly different vertical sit, mirroring the OMF look. */
/* Pilot face top-left -- positioned per user 2026-05-21 feedback
 * (3 px left + 7 px up from the initial guess). */
#define PFACE_X           5
#define PFACE_Y           5

/* Logo (anim[5] sprite 1, 153x95 native, OMF anchor (164, 0)). */
#define LOGO_IDX          1
#define LOGO_X          164
#define LOGO_Y            0

/* Footer -- between the bio panel and the bottom grid (just above the
 * row 0 cells). */
#define FOOTER_X        185
#define FOOTER_Y        118

static CCB *gBackdrop = NULL;
static CCB *gLogo     = NULL;
static CCB *gRedCell  = NULL;
static CCB *gSmallCels[10];
static CCB *gBigCels[10];

static int gSmallLoaded = 0;
static int gBigLoaded   = 0;

static void
LoadAssets(void)
{
    int i;

    gBackdrop = LoadCel("Art/MENU/MELEE_BG.CEL", MEMTYPE_ANY);
    if (gBackdrop != NULL) {
        gBackdrop->ccb_XPos = 0  << 16;
        gBackdrop->ccb_YPos = SCREEN_Y_OFFSET << 16;
    }

    gRedCell = LoadCel(RED_CELL_PATH, MEMTYPE_ANY);

    if (LOGO_IDX < (int)LOGO_COUNT) {
        gLogo = LoadCel((char *)logo_sprites[LOGO_IDX].cel_path, MEMTYPE_ANY);
    }

    gSmallLoaded = (PILOT_SMALL_COUNT < 10) ? (int)PILOT_SMALL_COUNT : 10;
    for (i = 0; i < gSmallLoaded; i++) {
        gSmallCels[i] = LoadCel((char *)pilot_small_portraits[i].cel_path,
                                MEMTYPE_ANY);
        if ((i & 3) == 0) MusicService();   /* keep BGM buffer fed */
    }

    gBigLoaded = (PILOT_BIG_COUNT < 10) ? (int)PILOT_BIG_COUNT : 10;
    for (i = 0; i < gBigLoaded; i++) {
        gBigCels[i] = LoadCel((char *)pilot_big_portraits[i].cel_path,
                              MEMTYPE_ANY);
        if ((i & 3) == 0) MusicService();
    }
}

static void
UnloadAssets(void)
{
    int i;
    for (i = 0; i < gSmallLoaded; i++) {
        if (gSmallCels[i]) { UnloadCel(gSmallCels[i]); gSmallCels[i] = NULL; }
    }
    for (i = 0; i < gBigLoaded; i++) {
        if (gBigCels[i])   { UnloadCel(gBigCels[i]);   gBigCels[i]   = NULL; }
    }
    if (gLogo)     { UnloadCel(gLogo);     gLogo     = NULL; }
    if (gRedCell)  { UnloadCel(gRedCell);  gRedCell  = NULL; }
    if (gBackdrop) { UnloadCel(gBackdrop); gBackdrop = NULL; }
}

static void
PilotSelectSceneInit(scene_3do *s)
{
    Item                 currentBitmapItem;
    int32                whichScreen;
    uint32               padBtns;
    uint32               lastPad;
    int32                padId;
    ControlPadEventData  cped;
    int                  cursor_col, cursor_row;
    int                  i;

    (void)s;

    LoadAssets();

    cursor_col = gSelectedPilotId % GRID_COLS;
    cursor_row = gSelectedPilotId / GRID_COLS;
    if (cursor_row >= GRID_ROWS) { cursor_row = 0; cursor_col = 0; }

    whichScreen = 0;
    padId   = GetControlPad(1, FALSE, &cped);
    lastPad = (padId >= 0) ? cped.cped_ButtonBits : 0;

    while (SceneNextId() == SCENE_NONE && !SceneShouldQuit()) {
        CCB *line;
        int  cur_id = cursor_row * GRID_COLS + cursor_col;

        currentBitmapItem = gScreenContext->sc_BitmapItems[whichScreen];
        SetVRAMPages(gVRAMIOReq,
                     gScreenContext->sc_Bitmaps[whichScreen]->bm_Buffer,
                     0x80108010,
                     gScreenContext->sc_nFrameBufferPages, -1);

        BlitTextBeginFrame();

        /* Backdrop. */
        if (gBackdrop != NULL) {
            gBackdrop->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gBackdrop);
        }

        /* Top-right logo. */
        if (gLogo != NULL) {
            gLogo->ccb_XPos = LOGO_X << 16;
            gLogo->ccb_YPos = (LOGO_Y + SCREEN_Y_OFFSET) << 16;
            gLogo->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gLogo);
        }

        /* Top-left big pilot face for the cursored pilot. */
        if (cur_id >= 0 && cur_id < gBigLoaded && gBigCels[cur_id] != NULL) {
            gBigCels[cur_id]->ccb_XPos = PFACE_X << 16;
            gBigCels[cur_id]->ccb_YPos = (PFACE_Y + SCREEN_Y_OFFSET) << 16;
            gBigCels[cur_id]->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gBigCels[cur_id]);
        }

        /* Pilot name under the face, plus altpal indices as a poor-man's
         * "stats line" until we have real bios/stats data. */
        if (cur_id >= 0 && cur_id < (int)PILOTS_COUNT) {
            line = BlitTextChain(PFACE_X, PFACE_Y + SCREEN_Y_OFFSET + 65,
                                 pilots_data[cur_id].name);
            if (line != NULL) DrawCels(currentBitmapItem, line);
        }

        /* Red BG under the selected grid slot (drawn BEFORE the portrait
         * so it shows through). The red CEL is 51x36 fixed; we anchor it
         * at the selected slot's canon position. */
        if (gRedCell != NULL && cur_id >= 0 && cur_id < gSmallLoaded) {
            gRedCell->ccb_XPos = pilot_small_portraits[cur_id].pos_x << 16;
            gRedCell->ccb_YPos =
                (pilot_small_portraits[cur_id].pos_y + SCREEN_Y_OFFSET) << 16;
            gRedCell->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gRedCell);
        }

        /* Grid: 10 small pilot portraits at canon positions. */
        for (i = 0; i < gSmallLoaded; i++) {
            if (gSmallCels[i] == NULL) continue;
            gSmallCels[i]->ccb_XPos = pilot_small_portraits[i].pos_x << 16;
            gSmallCels[i]->ccb_YPos =
                (pilot_small_portraits[i].pos_y + SCREEN_Y_OFFSET) << 16;
            gSmallCels[i]->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gSmallCels[i]);
        }

        /* Cursor brackets around selected cell. */
        {
            int cell_left  = pilot_small_portraits[cur_id].pos_x - 8;
            int cell_right = pilot_small_portraits[cur_id].pos_x + SMALL_W;
            int cy = pilot_small_portraits[cur_id].pos_y + SCREEN_Y_OFFSET
                   + SMALL_H / 2 - 3;
            line = BlitTextChain(cell_left,  cy, ">");
            if (line != NULL) DrawCels(currentBitmapItem, line);
            line = BlitTextChain(cell_right, cy, "<");
            if (line != NULL) DrawCels(currentBitmapItem, line);
        }

        /* Footer. */
        line = BlitTextChain(FOOTER_X, FOOTER_Y, "CHOOSE YOUR PILOT");
        if (line != NULL) DrawCels(currentBitmapItem, line);

        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;

        padId   = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;
        {
            uint32 edges = padBtns & ~lastPad;
            if (edges & ControlLeft)  cursor_col = (cursor_col + GRID_COLS - 1) % GRID_COLS;
            if (edges & ControlRight) cursor_col = (cursor_col + 1) % GRID_COLS;
            if (edges & ControlUp)    cursor_row = (cursor_row + GRID_ROWS - 1) % GRID_ROWS;
            if (edges & ControlDown)  cursor_row = (cursor_row + 1) % GRID_ROWS;
            if (edges & ControlA) {
                gSelectedPilotId = cur_id;
                printf("PilotSelect confirm: id=%d (%s) altpals=(%d,%d,%d)\n",
                       cur_id, pilots_data[cur_id].name,
                       pilots_data[cur_id].color_1,
                       pilots_data[cur_id].color_2,
                       pilots_data[cur_id].color_3);
                SceneSetNext(SCENE_SELECT);
            }
            if (edges & ControlB) {
                printf("PilotSelect cancel -> SCENE_MAIN\n");
                SceneSetNext(SCENE_MAIN);
            }
        }
        lastPad = padBtns;
    }

    UnloadAssets();
}

static scene_3do gPilotSelectInst = {
    SCENE_PILOT_SELECT,
    NULL,
    PilotSelectSceneInit,
    NULL,
    NULL, NULL, NULL
};

void
PilotSelectSceneRegister(void)
{
    SceneRegister(&gPilotSelectInst);
}
