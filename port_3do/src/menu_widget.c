/*
 * menu_widget.c -- vertical menu widget, see menu_widget.h.
 */
#include "menu_widget.h"
#include "blit_text.h"

#include "celutils.h"
#include "controlpad.h"
#include "event.h"
#include "debug3do.h"

void
menu_widget_input(menu_widget *m, uint32 edges, scene_3do *owner)
{
    if (m == NULL || m->count <= 0) return;

    if (edges & ControlUp) {
        m->selected = (m->selected + m->count - 1) % m->count;
    }
    if (edges & ControlDown) {
        m->selected = (m->selected + 1) % m->count;
    }
    if (edges & ControlA) {
        if (m->selected >= 0 && m->selected < m->count) {
            const menu_item *it = &m->items[m->selected];
            if (it->enabled && it->on_select != NULL) {
                it->on_select(owner);
            }
        }
    }
}

/* Walk a CCB chain to its tail (the first CCB with CCB_LAST set). */
static CCB *
chain_tail(CCB *head)
{
    CCB *t = head;
    while (t != NULL && !(t->ccb_Flags & CCB_LAST)) t = t->ccb_NextPtr;
    return t;
}

/* Append `next` (a chain headed by next, terminated with CCB_LAST) onto
 * the tail of `head`. Returns the resulting head (or `next` if head was
 * NULL). */
static CCB *
chain_append(CCB *head, CCB *next)
{
    CCB *tail;
    if (next == NULL) return head;
    if (head == NULL) return next;
    tail = chain_tail(head);
    if (tail != NULL) {
        tail->ccb_Flags  &= ~CCB_LAST;
        tail->ccb_NextPtr = next;
    }
    return head;
}

void
menu_widget_render(menu_widget *m, Item bitmap)
{
    CCB *head = NULL;
    CCB *line;
    int  i, y;

    if (m == NULL || m->count <= 0) return;

    /* One BlitTextChain per item label. */
    for (i = 0; i < m->count; i++) {
        y = m->y + i * m->line_height;
        line = BlitTextChain(m->x, y, m->items[i].label);
        head = chain_append(head, line);
    }

    /* Cursor caret -- 10 px left of the selected line, so labels stay
     * aligned. Glyph is the small font's ">" (ASCII 62). */
    if (m->selected >= 0 && m->selected < m->count) {
        y = m->y + m->selected * m->line_height;
        line = BlitTextChain(m->x - 10, y, ">");
        head = chain_append(head, line);
    }

    if (head != NULL) DrawCels(bitmap, head);
}
