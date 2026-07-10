/* audio.c — OpenUG Audio module implementation. */
#include "audio.h"

volatile float g_eng_freq = 55.0f, g_eng_vol = 0.0f;
volatile float g_road_vol = 0.0f;
volatile float g_hit = 0.0f;
volatile float g_skid = 0.0f;

static unsigned g_rng = 22222;
static float frand(void) { g_rng = g_rng*1664525u + 1013904223u; return (int)(g_rng>>9)/4194304.0f - 1.0f; }

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static double phase = 0.0; static float hit = 0.0f, lp = 0.0f;
    int16_t *out = (int16_t *)stream;
    int n = len / 2;                 /* mono S16 */
    static float hp = 0.0f; static double sq = 0.0;
    float freq = g_eng_freq, vol = g_eng_vol, road = g_road_vol, skid = g_skid;
    if (g_hit > hit) hit = g_hit; g_hit = 0.0f;   /* latch a new collision */
    for (int i = 0; i < n; i++) {
        phase += freq / 44100.0; if (phase >= 1.0) phase -= 1.0;
        /* engine: sawtooth + octave */
        float saw  = 2.0f*(float)phase - 1.0f;
        double p2 = phase*2.0; p2 -= (int)p2;
        float saw2 = 2.0f*(float)p2 - 1.0f;
        float eng = (saw*0.6f + saw2*0.35f) * vol;
        /* road/wind: low-passed white noise scaled by speed */
        float nz = frand();
        lp += (nz - lp) * 0.25f;
        float env = lp * road;
        /* tyre screech: high-passed noise + a squeal square wave while drifting */
        hp += (nz - hp) * 0.5f;
        sq += 920.0/44100.0; if (sq >= 1.0) sq -= 1.0;
        float scr = ((nz - hp)*0.7f + (sq < 0.5 ? 0.15f : -0.15f)) * skid;
        /* collision: short noise burst that decays */
        float thud = frand() * hit; hit *= 0.9985f;
        float s = eng + env + scr + thud*0.8f;
        int v = (int)(s * 11000.0f);
        if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}

SDL_AudioDeviceID audio_init(void) {
    SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
    return adev;
}
