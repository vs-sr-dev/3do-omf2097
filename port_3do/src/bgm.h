/*
 * bgm.h -- background music API exposed by hello.c so scene_*.c files
 * can pump MusicService() in their own main loops.
 *
 * Sessione 8 polish 2026-05-21: music is started once at main() entry
 * (before SceneRunLoop) and runs continuously through every scene's
 * loop until SceneQuit triggers MusicStop + CloseAudioFolio at exit.
 * Each scene calls MusicService() per frame to keep the spStreamer
 * fed; without it the music stutters then stops.
 */
#ifndef BGM_H
#define BGM_H

void MusicStart(const char *filename);
void MusicStop(void);
void MusicService(void);

/* Halt audio output without tearing down the player / instruments /
 * 128 KB ring buffer (per [[project-audio-load-ordering]]). Use this
 * during heavy LoadCel storms to silence the BGM stutter while keeping
 * the music buffer's memory allocated -- otherwise a later MusicStop+
 * MusicStart fails to find a contiguous 128 KB block. */
void MusicPause(void);

#endif /* BGM_H */
