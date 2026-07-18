/* audio.c — OpenUG Audio module implementation. */
#include "audio.h"
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* band center RPMs (idle / powerband / scream) */
static const float ENG_CENTER[3] = { 1500.0f, 4000.0f, 7000.0f };
/* 4-cyl 4-stroke: 2 firing events per rev -> fundamental = rpm/60*2 */
#define ENG_FIRE_HZ(rpm) ((rpm) / 60.0f * 2.0f)

EngineSynthState g_engine = {
    .current_rpm = ENG_IDLE_RPM, .target_rpm = ENG_IDLE_RPM,
    .load = 0.0f, .current_load = 0.0f, .master_volume = 0.0f,
    .bands = { { ENG_FIRE_HZ(1500.0f), 0, 0 },
               { ENG_FIRE_HZ(4000.0f), 0, 0 },
               { ENG_FIRE_HZ(7000.0f), 0, 0 } },
};
volatile float g_road_vol = 0.0f;
volatile float g_hit = 0.0f;
volatile float g_skid = 0.0f;

/* piecewise-linear crossfade: full L at/below C_L, full H at/above C_H */
void eng_band_weights(float rpm, float w[3]) {
    w[0] = w[1] = w[2] = 0.0f;
    if (rpm <= ENG_CENTER[0])      w[0] = 1.0f;
    else if (rpm < ENG_CENTER[1]) {
        float t = (rpm - ENG_CENTER[0]) / (ENG_CENTER[1] - ENG_CENTER[0]);
        w[0] = 1.0f - t; w[1] = t;
    } else if (rpm < ENG_CENTER[2]) {
        float t = (rpm - ENG_CENTER[1]) / (ENG_CENTER[2] - ENG_CENTER[1]);
        w[1] = 1.0f - t; w[2] = t;
    } else                         w[2] = 1.0f;
}

/* 6-speed gearbox: rpm-per-speed ratios (index 1..6; [0] unused). Scaled so
 * 6th at full speed_frac sits just under redline (1000 + 25*272 = 7800). */
static const float ENG_GEAR_RATIO[7] = { 0, 130, 82, 56, 41, 32, 25 };
#define ENG_RATIO_SCALE (6800.0f / 25.0f)

void eng_gearbox_step(float speed_frac, float throttle, float dt,
                      int *gear, float *shift_t) {
    if (*shift_t > 0.0f) {           /* clutch window: cut load, revs sag */
        *shift_t -= dt;
        g_engine.load = 0.0f;
        g_engine.target_rpm += (ENG_IDLE_RPM - g_engine.target_rpm) * 0.18f;
        return;
    }
    float rpm = ENG_IDLE_RPM + speed_frac * ENG_GEAR_RATIO[*gear] * ENG_RATIO_SCALE;
    if (rpm > ENG_UPSHIFT_RPM && *gear < ENG_GEARS) {
        (*gear)++; *shift_t = ENG_SHIFT_S; return;
    }
    if (rpm < ENG_DOWNSHIFT_RPM && *gear > 1) {
        (*gear)--; *shift_t = ENG_SHIFT_S; return;
    }
    if (rpm > ENG_REDLINE_RPM) rpm = ENG_REDLINE_RPM;
    g_engine.target_rpm = rpm;
    g_engine.load = throttle > 0.0f ? 1.0f : 0.0f;
}

/* per-band harmonic recipe: {multiple of the fundamental, amplitude} pairs.
 * low = lumpy idle (sub + fundamental), mid = growl, high = scream. */
typedef struct { float mult, amp; } Harm;
static const Harm ENG_HARM[3][4] = {
    { {0.5f,0.35f}, {1,0.80f}, {2,0.25f}, {3,0.10f} },
    { {1,0.45f},    {2,0.65f}, {3,0.40f}, {4,0.20f} },
    { {2,0.35f},    {3,0.55f}, {4,0.45f}, {6,0.30f} },
};

static unsigned g_rng = 22222;
static float frand(void) { g_rng = g_rng*1664525u + 1013904223u; return (int)(g_rng>>9)/4194304.0f - 1.0f; }

/* auxiliary acoustics (audio-thread state): turbo whine, blow-off valve,
 * overrun exhaust pops */
typedef struct {
    float turbo_phase;
    float hiload;            /* recent-max load, ~150ms decay */
    float bov_env, bov_hp;   /* blow-off hiss envelope + highpass state */
    float pop_env, pop_lp;   /* backfire envelope + lowpass (thump) state */
    float pop_timer;
} AuxAudio;
static AuxAudio g_aux;

/* ---- Ginsu sample bank (.abk) loading ----
 * ABKC header: bank offset u32 @+0x18, bank size u32 @+0x24. The bank ("BNKl")
 * holds PT ("platform") headers: TLV of (tag u8, len u8, value big-endian) —
 * 0x84 sample rate, 0x85 sample count, 0x88 data offset (bank-relative).
 * Stream data = EA-XA mono ADPCM: 15-byte frames (header: coef index high
 * nibble, shift low nibble; 28 nibbles) mixed with 0xEE-marked frames =
 * predictor reseed (2x s16be @+1) + 28 uncompressed s16be samples @+5.
 * All of this was derived from the data and cross-validated: decode is
 * exact at every resync frame and loops come out ~perfectly periodic. */

static const int EAXA_C1[4] = { 0, 240, 460, 392 };
static const int EAXA_C2[4] = { 0, 0, -208, -220 };
static int s16be(const unsigned char *p) { int v = (p[0] << 8) | p[1]; return v > 32767 ? v - 65536 : v; }

static void eaxa_decode(const unsigned char *d, int16_t *out, uint32_t nsamp) {
    int prev = 0, prev2 = 0; uint32_t n = 0;
    while (n < nsamp) {
        if (d[0] == 0xEE) {                       /* uncompressed + reseed */
            uint32_t take = nsamp - n < 28 ? nsamp - n : 28;
            for (uint32_t k = 0; k < take; k++) out[n+k] = (int16_t)s16be(d + 5 + 2*k);
            n += take;
            prev = s16be(d + 1); prev2 = s16be(d + 3);
            d += 61;
        } else {
            int ci = d[0] >> 4, shift = 20 - (d[0] & 15);
            for (int b = 1; b < 15 && n < nsamp; b++) {
                for (int hi = 1; hi >= 0 && n < nsamp; hi--) {
                    int nib = hi ? d[b] >> 4 : d[b] & 15;
                    if (nib > 7) nib -= 16;
                    int s = ((nib << shift) + prev*EAXA_C1[ci] + prev2*EAXA_C2[ci] + 0x80) >> 8;
                    if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                    prev2 = prev; prev = s;
                    out[n++] = (int16_t)s;
                }
            }
            d += 15;
        }
    }
}

/* fundamental of a loop by normalized autocorrelation (halving refinement
 * avoids locking onto a multiple of the true period) */
static float loop_base_freq(const int16_t *s, uint32_t n, float rate) {
    double mean = 0; for (uint32_t i = 0; i < n; i++) mean += s[i]; mean /= n;
    int best_lag = 0; double best_r = 0;
    for (int lag = 40; lag < 1500 && (uint32_t)lag < n/2; lag += 2) {
        double num = 0, den = 0;
        for (uint32_t k = 0; k + lag < n; k += 8) {
            double a = s[k] - mean, b = s[k+lag] - mean;
            num += a * b; den += a * a;
        }
        double r = den > 0 ? num / den : 0;
        if (r > best_r) { best_r = r; best_lag = lag; }
    }
    if (!best_lag) return 0;
    for (;;) {   /* prefer the fundamental over its multiples */
        int half = best_lag / 2;
        if (half < 40) break;
        double num = 0, den = 0;
        for (uint32_t k = 0; k + half < n; k += 8) {
            double a = s[k] - mean, b = s[k+half] - mean;
            num += a * b; den += a * a;
        }
        if (den <= 0 || num/den < 0.9 * best_r) break;
        best_lag = half;
    }
    return rate / (float)best_lag;
}

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

/* ---- Gnsu20 native sweep loading (SOUND/ENGINE/GIN_*.gin) ----
 * Header: "Gnsu20", f32 rpm_min/rpm_max @+0x08, u32 n1/n2/total/rate @+0x10.
 * Then (n1+1) u32 sample positions at evenly spaced RPMs (the rpm->position
 * curve; descending for decel recordings), (n2+1) u32 cycle-aligned grain
 * points, then EA-XAS v0 audio: 0x13-byte frames of 32 samples. Frame
 * header u32 LE packs coef index (bits 0-3), hist2 (bits 4-15, as
 * s16 & 0xFFF0), shift (16-19), hist1 (20-31, as s16 & 0xFFF0); both
 * history samples are OUTPUT, so every frame decodes independently.
 * Validated: local fundamental at each rpm marker == rpm/60 Hz.
 * (Frame layout confirmed against vgmstream's ea_xas_decoder.c, used as a
 * format reference like Nikki/OpenNFSTools — no code copied.) */

static void xas_decode(const unsigned char *d, int16_t *out, uint32_t nsamp) {
    uint32_t n = 0;
    while (n < nsamp) {
        uint32_t h = rd32(d);
        int ci = h & 15, shift = (h >> 16) & 15;
        float p2 = (int16_t)(h & 0xFFF0), p1 = (int16_t)((h >> 16) & 0xFFF0);
        out[n++] = (int16_t)p2;
        if (n < nsamp) out[n++] = (int16_t)p1;
        static const float XC1[4] = { 0.0f, 0.9375f, 1.796875f, 1.53125f };
        static const float XC2[4] = { 0.0f, 0.0f, -0.8125f, -0.859375f };
        for (int b = 4; b < 19 && n < nsamp; b++) {
            for (int hi = 1; hi >= 0 && n < nsamp; hi--) {
                int nib = hi ? d[b] >> 4 : d[b] & 15;
                if (nib > 7) nib -= 16;
                /* nib is signed (-8..7): left-shifting it is UB in C even
                   though every real target just sign-extends. Multiplying
                   by 2^12 first gives the identical bit pattern without the
                   UB; the right shift that follows stays on an already-
                   nonnegative-shifted value's implementation-defined (not
                   undefined) arithmetic shift, same as before. */
                float s = (float)((nib * 4096) >> shift) + p1*XC1[ci] + p2*XC2[ci];
                if (s > 32767.0f) s = 32767.0f; if (s < -32768.0f) s = -32768.0f;
                p2 = p1; p1 = s;
                out[n++] = (int16_t)s;
            }
        }
        d += 0x13;
    }
}

static void free_sweep(GinsuSweep *s) {
    free(s->samples); free(s->rpm_pos); free(s->grain);
    memset(s, 0, sizeof *s);
}

static int gin_load(const char *path, GinsuSweep *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)len);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return 0; }
    fclose(f);
    if (len < 0x20 || memcmp(d, "Gnsu20", 6) != 0) { free(d); return 0; }
    memcpy(&s->rpm_min, d + 8, 4); memcpy(&s->rpm_max, d + 12, 4);
    s->n_rpm = (int)rd32(d + 0x10); s->n_grain = (int)rd32(d + 0x14);
    s->total_samples = rd32(d + 0x18); s->sample_rate = rd32(d + 0x1c);
    long data0 = 0x20 + 4L*(s->n_rpm + 1) + 4L*(s->n_grain + 1);
    long nfr = (long)((s->total_samples + 31) / 32);
    if (s->n_rpm < 2 || s->n_grain < 2 || s->total_samples < 4096 ||
        data0 + nfr*0x13 > len) { free(d); return 0; }
    s->rpm_pos = (uint32_t *)malloc(4u*(s->n_rpm + 1));
    s->grain   = (uint32_t *)malloc(4u*(s->n_grain + 1));
    s->samples = (int16_t *)malloc((size_t)s->total_samples * 2);
    if (!s->rpm_pos || !s->grain || !s->samples) { free_sweep(s); free(d); return 0; }
    for (int i = 0; i <= s->n_rpm; i++)   s->rpm_pos[i] = rd32(d + 0x20 + 4*i);
    for (int i = 0; i <= s->n_grain; i++) s->grain[i]   = rd32(d + 0x20 + 4*(s->n_rpm+1) + 4*i);
    xas_decode(d + data0, s->samples, s->total_samples);
    free(d);
    return 1;
}

/* rpm at a sample position, from the sweep's marker curve (works for the
 * descending decel curves too) */
static float gin_rpm_at(const GinsuSweep *s, float pos) {
    float step = (s->rpm_max - s->rpm_min) / (float)s->n_rpm;
    for (int i = 0; i < s->n_rpm; i++) {
        float a = (float)s->rpm_pos[i], b = (float)s->rpm_pos[i+1];
        if ((pos >= a && pos <= b) || (pos >= b && pos <= a)) {
            float t = (b != a) ? (pos - a) / (b - a) : 0.0f;
            return s->rpm_min + ((float)i + t) * step;
        }
    }
    return pos <= (float)s->rpm_pos[0] ? s->rpm_min : s->rpm_max;
}

/* sample position for an rpm (clamped to the sweep's range) */
static float gin_pos_at(const GinsuSweep *s, float rpm) {
    float t = (rpm - s->rpm_min) / (s->rpm_max - s->rpm_min);
    if (t < 0) t = 0; if (t > 1) t = 1;
    float fi = t * (float)s->n_rpm;
    int i = (int)fi; if (i >= s->n_rpm) i = s->n_rpm - 1;
    return (float)s->rpm_pos[i] + (fi - (float)i) *
           ((float)s->rpm_pos[i+1] - (float)s->rpm_pos[i]);
}

int audio_load_ginsu_sweeps(const char *dataroot, const char *carname) {
    Ginsu20State *g = &g_engine.gin;
    if (g->has_gin_data) {
        free_sweep(&g->accel_sweep);
        if (!g->decel_is_alias) free_sweep(&g->decel_sweep);
        memset(g, 0, sizeof *g);
    }
    char dirp[512], up_car[64], up_f[256];
    snprintf(dirp, sizeof dirp, "%s/SOUND/ENGINE", dataroot);
    size_t cl = strlen(carname);
    if (cl >= sizeof up_car) return 0;
    for (size_t i = 0; i <= cl; i++) up_car[i] = (char)toupper((unsigned char)carname[i]);
    DIR *dir = opendir(dirp);
    if (!dir) return 0;
    char best[2][256] = { "", "" };            /* [0]=accel, [1]=decel */
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t fl = strlen(e->d_name);
        if (fl < 5 || fl >= sizeof up_f) continue;
        for (size_t i = 0; i <= fl; i++) up_f[i] = (char)toupper((unsigned char)e->d_name[i]);
        if (strncmp(up_f, "GIN_", 4) != 0 || strcmp(up_f + fl - 4, ".GIN") != 0) continue;
        if (!strstr(up_f, up_car)) continue;
        int dcl = strstr(up_f, "DCL") || strstr(up_f, "DECEL") ? 1 : 0;
        if (!best[dcl][0] || strlen(e->d_name) < strlen(best[dcl]))
            snprintf(best[dcl], sizeof best[dcl], "%s", e->d_name);
    }
    closedir(dir);
    if (!best[0][0]) return 0;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", dirp, best[0]);
    if (!gin_load(path, &g->accel_sweep)) return 0;
    int n = 1;
    if (best[1][0]) {
        snprintf(path, sizeof path, "%s/%s", dirp, best[1]);
        if (gin_load(path, &g->decel_sweep)) n = 2;
    }
    if (n == 1) { g->decel_sweep = g->accel_sweep; g->decel_is_alias = 1; }
    for (int a = 0; a < 2; a++) {   /* same indexing as the callback: 1=accel */
        GinsuSweep *sw = a ? &g->accel_sweep : &g->decel_sweep;
        g->play_cursor[a] = (float)sw->grain[0];
        g->grain_rpm[a] = gin_rpm_at(sw,
            0.5f*((float)sw->grain[0] + (float)sw->grain[1]));
        if (g->grain_rpm[a] < 1.0f) g->grain_rpm[a] = sw->rpm_min;
    }
    g->has_gin_data = 1;
    return n;
}

#define ENG_NBANKS 41   /* SOUND/ENGINE/CAR_00..CAR_40 in the retail data */

int audio_bank_for_car(const char *carname) {
    unsigned h = 5381;   /* djb2 of the car folder name -> stable per-car voice */
    for (const char *c = carname; *c; c++) h = h*33u + (unsigned char)*c;
    return (int)(h % ENG_NBANKS);
}

static void free_band(AudioSampleBand *sb) {
    free(sb->samples); memset(sb, 0, sizeof *sb);
}

int audio_load_engine_bank(const char *dataroot, int carbank) {
    for (int b = 0; b < 3; b++) { free_band(&g_engine.accel_bands[b]); free_band(&g_engine.decel_bands[b]); }
    free_band(&g_engine.idle_band);
    g_engine.sample_mode = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/SOUND/ENGINE/CAR_%02d_ENG_MB_SPU.abk", dataroot, carbank);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)len);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return 0; }
    fclose(f);
    if (len < 0x40 || memcmp(d, "ABKC", 4) != 0) { free(d); return 0; }
    long boff = rd32(d + 0x18), bsize = rd32(d + 0x24);
    if (boff + bsize > len || memcmp(d + boff, "BNKl", 4) != 0) { free(d); return 0; }
    /* collect the PT headers from the bank TOC */
    struct { uint32_t off, count, rate; } st[16]; int nst = 0;
    for (long p = boff; p < boff + 0x800 - 4 && nst < 16; p++) {
        if (memcmp(d + p, "PT\0\0", 4) != 0) continue;
        uint32_t off = 0, count = 0, rate = 22050;
        long q = p + 4;
        while (q < p + 0x30) {
            int tag = d[q++];
            if (tag == 0xFF || tag == 0xFE) break;
            if (tag == 0xFD) continue;
            int nlen = d[q++]; uint32_t v = 0;
            for (int k = 0; k < nlen; k++) v = v << 8 | d[q++];
            if (tag == 0x84) rate = v;
            else if (tag == 0x85) count = v;
            else if (tag == 0x88) off = v;
        }
        if (count && off) { st[nst].off = off; st[nst].count = count; st[nst].rate = rate; nst++; }
        p += 0x20;
    }
    /* Ginsu engine bank layout: [0]=idle, [1..3]=accel L/M/H, [4..6]=decel
       L/M/H, [7]=high/whine. ponytail: idle+whine unused — fold them in when
       the 3+3 blend feels thin at the extremes. */
    if (nst < 7) { free(d); return 0; }
    int loaded = 0;
    for (int b = 0; b < 7; b++) {
        /* stream 0 = idle, 1..3 = accel L/M/H, 4..6 = decel L/M/H */
        AudioSampleBand *sb = b == 0 ? &g_engine.idle_band
                            : b < 4 ? &g_engine.accel_bands[b-1]
                                    : &g_engine.decel_bands[b-4];
        uint32_t si = (uint32_t)b;
        if (boff + st[si].off >= len) continue;
        sb->samples = (int16_t *)malloc(st[si].count * sizeof(int16_t));
        if (!sb->samples) continue;
        eaxa_decode(d + boff + st[si].off, sb->samples, st[si].count);
        sb->sample_count = st[si].count;
        sb->srate = (float)st[si].rate;
        sb->base_freq = loop_base_freq(sb->samples, sb->sample_count, sb->srate);
        sb->playback_index = 0; sb->volume = 0;
        loaded++;
    }
    free(d);
    g_engine.sample_mode = (loaded == 7);
    return g_engine.sample_mode ? loaded : 0;
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static float hit = 0.0f, lp = 0.0f;
    int16_t *out = (int16_t *)stream;
    int n = len / 2;                 /* mono S16 */
    static float hp = 0.0f; static double sq = 0.0;
    EngineSynthState *e = &g_engine;
    float target = e->target_rpm, load = e->load, master = e->master_volume;
    float road = g_road_vol, skid = g_skid;
    if (g_hit > hit) hit = g_hit; g_hit = 0.0f;   /* latch a new collision */
    for (int i = 0; i < n; i++) {
        /* engine inertia: revs chase the target (~110ms), load a bit faster */
        e->current_rpm  += (target - e->current_rpm)  * 0.0002f;
        e->current_load += (load   - e->current_load) * 0.0005f;
        float w[3]; eng_band_weights(e->current_rpm, w);
        float eng = 0.0f;
        if (e->gin.has_gin_data) {
            /* native Gnsu20 path: loop the cycle-aligned grain matching the
               current rpm, pitch-corrected by the grain's recorded rpm.
               When rpm moves >6% off the playing grain, a secondary cursor
               sprouts at the target grain and crossfades in over ~12ms
               (dual-grain interpolation — no staircase on fast sweeps).
               Accel/decel sweeps crossfade by load. */
            Ginsu20State *g = &e->gin;
            for (int a = 0; a < 2; a++) {
                GinsuSweep *sw = a ? &g->accel_sweep : &g->decel_sweep;
                float rate = (float)sw->sample_rate / 44100.0f;
                int j = g->grain_idx[a];
                if (g->play_cursor[a] >= (float)sw->grain[j+1])   /* loop own grain */
                    g->play_cursor[a] -= (float)(sw->grain[j+1] - sw->grain[j]);
                if (g->play_cursor[a] < (float)sw->grain[j])
                    g->play_cursor[a] = (float)sw->grain[j];
                if (!g->xf_on[a] &&
                    fabsf(e->current_rpm - g->grain_rpm[a]) > 0.06f * g->grain_rpm[a]) {
                    float pos = gin_pos_at(sw, e->current_rpm);
                    int lo = 0, hi = sw->n_grain - 1;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (pos >= (float)sw->grain[mid+1]) lo = mid + 1; else hi = mid;
                    }
                    if (lo != j) {
                        g->xf_on[a] = 1; g->xf_grain[a] = lo; g->xf_t[a] = 0.0f;
                        g->xf_cursor[a] = (float)sw->grain[lo];
                        g->xf_rpm[a] = gin_rpm_at(sw,
                            0.5f*((float)sw->grain[lo] + (float)sw->grain[lo+1]));
                        if (g->xf_rpm[a] < 1.0f) g->xf_rpm[a] = sw->rpm_min;
                    }
                }
                uint32_t i0 = (uint32_t)g->play_cursor[a];
                float fr = g->play_cursor[a] - (float)i0;
                float s = (sw->samples[i0]*(1.0f-fr) + sw->samples[i0+1]*fr) / 32768.0f;
                g->play_cursor[a] += (e->current_rpm / g->grain_rpm[a]) * rate;
                if (g->xf_on[a]) {
                    int k = g->xf_grain[a];
                    if (g->xf_cursor[a] >= (float)sw->grain[k+1])
                        g->xf_cursor[a] -= (float)(sw->grain[k+1] - sw->grain[k]);
                    uint32_t i1 = (uint32_t)g->xf_cursor[a];
                    float f1 = g->xf_cursor[a] - (float)i1;
                    float s2 = (sw->samples[i1]*(1.0f-f1) + sw->samples[i1+1]*f1) / 32768.0f;
                    float t = g->xf_t[a];
                    s = s*(1.0f-t) + s2*t;
                    g->xf_cursor[a] += (e->current_rpm / g->xf_rpm[a]) * rate;
                    g->xf_t[a] += 1.0f / (0.012f * 44100.0f);
                    if (g->xf_t[a] >= 1.0f) {   /* promote secondary to primary */
                        g->grain_idx[a] = k; g->play_cursor[a] = g->xf_cursor[a];
                        g->grain_rpm[a] = g->xf_rpm[a]; g->xf_on[a] = 0;
                    }
                }
                eng += s * (a ? e->current_load : 1.0f - e->current_load) * master;
            }
            eng *= 0.55f;   /* sweeps are recorded near full scale */
        } else if (e->sample_mode) {
            /* Ginsu path: idle loop below ~2200rpm, crossfading into the 3
               RPM-band loop pairs; accel vs decel crossfaded by load */
            float wi = (2200.0f - e->current_rpm) / 1200.0f;   /* 1 at idle, 0 by 2200 */
            if (wi < 0) wi = 0; if (wi > 1) wi = 1;
            AudioSampleBand *ib = &e->idle_band;
            ib->playback_index += (e->current_rpm / ENG_IDLE_RPM) * (ib->srate / 44100.0f);
            while (ib->playback_index >= (float)ib->sample_count)
                ib->playback_index -= (float)ib->sample_count;
            ib->volume = wi * master;
            if (ib->volume > 1e-4f) {
                uint32_t i0 = (uint32_t)ib->playback_index;
                uint32_t i1 = i0 + 1 < ib->sample_count ? i0 + 1 : 0;
                float fr = ib->playback_index - (float)i0;
                eng += (ib->samples[i0]*(1.0f-fr) + ib->samples[i1]*fr) / 32768.0f * ib->volume;
            }
            for (int b = 0; b < 3; b++) {
                float ratio = e->current_rpm / ENG_CENTER[b];
                for (int a = 0; a < 2; a++) {
                    AudioSampleBand *sb = a ? &e->accel_bands[b] : &e->decel_bands[b];
                    sb->playback_index += ratio * (sb->srate / 44100.0f);
                    while (sb->playback_index >= (float)sb->sample_count)
                        sb->playback_index -= (float)sb->sample_count;
                    sb->volume = w[b] * (1.0f - wi)
                               * (a ? e->current_load : 1.0f - e->current_load) * master;
                    if (sb->volume < 1e-4f) continue;
                    uint32_t i0 = (uint32_t)sb->playback_index;
                    uint32_t i1 = i0 + 1 < sb->sample_count ? i0 + 1 : 0;
                    float fr = sb->playback_index - (float)i0;
                    float s = (sb->samples[i0]*(1.0f-fr) + sb->samples[i1]*fr) / 32768.0f;
                    eng += s * sb->volume;
                }
            }
            eng *= 1.6f;   /* the game loops are quieter than full scale */
        } else {
            float fund = ENG_FIRE_HZ(e->current_rpm);
            for (int b = 0; b < 3; b++) {
                AudioBand *bd = &e->bands[b];
                /* pitch = base_freq * (rpm / center) == the shared fundamental */
                bd->phase += fund / 44100.0f; if (bd->phase >= 1.0f) bd->phase -= 1.0f;
                bd->volume = w[b] * (0.30f + 0.70f*e->current_load) * master;
                if (bd->volume < 1e-4f) continue;
                float s = 0.0f;
                for (int h = 0; h < 4; h++)
                    s += ENG_HARM[b][h].amp *
                         sinf(6.2831853f * ENG_HARM[b][h].mult * bd->phase);
                eng += s * bd->volume;
            }
            eng *= 0.6f;   /* mix gain: matches the old synth's loudness */
        }

        /* --- auxiliary acoustics, layered over any engine mode --- */
        {
            AuxAudio *x = &g_aux;
            float rf = e->current_rpm / ENG_REDLINE_RPM;
            float anz = frand();
            /* turbo whine: pitch and volume follow load * rpm */
            float tv = e->current_load * rf; if (tv > 1.0f) tv = 1.0f;
            x->turbo_phase += (1200.0f * (1.0f + 1.5f*rf)) / 44100.0f;
            if (x->turbo_phase >= 1.0f) x->turbo_phase -= 1.0f;
            eng += (sinf(6.2831853f*x->turbo_phase)*0.8f + anz*0.2f)
                 * tv * 0.045f * master;
            /* blow-off valve: boost vented on a sharp lift-off (load falls
               from >0.7 to <0.15 within the ~150ms hiload memory) */
            x->hiload -= 4.5f / 44100.0f;
            if (e->current_load > x->hiload) x->hiload = e->current_load;
            if (x->hiload > 0.7f && e->current_load < 0.15f && rf > 0.3f &&
                x->bov_env < 0.01f) { x->bov_env = 0.9f; x->hiload = 0.0f; }
            if (x->bov_env > 0.002f) {
                x->bov_hp += (anz - x->bov_hp) * 0.45f;   /* hissy top end */
                eng += (anz - x->bov_hp) * x->bov_env * 0.5f * master;
                x->bov_env *= 0.99975f;                   /* ~250ms tail */
            }
            /* overrun pops: high rpm + closed throttle, irregular intervals */
            if (e->current_rpm > 4000.0f && e->current_load < 0.05f) {
                x->pop_timer -= 1.0f / 44100.0f;
                if (x->pop_timer <= 0.0f) {
                    g_rng = g_rng*1664525u + 1013904223u;
                    x->pop_timer = 0.10f + (float)(g_rng >> 20 & 1023) / 1023.0f * 0.20f;
                    x->pop_env = 0.5f + (float)(g_rng >> 8 & 255) / 255.0f * 0.5f;
                }
            } else x->pop_timer = 0.12f;
            if (x->pop_env > 0.003f) {
                x->pop_lp += (anz - x->pop_lp) * 0.10f;   /* deep bang */
                eng += x->pop_lp * x->pop_env * 2.2f * master;
                x->pop_env *= 0.9992f;                    /* ~35ms transient */
            }
        }
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

/* asserts the crossfade math: pure bands at the centers, 50/50 at the
 * midpoints, and the weights always partition to 1. */
void audio_selftest(void) {
    float w[3];
    eng_band_weights(1000.0f, w); assert(w[0]==1.0f && w[1]==0.0f && w[2]==0.0f);
    eng_band_weights(1500.0f, w); assert(w[0]==1.0f);
    eng_band_weights(2750.0f, w); assert(fabsf(w[0]-0.5f)<1e-4f && fabsf(w[1]-0.5f)<1e-4f);
    eng_band_weights(4000.0f, w); assert(w[1]==1.0f);
    eng_band_weights(5500.0f, w); assert(fabsf(w[1]-0.5f)<1e-4f && fabsf(w[2]-0.5f)<1e-4f);
    eng_band_weights(8000.0f, w); assert(w[2]==1.0f && w[0]==0.0f);
    for (float r = 800.0f; r <= 8200.0f; r += 37.0f) {
        eng_band_weights(r, w);
        assert(fabsf(w[0]+w[1]+w[2]-1.0f) < 1e-4f);
    }
    /* gearbox: a 20s 0->top ramp must climb 1..6 with rpm in [idle, redline]
     * and no flapping; the way back down must land in 1st. */
    int gear = 1, maxg = 1; float st = 0.0f;
    for (int i = 0; i <= 1200; i++) {
        eng_gearbox_step(i/1200.0f, 1.0f, 1.0f/60.0f, &gear, &st);
        assert(gear >= 1 && gear <= ENG_GEARS);
        assert(g_engine.target_rpm >= ENG_IDLE_RPM - 1.0f &&
               g_engine.target_rpm <= ENG_REDLINE_RPM + 1.0f);
        if (gear > maxg) maxg = gear;
    }
    assert(maxg == ENG_GEARS);
    for (int i = 1200; i >= 0; i--)
        eng_gearbox_step(i/1200.0f, 0.0f, 1.0f/60.0f, &gear, &st);
    assert(gear == 1);
    g_engine.target_rpm = ENG_IDLE_RPM; g_engine.load = 0.0f;   /* reset */
}

SDL_AudioDeviceID audio_init(void) {
    SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
    return adev;
}
