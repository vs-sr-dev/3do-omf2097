/*
 * menu_widget.h -- minimal vertical menu for 3DO scenes.
 *
 * Concept-port (not source-port) of openomf's game/gui/menu.h: the SDL
 * component graph is overkill for our 2-6 item menus, so we keep just
 * the list + selected cursor pattern. See SESSIONE7_MENU_SCOPE.md §3.3.
 *
 * The widget owns NO scene state itself -- callbacks receive the parent
 * scene_3do* (`owner`) so they can stash state on s->userdata or call
 * SceneSetNext/SceneQuit directly.
 *
 * Rendering uses blit_text.c -- caller MUST call BlitTextBeginFrame()
 * once per frame before invoking menu_widget_render (or any other
 * BlitText call in the same frame).
 */
#ifndef MENU_WIDGET_H
#define MENU_WIDGET_H

#include "types.h"
#include "graphics.h"
#include "scene.h"

typedef void (*menu_action_cb)(scene_3do *owner);

typedef struct {
    const char     *label;
    menu_action_cb  on_select;     /* invoked on A press if enabled */
    int             enabled;       /* 0 = greyed out (currently still rendered) */
} menu_item;

typedef struct {
    const menu_item *items;
    int  count;
    int  selected;                 /* 0..count-1 */
    int  x, y;                     /* top-left of FIRST item label, in 320x240 coords */
    int  line_height;              /* vertical spacing between items */
} menu_widget;

/* Apply newly-pressed buttons to the menu. `edges` = (cur_pad & ~prev_pad),
 * tested against ControlUp/ControlDown/ControlA. ControlA invokes the
 * selected item's on_select. */
void menu_widget_input(menu_widget *m, uint32 edges, scene_3do *owner);

/* Render menu items + cursor caret. Caller must:
 *   - call BlitTextBeginFrame() earlier in the frame
 *   - have already programmed the back-buffer via SetVRAMPages
 *   - optionally render a backdrop BEFORE this call */
void menu_widget_render(menu_widget *m, Item bitmap);

#endif /* MENU_WIDGET_H */
