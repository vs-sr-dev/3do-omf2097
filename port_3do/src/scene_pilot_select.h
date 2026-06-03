/*
 * scene_pilot_select.h -- export point for SCENE_PILOT_SELECT.
 *
 * Sessione 7 final polish: the canonical OMF select flow is pilot first,
 * HAR second (per openomf game/scenes/melee.c `local->page` field and
 * [[project-pilot-har-two-page-select]]). This is the first half.
 */
#ifndef SCENE_PILOT_SELECT_H
#define SCENE_PILOT_SELECT_H

/* Index into pilots_data[] (0..9). Sessione 8 will use this to drive
 * altpal PLUT swap on HAR CELs in battle. */
extern int gSelectedPilotId;

void PilotSelectSceneRegister(void);

#endif /* SCENE_PILOT_SELECT_H */
