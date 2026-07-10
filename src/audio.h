/* audio.h — OpenUG Audio module: a procedural engine/road/skid/thud synth
 * (no audio assets). The main thread writes the g_* controls, the SDL audio
 * thread reads them — benign float races. */
#ifndef OPENUG_AUDIO_H
#define OPENUG_AUDIO_H

#include <SDL.h>

extern volatile float g_eng_freq;   /* engine pitch (Hz-ish) */
extern volatile float g_eng_vol;    /* engine volume */
extern volatile float g_road_vol;   /* tyre/wind noise, scales with speed */
extern volatile float g_hit;        /* collision thud amplitude (main sets, cb decays) */
extern volatile float g_skid;       /* tyre-screech volume while drifting */

/* Open + start the synth device. Returns 0 if unavailable (engine runs silent). */
SDL_AudioDeviceID audio_init(void);

#endif
