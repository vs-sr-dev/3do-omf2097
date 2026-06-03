/*
 * blit_text.c -- font runtime, see blit_text.h.
 *
 * Loads the 95 glyph CELs once and stores them as immutable "source"
 * CCBs. On each BlitTextChain call we shallow-copy from the source into
 * a per-frame CCB pool slot, then patch XPos/YPos/NextPtr. This lets a
 * single chain (or multiple chains in one frame) reference the same
 * source glyph at multiple positions -- the OMF small font has plenty
 * of repeated letters in real menu text ("PRESS", "BUTTON", etc.).
 *
 * BlitTextBeginFrame() must be called once per frame to reset the
 * pool head. Pool size BLIT_TEXT_POOL_SIZE is the maximum number of
 * NON-BLANK glyphs renderable per frame.
 */
#include "blit_text.h"

#include "celutils.h"
#include "mem.h"
#include "debug3do.h"

#include "font_small_data.h"

/* 256 non-blank glyphs / frame: ~10-12 lines, enough for the §1.1 DRAM-map
 * readout (header + 6 stage rows + arena + footer ≈ 188 glyphs). At 128 the
 * pool exhausted past line ~3 and later rows flickered. CCB is ~88 bytes so
 * the pool is ~22 KB BSS. */
#define BLIT_TEXT_POOL_SIZE  256

static CCB *gGlyphs[FONT_SMALL_GLYPH_COUNT] = {0};
static int  gGlyphsLoaded = 0;

static CCB  gPool[BLIT_TEXT_POOL_SIZE];
static int  gPoolHead = 0;

int
BlitTextInit(void)
{
    int i;
    if (gGlyphsLoaded) return 0;
    for (i = 0; i < FONT_SMALL_GLYPH_COUNT; i++) {
        gGlyphs[i] = LoadCel((char *)font_small_cel_paths[i], MEMTYPE_ANY);
        if (gGlyphs[i] == NULL) {
            printf("BlitTextInit: LoadCel %s failed\n",
                   font_small_cel_paths[i]);
        }
    }
    gGlyphsLoaded = 1;
    gPoolHead = 0;
    return 0;
}

void
BlitTextShutdown(void)
{
    int i;
    if (!gGlyphsLoaded) return;
    for (i = 0; i < FONT_SMALL_GLYPH_COUNT; i++) {
        if (gGlyphs[i] != NULL) {
            UnloadCel(gGlyphs[i]);
            gGlyphs[i] = NULL;
        }
    }
    gGlyphsLoaded = 0;
    gPoolHead = 0;
}

void
BlitTextBeginFrame(void)
{
    gPoolHead = 0;
}

CCB *
BlitTextChain(int x, int y, const char *str)
{
    CCB *head = NULL;
    CCB *prev = NULL;
    int  pen_x = x;
    int  i;

    if (!gGlyphsLoaded || str == NULL) return NULL;

    for (i = 0; str[i] != '\0'; i++) {
        int  c = (unsigned char)str[i];
        CCB *src;
        CCB *slot;

        if (c < FONT_SMALL_FIRST_ASCII ||
            c >= FONT_SMALL_FIRST_ASCII + FONT_SMALL_GLYPH_COUNT) {
            pen_x += FONT_SMALL_GLYPH_W;  /* unknown -> blank space */
            continue;
        }
        if (c == ' ') {
            pen_x += FONT_SMALL_GLYPH_W;
            continue;
        }
        src = gGlyphs[c - FONT_SMALL_FIRST_ASCII];
        if (src == NULL) {
            pen_x += FONT_SMALL_GLYPH_W;
            continue;
        }
        if (gPoolHead >= BLIT_TEXT_POOL_SIZE) {
            /* Pool exhausted -- terminate chain here. */
            break;
        }

        slot = &gPool[gPoolHead++];
        *slot = *src;       /* struct copy -- armcc supports this w/o <string.h> */
        slot->ccb_XPos    = pen_x << 16;
        slot->ccb_YPos    = y     << 16;
        slot->ccb_Flags  &= ~CCB_LAST;
        slot->ccb_NextPtr = NULL;

        if (head == NULL) head = slot;
        if (prev != NULL) prev->ccb_NextPtr = slot;
        prev = slot;
        pen_x += FONT_SMALL_GLYPH_W;
    }

    if (prev != NULL) prev->ccb_Flags |= CCB_LAST;
    return head;
}

void
BlitTextDraw(Item bitmap, int x, int y, const char *str)
{
    CCB *head;
    BlitTextBeginFrame();
    head = BlitTextChain(x, y, str);
    if (head != NULL) DrawCels(bitmap, head);
}
