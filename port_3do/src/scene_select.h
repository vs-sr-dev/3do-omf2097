/*
 * scene_select.h -- export point for SCENE_SELECT's registration hook
 * + the selected-HAR-id global. See scene_select.c.
 */
#ifndef SCENE_SELECT_H
#define SCENE_SELECT_H

/* Index into a per-HAR table (0=Jaguar, 1=Shadow for the sessione 7
 * scope). Read by scene_battle to decide which fighter to spawn.
 * Persists across scene transitions -- a real game would carry it via
 * scene_3do.userdata + a chosen-player global per the openomf
 * `game_player_set_har` pattern, but for a 2-HAR demo a global is OK
 * (see SESSIONE7_MENU_SCOPE.md §9.4). */
extern int gSelectedHarId;

/* Match HAR_* enum in openomf common_defines.h:62-76 so a sessione 8
 * wiring of selection -> battle data can index a unified HAR table. */
#define SELECT_HAR_JAGUAR  0
#define SELECT_HAR_SHADOW  1
#define SELECT_HAR_THORN   2

void SelectSceneRegister(void);

#endif /* SCENE_SELECT_H */
