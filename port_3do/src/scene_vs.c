/*
 * scene_vs.c -- SCENE_VS, the pre-fight matchup screen.
 *
 * Sessione 10 minimal scaffold (see scene_vs.h header for full scope
 * notes). Renders:
 *   - VS.BK background CEL (full 320x200)
 *   - Player1 pilot portrait, left side
 *   - Player2 (AI) pilot portrait, right side (flipped horizontally
 *     via CCB_HDX sign to mirror canon facing-left behavior)
 *   - "PLAYER VS. OPPONENT" header text + "PRESS A" footer
 *
 * Input:
 *   A  -> SCENE_BATTLE
 *   B  -> SCENE_SELECT (cancel back to HAR pick)
 *
 * Deferred to S11+ (per [[project-post-s8-roadmap]]):
 *   - Insults text from ENGLISH.DAT (lang_get(749..990), 242 entries)
 *   - VS.BK anim[5] HAR sheet overlay over the bg
 *   - Animated welder / scientist / scrape decals
 *   - Arena-select widget (anim[3] with arena thumbnails)
 *   - Canon AI pairing logic (currently deterministic from pilot id)
 */
#include "scene_vs.h"
#include "scene.h"
#include "scene_dispatch.h"
#include "scene_pilot_select.h"   /* gSelectedPilotId */
#include "scene_select.h"         /* SCENE_SELECT id */
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
#include "insults_data.h"
#include "vs_har_sheet_data.h"

extern ScreenContext *gScreenContext;
extern Item gVBLIOReq;
extern Item gVRAMIOReq;

int gOpponentPilotId = -1;     /* (re)computed each scene entry */
int gOpponentHarId   = -1;     /* (re)computed each scene entry */

#define VS_HAR_ANCHOR_X     160
#define VS_HAR_ANCHOR_Y       0

#define GLYPH_PITCH_PX        6     /* BlitText glyph step */
#define LINE_HEIGHT_PX        8
#define INSULT_WRAP_CHARS    24     /* ~150px / 6px-per-glyph minus slack */

/* Render `str` starting at y, word-wrapping at INSULT_WRAP_CHARS, stripping
 * '\n', emitting one BlitText chain per line.
 *
 * If `right_align` is 0: anchor_x is the LEFT edge of each line.
 * If `right_align` is 1: anchor_x is the RIGHT edge of each line, and per-
 *   line draw X is shifted left by (line_chars * GLYPH_PITCH_PX) so the
 *   line's last glyph lands at anchor_x. Used for the P2 insult so it
 *   visually "exits" the right-side portrait.
 *
 * Stops after MAX_LINES lines to bound DRAM use. */
static void
DrawWrappedInsult(Item bitmap, int anchor_x, int y, int right_align,
                  const char *str)
{
    char buf[80];
    int  buflen = 0;
    int  word_start = 0;
    int  line_idx = 0;
    enum { MAX_LINES = 3 };
    int  i;
    int  len;
    CCB *chain;
    int  draw_x;

    if (str == NULL || *str == 0) return;
    len = (int)strlen(str);

    for (i = 0; i <= len; i++) {
        char c = (i < len) ? str[i] : 0;
        if (c == '\n') c = ' ';     /* fold linebreaks into spaces */

        if (c == 0 || c == ' ') {
            int word_len = i - word_start;
            int needed   = (buflen ? 1 : 0) + word_len;

            if (buflen + needed > INSULT_WRAP_CHARS && buflen > 0) {
                buf[buflen] = 0;
                draw_x = right_align
                       ? (anchor_x - buflen * GLYPH_PITCH_PX)
                       : anchor_x;
                chain = BlitTextChain(draw_x, y + line_idx * LINE_HEIGHT_PX, buf);
                if (chain != NULL) DrawCels(bitmap, chain);
                line_idx++;
                buflen = 0;
                if (line_idx >= MAX_LINES) return;
            }

            if (buflen > 0 && word_len > 0) buf[buflen++] = ' ';
            if (word_len > 0) {
                int copy = word_len;
                if (buflen + copy >= (int)sizeof(buf) - 1)
                    copy = (int)sizeof(buf) - 1 - buflen;
                memcpy(buf + buflen, str + word_start, copy);
                buflen += copy;
            }
            word_start = i + 1;
        }
    }

    if (buflen > 0 && line_idx < MAX_LINES) {
        buf[buflen] = 0;
        draw_x = right_align
               ? (anchor_x - buflen * GLYPH_PITCH_PX)
               : anchor_x;
        chain = BlitTextChain(draw_x, y + line_idx * LINE_HEIGHT_PX, buf);
        if (chain != NULL) DrawCels(bitmap, chain);
    }
}

#define SCREEN_W            320
#define SCREEN_H            200
#define P1_PORTRAIT_X         5
#define P2_PORTRAIT_X       258   /* SCREEN_W - 57 - 5 */
#define PORTRAIT_Y          135   /* lower area, leaves Y=0..134 for HAR anim[5] */

#define HEADER_X              0   /* centered by BlitText */
#define HEADER_Y             10
#define INSULT0_X            77   /* P1 insult: LEFT edge, just past P1 portrait */
#define INSULT0_Y           130
#define INSULT1_X           240   /* P2 insult: RIGHT edge, just before P2 portrait */
#define INSULT1_Y           165
#define FOOTER_X            140
#define FOOTER_Y            190

static CCB   *gBg            = NULL;
static CCB   *gP1Face        = NULL;
static CCB   *gP2Face        = NULL;
static int32  gP2HdxNative   = 0;   /* |HDX| captured at LoadCel time */
static int    gP2Width       = 57;  /* p2 portrait width for flip re-anchor */
static CCB   *gP1Har         = NULL;
static CCB   *gP2Har         = NULL;
static int32  gP1HarHdxNative = 0;
static int32  gP2HarHdxNative = 0;

/* P2 HAR pick (MILESTONE_1V1 §1.1). The battle only has TWO playable movesets
 * (JAGUAR=0, THORN=2 — SHADOW has no atlas), so the opponent must resolve to one
 * of those for the VS screen and the fight to AGREE (previously this returned
 * SHADOW=1 and the battle silently swapped it for Jaguar — a VS-vs-fight
 * mismatch). Deterministic from the opponent pilot id for reproducible
 * smoke-tests; mirror matches (P2 == P1 HAR) are allowed and handled (P2 loads
 * its own atlas instance). p1_har is unused now but kept for signature stability
 * if a future canon "favorite HAR per pilot" table reinstates the dependency. */
static int
ChooseOpponentHar(int p1_har)
{
    (void)p1_har;
    /* Pilot-id parity → a real, playable HAR. Even pilots fight as Jaguar,
     * odd as Thorn. gOpponentPilotId is already set by the caller. */
    return (gOpponentPilotId & 1) ? SELECT_HAR_THORN : SELECT_HAR_JAGUAR;
}

static int
ChooseOpponent(int p1)
{
    /* Skip slot 10 (NOVA boss has no canon pilot row) and avoid
     * self-match. Deterministic so S10 smoke-tests are reproducible. */
    int n = (int)PILOT_BIG_COUNT;     /* 11 */
    int candidate;

    if (n <= 1) return 0;
    if (p1 < 0 || p1 >= n) p1 = 0;
    if (n > (int)PILOTS_COUNT) n = (int)PILOTS_COUNT;   /* skip NOVA for now */

    candidate = (p1 + 5) % n;
    if (candidate == p1) candidate = (candidate + 1) % n;
    return candidate;
}

static void
LoadAssets(void)
{
    int p1 = gSelectedPilotId;
    int p2;

    gOpponentPilotId = ChooseOpponent(p1);
    p2 = gOpponentPilotId;

    gBg = LoadCel("Art/VS.CEL", MEMTYPE_ANY);
    if (gBg != NULL) {
        gBg->ccb_XPos = 0 << 16;
        gBg->ccb_YPos = 0 << 16;
    }

    if (p1 >= 0 && p1 < (int)PILOT_BIG_COUNT) {
        gP1Face = LoadCel((char *)pilot_big_portraits[p1].cel_path,
                          MEMTYPE_ANY);
    }
    if (p2 >= 0 && p2 < (int)PILOT_BIG_COUNT) {
        gP2Face = LoadCel((char *)pilot_big_portraits[p2].cel_path,
                          MEMTYPE_ANY);
        if (gP2Face != NULL) {
            /* Capture native HDX (3it CELs don't always come back at 0x10000 --
             * see [[project-3it-cel-hdx-vdy]]) so the flip can negate it
             * without losing scale. */
            gP2HdxNative = gP2Face->ccb_HDX;
            gP2Width     = pilot_big_portraits[p2].w;
        }
    }

    /* HAR sheet sprites: P1 from gSelectedHarId, P2 deterministic. */
    {
        int p1_har = gSelectedHarId;
        int p2_har;

        if (p1_har < 0 || p1_har >= (int)VS_HAR_SHEET_COUNT) p1_har = 0;
        p2_har = ChooseOpponentHar(p1_har);
        gOpponentHarId = p2_har;

        gP1Har = LoadCel((char *)vs_har_sheet[p1_har].cel_path_p1, MEMTYPE_ANY);
        if (gP1Har != NULL) gP1HarHdxNative = gP1Har->ccb_HDX;

        /* P2 uses the canon pal_offset=48 baked CEL set, picked PER
         * OPPONENT PILOT (canon palette_load_player_colors injects the
         * pilot's altpal into palette[49..95] at runtime; we baked one
         * variant per pilot at pipeline time). */
        {
            int opp_pilot = gOpponentPilotId;
            if (opp_pilot < 0 || opp_pilot >= VS_HAR_P2_PILOT_COUNT)
                opp_pilot = 0;
            gP2Har = LoadCel(
                (char *)vs_har_sheet[p2_har].cel_path_p2[opp_pilot],
                MEMTYPE_ANY);
            if (gP2Har != NULL) gP2HarHdxNative = gP2Har->ccb_HDX;
        }
    }
}

static void
UnloadAssets(void)
{
    if (gP2Har)  { UnloadCel(gP2Har);  gP2Har  = NULL; }
    if (gP1Har)  { UnloadCel(gP1Har);  gP1Har  = NULL; }
    if (gP2Face) { UnloadCel(gP2Face); gP2Face = NULL; }
    if (gP1Face) { UnloadCel(gP1Face); gP1Face = NULL; }
    if (gBg)     { UnloadCel(gBg);     gBg     = NULL; }
}

static void
VsSceneInit(scene_3do *s)
{
    Item                 currentBitmapItem;
    int32                whichScreen;
    uint32               padBtns;
    uint32               lastPad;
    int32                padId;
    ControlPadEventData  cped;
    char                 header[40];
    const char          *p1name = "?";
    const char          *p2name = "?";

    (void)s;

    LoadAssets();

    if (gSelectedPilotId >= 0 && gSelectedPilotId < (int)PILOTS_COUNT) {
        p1name = pilots_data[gSelectedPilotId].name;
    }
    if (gOpponentPilotId >= 0 && gOpponentPilotId < (int)PILOTS_COUNT) {
        p2name = pilots_data[gOpponentPilotId].name;
    }
    sprintf(header, "%s VS. %s", p1name, p2name);
    printf("VsScene: %s\n", header);

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

        /* Background. */
        if (gBg != NULL) {
            gBg->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gBg);
        }

        /* P1 HAR sprite, anchored at (VS_HAR_ANCHOR_X, VS_HAR_ANCHOR_Y) =
         * (160, 0). Sprite top-left = anchor + sheet.pos. Faces RIGHT --
         * normal orientation, no flip. */
        if (gP1Har != NULL) {
            int p1h = (gSelectedHarId >= 0 &&
                       gSelectedHarId < (int)VS_HAR_SHEET_COUNT)
                    ? gSelectedHarId : 0;
            int tl_x = VS_HAR_ANCHOR_X + vs_har_sheet[p1h].pos_x;
            int tl_y = VS_HAR_ANCHOR_Y + vs_har_sheet[p1h].pos_y;
            gP1Har->ccb_HDX  = gP1HarHdxNative;
            gP1Har->ccb_XPos = tl_x << 16;
            gP1Har->ccb_YPos = tl_y << 16;
            gP1Har->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gP1Har);
        }

        /* P2 HAR sprite, flipped HORIZONTALLY around the anchor X=160 so
         * it lands on the right side of the screen facing LEFT. After
         * mirror, sprite top-left X = anchor - (pos_x + w). 3DO CCB flip
         * pattern: negate HDX, set XPos to top-left + (w - 1). */
        if (gP2Har != NULL) {
            int p2h = (gOpponentHarId >= 0 &&
                       gOpponentHarId < (int)VS_HAR_SHEET_COUNT)
                    ? gOpponentHarId : 1;
            int w   = vs_har_sheet[p2h].w;
            int tl_x = VS_HAR_ANCHOR_X - vs_har_sheet[p2h].pos_x - w;
            int tl_y = VS_HAR_ANCHOR_Y + vs_har_sheet[p2h].pos_y;
            gP2Har->ccb_HDX  = -gP2HarHdxNative;
            gP2Har->ccb_XPos = (tl_x + w - 1) << 16;
            gP2Har->ccb_YPos = tl_y << 16;
            gP2Har->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gP2Har);
        }

        /* Player 1 portrait, left. */
        if (gP1Face != NULL) {
            gP1Face->ccb_XPos = P1_PORTRAIT_X << 16;
            gP1Face->ccb_YPos = PORTRAIT_Y    << 16;
            gP1Face->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gP1Face);
        }

        /* Player 2 portrait, right. Canon faces LEFT, so we flip the CEL
         * horizontally by negating HDX and re-anchoring XPos by (w-1)
         * (per the HAR flip pattern in hello.c PrepareJaguarCel). */
        if (gP2Face != NULL) {
            gP2Face->ccb_HDX  = -gP2HdxNative;
            gP2Face->ccb_XPos = (P2_PORTRAIT_X + gP2Width - 1) << 16;
            gP2Face->ccb_YPos = PORTRAIT_Y << 16;
            gP2Face->ccb_Flags |= CCB_LAST;
            DrawCels(currentBitmapItem, gP2Face);
        }

        /* "PILOT VS. OPPONENT" header. */
        line = BlitTextChain(HEADER_X + 70, HEADER_Y, header);
        if (line != NULL) DrawCels(currentBitmapItem, line);

        /* Insults (canon vs.c L686-690 + L468-470, 1-player mode):
         *   insult[0] = lang_get(749 + 11*p1 + p2) at (77, 150)
         *   insult[1] = lang_get(870 + 11*p2 + p1) at (110, 170)
         * Stored as a flat 121-entry array indexed (11*p1 + p2). */
        if (gSelectedPilotId >= 0 && gSelectedPilotId < (int)PILOTS_COUNT &&
            gOpponentPilotId >= 0 && gOpponentPilotId < (int)PILOTS_COUNT) {
            int idx0 = LANG_INSULT_STRIDE * gSelectedPilotId + gOpponentPilotId;
            int idx1 = LANG_INSULT_STRIDE * gOpponentPilotId + gSelectedPilotId;
            if (idx0 >= 0 && idx0 < (int)LANG_INSULT_P1_COUNT) {
                DrawWrappedInsult(currentBitmapItem, INSULT0_X, INSULT0_Y,
                                  0 /* left-align */, lang_insult_p1[idx0]);
            }
            if (idx1 >= 0 && idx1 < (int)LANG_INSULT_P2_COUNT) {
                DrawWrappedInsult(currentBitmapItem, INSULT1_X, INSULT1_Y,
                                  1 /* right-align */, lang_insult_p2[idx1]);
            }
        }

        /* Footer prompt. */
        line = BlitTextChain(FOOTER_X, FOOTER_Y, "PRESS A");
        if (line != NULL) DrawCels(currentBitmapItem, line);

        DisplayScreen(gScreenContext->sc_Screens[whichScreen], 0);
        WaitVBL(gVBLIOReq, 1);
        MusicService();
        whichScreen = (whichScreen + 1) % gScreenContext->sc_nScreens;

        padId   = GetControlPad(1, FALSE, &cped);
        padBtns = (padId >= 0) ? cped.cped_ButtonBits : 0;
        {
            uint32 edges = padBtns & ~lastPad;
            if (edges & ControlA) {
                printf("VsScene confirm -> SCENE_BATTLE\n");
                SceneSetNext(SCENE_BATTLE);
            }
            if (edges & ControlB) {
                printf("VsScene cancel -> SCENE_SELECT\n");
                SceneSetNext(SCENE_SELECT);
            }
        }
        lastPad = padBtns;
    }

    UnloadAssets();
}

static scene_3do gVsSceneInst = {
    SCENE_VS,
    NULL,
    VsSceneInit,
    NULL,
    NULL, NULL, NULL
};

void
VsSceneRegister(void)
{
    SceneRegister(&gVsSceneInst);
}
