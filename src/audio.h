/* audio.h — OpenUG Audio module: a procedural engine/road/skid/thud synth
 * (no audio assets). The main thread writes the g_* controls, the SDL audio
 * thread reads them — benign float races. */
#ifndef OPENUG_AUDIO_H
#define OPENUG_AUDIO_H

#include <SDL.h>
#include <stdint.h>

/* --- engine synth: 3 RPM bands (idle/powerband/scream) crossfaded by RPM,
 * timbre + volume shaped by load. All bands pitch-track the same engine
 * fundamental (base_freq * rpm/center); what crossfades is the harmonic mix. */
#define ENG_IDLE_RPM      1000.0f
#define ENG_REDLINE_RPM   8000.0f
#define ENG_UPSHIFT_RPM   7000.0f
#define ENG_DOWNSHIFT_RPM 3200.0f
#define ENG_SHIFT_S       0.15f     /* clutch/shift-cut window, seconds */
#define ENG_GEARS         6

typedef struct {
    float base_freq;      /* fundamental at this band's center RPM */
    float phase;          /* phase accumulator (audio thread) */
    float volume;         /* rpm-weight * load mix (audio thread) */
} AudioBand;

/* Ginsu-style sample band: a decoded PCM loop from the game's own .abk
 * engine bank, pitch-shifted by a fractional read cursor. */
typedef struct {
    int16_t *samples;       /* decoded 16-bit mono PCM loop */
    uint32_t sample_count;
    float playback_index;   /* fractional read cursor (audio thread) */
    float base_freq;        /* estimated fundamental of the recording, Hz */
    float srate;            /* recording sample rate, Hz */
    float volume;           /* rpm-weight * accel/decel mix (audio thread) */
} AudioSampleBand;

/* A decoded Gnsu20 RPM sweep (SOUND/ENGINE/GIN_*.gin): one continuous
 * recording of the engine revving rpm_min -> rpm_max, with the file's own
 * rpm->sample-position curve and cycle-aligned grain (loop point) table. */
typedef struct {
    int16_t *samples;
    uint32_t total_samples;
    float rpm_min, rpm_max;
    uint32_t sample_rate;
    uint32_t *rpm_pos;      /* n_rpm+1 sample positions at evenly spaced RPMs */
    int n_rpm;
    uint32_t *grain;        /* n_grain+1 ascending cycle-aligned loop points */
    int n_grain;
} GinsuSweep;

typedef struct {
    GinsuSweep accel_sweep, decel_sweep;
    float play_cursor[2];   /* per-sweep looping cursor (accel, decel) */
    int   grain_idx[2];     /* per-sweep current grain window */
    float grain_rpm[2];     /* recorded rpm at the current grain (cached) */
    /* sub-grain crossfade: a secondary cursor sprouts at the rpm-target
       grain and fades in over ~12ms, then becomes the primary */
    int   xf_on[2], xf_grain[2];
    float xf_cursor[2], xf_rpm[2], xf_t[2];
    int   decel_is_alias;   /* decel_sweep shares accel's buffers (no _DCL file) */
    int   has_gin_data;
} Ginsu20State;

typedef struct {
    float current_rpm;              /* audio thread: smoothed toward target */
    volatile float target_rpm;      /* main thread writes */
    volatile float load;            /* main thread writes: 0 coast .. 1 full throttle */
    float current_load;             /* audio thread: smoothed load */
    AudioBand bands[3];             /* low / mid / high (procedural fallback) */
    AudioSampleBand accel_bands[3]; /* .abk Ginsu loops under load */
    AudioSampleBand decel_bands[3]; /* .abk Ginsu loops on coast */
    AudioSampleBand idle_band;      /* stationary loop, blended out by ~2200rpm */
    Ginsu20State gin;               /* native Gnsu20 sweeps (preferred) */
    int sample_mode;                /* 1 = .abk loops loaded, 0 = synth fallback */
    volatile float master_volume;   /* main thread writes */
} EngineSynthState;

extern EngineSynthState g_engine;
extern volatile float g_road_vol;   /* tyre/wind noise, scales with speed */
extern volatile float g_hit;        /* collision thud amplitude (main sets, cb decays) */
extern volatile float g_skid;       /* tyre-screech volume while drifting */

/* RPM crossfade weights for the 3 bands (pure function, used by the synth
 * and the selftest). */
void eng_band_weights(float rpm, float w[3]);

/* 6-speed virtual gearbox: per-frame step. speed_frac = |speed|/top (0..1),
 * dt in seconds. Advances *gear / *shift_t and writes g_engine.target_rpm +
 * .load; during a shift the load is cut and revs sag toward idle. */
void eng_gearbox_step(float speed_frac, float throttle, float dt,
                      int *gear, float *shift_t);

/* Pick an engine bank (0..40) for a car folder name. Deterministic name
 * hash — every car gets a stable, distinct voice. NOT the game's authentic
 * assignment: no car->bank table exists in the data; the real per-car
 * recordings are the named Gnsu20 files (SOUND/ENGINE/GIN_*.gin). */
int audio_bank_for_car(const char *carname);

/* Load the game's engine sound bank (SOUND/ENGINE/CAR_%02d_ENG_MB_SPU.abk)
 * into g_engine's idle/accel/decel bands, freeing any previous bank. Call
 * BEFORE audio_init (no locking). Returns loops decoded (7), 0 = synth. */
int audio_load_engine_bank(const char *dataroot, int carbank);

/* Load the car's native Gnsu20 sweeps: scans SOUND/ENGINE for GIN_*.gin
 * whose filename contains the car folder name (accel) and its _DCL/Decel
 * variant (decel; falls back to the accel sweep if absent). Call BEFORE
 * audio_init. Returns sweeps loaded (1-2), 0 = none (use the .abk bank). */
int audio_load_ginsu_sweeps(const char *dataroot, const char *carname);

/* Open + start the synth device. Returns 0 if unavailable (engine runs silent). */
SDL_AudioDeviceID audio_init(void);
void audio_selftest(void);   /* asserts the band-crossfade math */

#endif
