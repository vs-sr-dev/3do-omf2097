/*
 * scene_vs.h -- export point for SCENE_VS, the pre-fight matchup screen.
 *
 * Sessione 10 minimal scaffold: VS.BK background + 2 pilot portraits
 * (player vs hardcoded AI opponent) + "press A to continue" footer.
 * Canon source: openomf-master/src/game/scenes/vs.c. The deferred work
 * (insults from LANG file, anim[5] HAR sheet over the bg, welder /
 * scientist / gantries animated overlays, arena-select widget when p2
 * is selectable) lands in S11+.
 *
 * Reuses MELEE.BK anim[4] pilot_big_portraits CELs (already in iso) --
 * those sprites are 57x57 and the VS.BK info dump showed VS.BK anim[4]
 * is the same 57x57 sheet, so we skip a duplicate extract for now.
 */
#ifndef SCENE_VS_H
#define SCENE_VS_H

/* Opponent index into pilot_big_portraits[]. Defaults to a deterministic
 * variant of gSelectedPilotId so the matchup is never self-vs-self;
 * S11 will swap this for the canon-AI pairing logic + lang_get insults. */
extern int gOpponentPilotId;

/* Index into vs_har_sheet[] (== OMF har_id 0..10). Defaults to SHADOW=1
 * with a fallback to THORN if P1 also chose SHADOW so the matchup stays
 * visually distinct. */
extern int gOpponentHarId;

void VsSceneRegister(void);

#endif /* SCENE_VS_H */
