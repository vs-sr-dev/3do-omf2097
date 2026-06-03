/*
 * scene_select.c -- SCENE_SELECT, canon "page 2" of the pilot-then-HAR
 * flow (per [[project-pilot-har-two-page-select]] + sessione 8
 * ground-truth screenshots).
 *
 * Canon layout matches PILOT_SELECT closely (same top-left pilot face,
 * top-right logo, footer text) but the bottom grid now shows HARs:
 *   - All 10 cells from anim[0] grayscale (non-selected look)
 *   - Selected cell overdrawn with anim[1] color version + red_cell BG
 *   - Cursor brackets around the selected cell
 *
 * Shareware/demo: only HARs 0/1/2 (JAGUAR/SHADOW/THORN) are playable
 * fights; cursor cycles only on those 3 slots. The other 7 grid cells
 * are visible but unselectable -- visual reminder of the full canon
 * roster.
 */
#include "scene_select.h"
#include "scene.h"
#include "scene_dispatch.h"
#include "scene_pilot_select.h"   /* gSelectedPilotId */
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
#include "har_idles_data.h"
#include "bgm.h"

extern ScreenContext *gScreenContext;
extern Item gVBLIOReq;
extern Item gVRAMIOReq;

int gSelectedHarId = SELECT_HAR_JAGUAR;

#define PLAYABLE_HAR_COUNT  3
#define SCREEN_Y_OFFSET    20
#define HAR_CELL_W         51
#define HAR_CELL_H         36

#define PFACE_X             5
#define PFACE_Y             5

#define LOGO_IDX            1
#define LOGO_X            164
#define LOGO_Y              0

#define FOOTER_X          185
#define FOOTER_Y          118

static CCB *gBackdrop = NULL;
static CCB *gLogo     = NULL;
static CCB *gRedCell  = NULL;
static CCB *gPilotBig = NULL;
static CCB *gHarDim[10];
static CCB *gHarColor[10];

#define MAX_IDLE_FRAMES_PER_HAR  12   /* canon max observed = 10 (shredder/chronos/electra) */
#define IDLE_PREVIEW_X          110   /* obj anchor X; sprite_top_left = anchor + sheet.pos */
#define IDLE_PREVIEW_Y          100
#define IDLE_TICK_DIVISOR         4   /* OMF ticks-per-step divided by N to fit our 60Hz loop */

static CCB *gIdleFrames[10][MAX_IDLE_FRAMES_PER_HAR];
static int  gIdleLoaded = 0;
static int  gIdleStepIdx = 0;          /* current step in cursored HAR's anim */
static int  gIdleStepTick = 0;         /* tick counter within current step */
static int  gIdleLastCursor = -1;      /* reset step idx when cursor moves */

static int gHarLoaded = 0;

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

    if (gSelectedPilotId >= 0 && gSelectedPilotId < (int)PILOT_BIG_COUNT) {
        gPilotBig = LoadCel(
            (char *)pilot_big_portraits[gSelectedPilotId].cel_path,
            MEMTYPE_ANY);
    }

    gHarLoaded = (HAR_CELLS_COUNT < 10) ? (int)HAR_CELLS_COUNT : 10;
    for (i = 0; i < gHarLoaded; i++) {
        gHarDim[i]   = LoadCel((char *)har_dim_cells[i].cel_path,   MEMTYPE_ANY);
        gHarColor[i] = LoadCel((char *)har_color_cells[i].cel_path, MEMTYPE_ANY);
        if ((i & 3) == 0) MusicService();   /* keep BGM buffer fed */
    }

    /* Preload idle anims for all 10 HARs (canon move 11 = ANIM_IDLE). Each
     * HAR has 5..10 frames at ~1-3 KB each -> ~120 KB DRAM peak. Unloaded
     * in UnloadAssets so BATTLE has full DRAM. */
    gIdleLoaded = (HAR_IDLES_COUNT < 10) ? (int)HAR_IDLES_COUNT : 10;
    for (i = 0; i < gIdleLoaded; i++) {
        int f;
        int nframes = har_idles[i].frame_count;
        if (nframes > MAX_IDLE_FRAMES_PER_HAR) nframes = MAX_IDLE_FRAMES_PER_HAR;
        for (f = 0; f < nframes; f++) {
            const idle_frame_t *fr = &har_idles[i].frames[f];
            if (fr->cel_path != NULL) {
                gIdleFrames[i][f] = LoadCel((char *)fr->cel_path, MEMTYPE_ANY);
            } else {
                gIdleFrames[i][f] = NULL;
            }
        }
        if ((i & 1) == 0) MusicService();
    }
    gIdleStepIdx   = 0;
    gIdleStepTick  = 0;
    gIdleLastCursor = -1;
}

static void
UnloadAssets(void)
{
    int i;
    /* Flush idle frame pool first (largest contributor: ~120 KB). */
    for (i = 0; i < gIdleLoaded; i++) {
        int f;
        for (f = 0; f < MAX_IDLE_FRAMES_PER_HAR; f++) {
            if (gIdleFrames[i][f]) {
                UnloadCel(gIdleFrames[i][f]);
                gIdleFrames[i][f] = NULL;
            }
        }
    }
    gIdleLoaded = 0;
    for (i = 0; i < gHarLoaded; i++) {
        if (gHarDim[i])   { UnloadCel(gHarDim[i]);   gHarDim[i]   = NULL; }
        if (gHarColor[i]) { UnloadCel(gHarColor[i]); gHarColor[i] = NULL; }
    }
    if (gPilotBig) { UnloadCel(gPilotBig); gPilotBig = NULL; }
    if (gLogo)     { UnloadCel(gLogo);     gLogo     = NULL; }
    if (gRedCell)  { UnloadCel(gRedCell);  gRedCell  = NULL; }
    if (gBackdrop) { UnloadCel(gBackdrop); gBackdrop = NULL; }
}

static void
SelectSceneInit(scene_3do *s)
{
    Item                 currentBitmapItem;
    int32                whichScreen;
    uint32               padBtns;
    uint32               lastPad;
    int32                padId;
    ControlPadEventData  cped;
    int                  cursor;
    int                  i;

    (void)s;

    LoadAssets();

    cursor = gSelectedHarId;
    if (cursor < 0 || cursor >= PLAYABLE_HAR_COUNT) cursor = 0;

    whichScreen = 0;
    padId   = GetControlPad(1, FALSE, &cped);
    lastPad = (padId >= 0) ? cped.cped_ButtonBits : 0;

    while (SceneNextId() == SCENE_NONE && !SceneShouldQuit()) {
        CCB *line;

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

        /* Top-left big pilot face (carried over from PILOT_SELECT). */
        if (gPilotBig != NULL) {
            gPilotBig->ccb_XPos = PFACE_X << 16;
            gPilotBig->ccb_YPos = (PFACE_Y + SCREEN_Y_OFFSET) << 16;
            gPilotBig->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gPilotBig);
        }
        if (gSelectedPilotId >= 0 && gSelectedPilotId < (int)PILOTS_COUNT) {
            line = BlitTextChain(PFACE_X, PFACE_Y + SCREEN_Y_OFFSET + 65,
                                 pilots_data[gSelectedPilotId].name);
            if (line != NULL) DrawCels(currentBitmapItem, line);
        }

        /* Cursored HAR idle anim preview (right of pilot face). Resets step
         * to 0 when the cursor moves to a different HAR slot. Advances one
         * tick per render-frame; current step's dwell determines when we
         * switch to the next step in the sequence. */
        if (cursor != gIdleLastCursor) {
            gIdleStepIdx = 0;
            gIdleStepTick = 0;
            gIdleLastCursor = cursor;
        }
        if (cursor >= 0 && cursor < gIdleLoaded &&
            har_idles[cursor].step_count > 0 &&
            har_idles[cursor].frame_count > 0) {
            const idle_har_t *cur = &har_idles[cursor];
            const idle_step_t *step;
            int sidx, fidx;
            CCB *fc;

            if (gIdleStepIdx >= cur->step_count) gIdleStepIdx = 0;
            step = &cur->steps[gIdleStepIdx];

            /* Advance tick counter; step at canon dwell scaled by divisor
             * so the idle plays at a comfortable speed on 60Hz loop. */
            gIdleStepTick++;
            if (gIdleStepTick >= step->dwell_ticks * IDLE_TICK_DIVISOR) {
                gIdleStepTick = 0;
                gIdleStepIdx++;
                if (gIdleStepIdx >= cur->step_count) gIdleStepIdx = 0;
                step = &cur->steps[gIdleStepIdx];
            }

            sidx = step->sprite_idx;
            if (sidx < 0 || sidx >= cur->frame_count)         sidx = 0;
            if (sidx >= MAX_IDLE_FRAMES_PER_HAR)              sidx = 0;
            fidx = sidx;
            fc = gIdleFrames[cursor][fidx];
            if (fc != NULL) {
                const idle_frame_t *fr = &cur->frames[fidx];
                fc->ccb_XPos = (IDLE_PREVIEW_X + fr->pos_x) << 16;
                fc->ccb_YPos = (IDLE_PREVIEW_Y + fr->pos_y +
                                SCREEN_Y_OFFSET) << 16;
                fc->ccb_Flags |= CCB_LAST;
                DrawCels(currentBitmapItem, fc);
            }
        }

        /* Red BG under the selected HAR cell. */
        if (gRedCell != NULL && cursor >= 0 && cursor < gHarLoaded) {
            gRedCell->ccb_XPos = har_color_cells[cursor].pos_x << 16;
            gRedCell->ccb_YPos =
                (har_color_cells[cursor].pos_y + SCREEN_Y_OFFSET) << 16;
            gRedCell->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gRedCell);
        }

        /* HAR grid: dim (darkened/desaturated) default, color overdraw on
         * the cursored slot -- mirrors canon DOS behavior. */
        for (i = 0; i < gHarLoaded; i++) {
            CCB *use = (i == cursor) ? gHarColor[i] : gHarDim[i];
            if (use == NULL) continue;
            use->ccb_XPos = har_color_cells[i].pos_x << 16;
            use->ccb_YPos = (har_color_cells[i].pos_y + SCREEN_Y_OFFSET) << 16;
            use->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, use);
        }

        /* Cursor brackets around the selected HAR cell. */
        {
            int cell_left  = har_color_cells[cursor].pos_x - 8;
            int cell_right = har_color_cells[cursor].pos_x + HAR_CELL_W;
            int cy = har_color_cells[cursor].pos_y + SCREEN_Y_OFFSET
                   + HAR_CELL_H / 2 - 3;
            line = BlitTextChain(cell_left,  cy, ">");
            if (line != NULL) DrawCels(currentBitmapItem, line);
            line = BlitTextChain(cell_right, cy, "<");
            if (line != NULL) DrawCels(currentBitmapItem, line);
        }

        /* Footer. */
        line = BlitTextChain(FOOTER_X, FOOTER_Y, "CHOOSE YOUR ROBOT");
        if (line != NULL) DrawCels(currentBitmapItem, line);

        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;

        padId   = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;
        {
            uint32 edges = padBtns & ~lastPad;
            /* Cursor cycles only on the playable trio for now. */
            if (edges & ControlLeft)  cursor = (cursor + PLAYABLE_HAR_COUNT - 1) % PLAYABLE_HAR_COUNT;
            if (edges & ControlRight) cursor = (cursor + 1) % PLAYABLE_HAR_COUNT;
            if (edges & ControlA) {
                gSelectedHarId = cursor;
                printf("SelectScene confirm: harId=%d -> SCENE_VS\n", cursor);
                SceneSetNext(SCENE_VS);
            }
            if (edges & ControlB) {
                printf("SelectScene cancel -> SCENE_PILOT_SELECT\n");
                SceneSetNext(SCENE_PILOT_SELECT);
            }
        }
        lastPad = padBtns;
    }

    UnloadAssets();
}

static scene_3do gSelectSceneInst = {
    SCENE_SELECT,
    NULL,
    SelectSceneInit,
    NULL,
    NULL, NULL, NULL
};

void
SelectSceneRegister(void)
{
    SceneRegister(&gSelectSceneInst);
}
