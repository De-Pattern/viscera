/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: the audio engine. Everything here runs in the audio interrupt on
 * the FM-1, so it is either VSC_HOT (placed in RAM) or always inlined, uses
 * no C library and only single-precision float. */
#include "vsc.h"

struct vsc_project vsc;
struct vsc_meter vsc_meter;
uint8_t vsc_lock[VSC_TRACKS][VSC_NP];
uint8_t vsc_scope[128];

/* ---------------------------------------------------------------- maths */
VSC_INL float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
VSC_INL float absf(float x) { return x < 0.f ? -x : x; }
VSC_INL int floori(float x) { int i = (int)x; return (float)i > x ? i - 1 : i; }
/* 2^x, about 1e-4 relative error over the audio range. */
VSC_INL float exp2a(float x)
{
    x = clampf(x, -60.f, 30.f);
    int i = floori(x);
    float f = x - (float)i;
    float p = 1.f + f * (0.6951786f + f * (0.2261598f + f * 0.0782371f));
    union { float f; uint32_t u; } v;
    v.f = p;
    v.u += (uint32_t)i << 23;
    return v.f;
}
VSC_INL float tanha(float x)
{
    x = clampf(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
/* sin(2*pi*p) for p in [0,1). */
VSC_INL float sin01(float p)
{
    float x = p < 0.5f ? p * 4.f - 1.f : 3.f - p * 4.f; /* triangle -1..1 */
    return x * (1.5f - 0.5f * x * x);
}
/* cos(2 pi p) for p in 0..0.5, accurate enough to tune a resonator */
VSC_INL float cos2pi(float p)
{
    float x = p * 6.28318531f, flip = 1.f;
    if (x > 1.57079633f) { x = 3.14159265f - x; flip = -1.f; }
    float x2 = x * x;
    return flip * (1.f + x2 * (-0.5f + x2 * (1.f / 24.f + x2 * (-1.f / 720.f + x2 * (1.f / 40320.f)))));
}
VSC_INL float tanpre(float x) { float x2 = x * x; return x * (15.f - x2) / (15.f - 6.f * x2); }
/* log2(x) for x > 0, about 2e-4 absolute error */
VSC_INL float log2a(float x)
{
    union { float f; uint32_t u; } v; v.f = x;
    float e = (float)(int)((v.u >> 23) & 255u) - 127.f;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;   /* mantissa in [1,2) */
    float m = v.f;
    return e + ((((-0.07915816f * m + 0.62887362f) * m - 2.08121416f) * m + 4.02854794f) * m - 2.49684606f);
}
VSC_INL float sqrta(float x)
{
    if (x <= 0.f) return 0.f;
    union { float f; uint32_t u; } v; v.f = x;
    v.u = 0x1fbd1df5u + (v.u >> 1);
    float y = v.f;
    y = 0.5f * (y + x / y); y = 0.5f * (y + x / y);
    return y;
}

static uint32_t rng = 0x9e3779b9u;
VSC_INL uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
VSC_INL float rndu(void) { return (float)(rnd() >> 8) * (1.f / 16777216.f); }
VSC_INL float rndb(void) { return rndu() * 2.f - 1.f; }

/* mu-law, 8-bit. Decode table is built at init into RAM. */
static float ulaw_table[256];
VSC_INL uint8_t ulaw_enc(float x)
{
    int s = (int)(clampf(x, -1.f, 1.f) * 32767.f);
    int sign = 0;
    if (s < 0) { s = -s; sign = 0x80; }
    if (s > 32635) s = 32635;
    s += 0x84;
    int e = 7;
    for (int m = 0x4000; !(s & m) && e > 0; --e, m >>= 1) {}
    int mant = (s >> (e + 3)) & 15;
    return (uint8_t)~(sign | (e << 4) | mant);
}
float vsc_ulaw(uint8_t v) { return ulaw_table[v]; }
static void ulaw_build(void)
{
    for (int i = 0; i < 256; ++i) {
        int u = ~i & 0xff;
        int sign = u & 0x80, e = (u >> 4) & 7, mant = u & 15;
        int s = (((mant << 3) + 0x84) << e) - 0x84;
        ulaw_table[i] = (float)(sign ? -s : s) * (1.f / 32768.f);
    }
}

/* -------------------------------------------------------------- buffers */
static uint8_t is_enum[VSC_NP];   /* built at init: the parameter table stays in flash */
#define balls_allowed(t) ((unsigned)vsc.t[(t)][P_RCOUNT] % VSC_BALLS + 1u)
static uint8_t loop_pool[VSC_LOOP_SLICES * VSC_LOOP_SLICE];
static uint8_t grain_ring[VSC_TRACKS][VSC_GRAIN_FRAMES];
static int16_t delay_line[VSC_DELAY_FRAMES];
enum { RV_C0 = 557, RV_C1 = 593, RV_C2 = 641, RV_C3 = 677, RV_A0 = 277, RV_A1 = 221, RV_A2 = 289, RV_A3 = 233 };
static int16_t rv_buf[RV_C0 + RV_C1 + RV_C2 + RV_C3 + RV_A0 + RV_A1 + RV_A2 + RV_A3];

/* --------------------------------------------------------------- events */
#define QN 128
struct queue { volatile uint32_t w, r; uint32_t d[QN]; };
static struct queue q_ctrl, q_midi;
static void push(struct queue *q, unsigned type, unsigned t, unsigned a, unsigned b)
{
    uint32_t w = q->w;
    if (w - q->r >= QN) return;
    q->d[w & (QN - 1)] = (type & 15u) | ((t & 15u) << 4) | ((a & 255u) << 8) | ((b & 255u) << 16);
    __asm__ volatile("" ::: "memory");
    q->w = w + 1;
}
void vsc_event(unsigned type, unsigned t, unsigned a, unsigned b) { push(&q_ctrl, type, t, a, b); }
void vsc_midi_event(unsigned type, unsigned t, unsigned a, unsigned b) { push(&q_midi, type, t, a, b); }

/* ---------------------------------------------------------------- state */
struct reader { int slot; int chunk; const int16_t *p; };
struct voice {
    uint32_t ipos; float fpos, inc, target_inc, env, vel;
    int8_t dir; uint8_t note, stage, on;
};
struct grain { float pos, inc; uint32_t len, t; float amp; uint8_t active; };
#define VSC_GEVENTS 8           /* hits remembered per track (GRAIN SCAN picks among them) */
struct track {
    /* material */
    uint32_t ipos; float fpos; int8_t dir; uint8_t running;
    float declick;
    uint8_t loop_state; uint32_t loop_len, rec_pos; float rec_phase;
    struct voice v[VSC_KEYS_VOICES];
    struct reader rd;
    float last_inc;
    int pitch_note;
    /* grain */
    struct grain g[VSC_GRAINS];
    uint32_t gw; float gphase, gfb;
    /* filter */
    float s1[4], s2[4];
    /* RING: band state, input history, the waves phase, noise per band, tone */
    float ky1[VSC_RING_BANDS], ky2[VSC_RING_BANDS], kx1, kx2;
    float kwph, knph, knz[VSC_RING_BANDS], knt[VSC_RING_BANDS], ktone, kgain[VSC_RING_BANDS];
    /* the bands' tuning, kept until PITCH, SCALE, DETUNE or DECAY moves (krk: what it was made from) */
    float ka1[VSC_RING_BANDS], ku[VSC_RING_BANDS], krk[3]; uint16_t kmask; uint8_t kok;
    float side[VSC_SUB];        /* this sub-block's stereo difference (RESON spreads its bands) */
    /* color */
    float comp_env, hold, hold_phase, tone_lp, dust;
    /* mods: four slots of any type */
    struct modst {
        float ph, val, env, sp, from, held, hold_ph;
        uint8_t steps[16], dl[24];   /* RANDOM's steps, FOLLOW's delay line: 0..255 */
        uint8_t idx, stage, dlw, pad;
    } ms[VSC_MODS];
    float follow;               /* last FOLLOW-style level, for COLOR's noise */
    float mat_pk;               /* this sub-block's material peak, for FOLLOW */
    float fol_lp;               /* FOLLOW's input filter */
    uint8_t env_stage, gates;
    /* MIX's filter, COLOR's noise and redux */
    float djs1, djs2, nz_env, nz_p0, nz_p1, nz_p2, nz_s1, nz_s2;
    /* step gate */
    int32_t gate_left; uint8_t gate_note;
    /* computed parameters, 0..1 for continuous ones */
    float e[VSC_NP];
    float out[VSC_BLOCK];       /* this block's mono output (post color) */
    float prev[VSC_BLOCK];      /* last block's, for resampling */
    float in[VSC_BLOCK];
    float pan_l, pan_r, send;
    uint16_t awake;             /* sub-blocks left before a silent track sleeps (see vsc_render) */
    /* DISSECT: where the listener's hits fell in this track's grain ring (frames written,
     * and the pitch heard then), a burst of grains under way, the note that asked for it */
    struct gev { uint32_t at; float note; } ev[VSC_GEVENTS];
    uint32_t gtotal;            /* frames written to the grain ring, ever */
    uint8_t ev_w, ev_n, gburst, gnote_hit;
    int32_t gwait; float gnote;
    float kburst;               /* RESON EXCITE: the hit's burst, decaying */
    float kfol;                 /* RESON FOLLOW: the last note it followed (0: none yet) */
    uint8_t kmode;              /* which device the FILTER state belongs to (RESON or REBUILD) */
};
static struct track tr[VSC_TRACKS];
/* The platform's clock for the CPU readout (unset: no readout), and the guard's relief. */
uint32_t (*vsc_clock_now)(void);
uint32_t (*vsc_clock_since)(uint32_t since);
volatile uint8_t vsc_relief;
/* A track with nothing to play sleeps: only its material (and modulators) run, so a
 * sample, a loop, the input or a note wakes it at once. Awake, it stays so until its
 * output has been silent for about 1.5 s, so tails (RESON, grains, COLOR) finish. */
#define VSC_WAKE_LEVEL 0.0001f      /* -80 dBFS */
#define VSC_WAKE_HOLD  2048u        /* sub-blocks: 2048 x 32 frames = 1.49 s */
/* globals after modulation, in parameter units (0..max), refreshed every sub-block */
static float gv[VSC_NG];
static float main_prev[VSC_BLOCK];
static float space_l[VSC_BLOCK], space_r[VSC_BLOCK];
static uint32_t dl_w; static float dl_lp; static uint8_t dl_half;
static uint32_t rv_idx[8]; static float rv_lp[4];
static float main_gain_s = 0.f;

/* Sequencer. */
static float seq_phase;           /* frames into the current step */
static uint32_t seq_step;         /* global 16th counter */
static uint8_t seq_running, seq_clock_count, seq_ext_pending;
static uint32_t ext_last;         /* frames since the last MIDI clock */
static uint32_t ext_prev_at;      /* frame of the previous clock */
static float ext_interval;        /* smoothed frames per MIDI clock */
static float tempo_now = 110.f;   /* the tempo everything follows this block */
static uint8_t from_midi;         /* the event being handled came from MIDI */
static uint8_t mod_enum[4][VSC_MOD_PARAMS];
static uint8_t vsc_tparam_max[VSC_NP], vsc_gparam_max[VSC_NG];   /* RAM copies for the audio context */
static void handle_panic(void);
static float main_comp_env;
static VSC_HOT void metro_tick(void);
volatile uint8_t vsc_learn_armed, vsc_learn_track, vsc_learn_param, vsc_learn_seen;
static uint8_t space_frozen;
static uint32_t frame_counter;

/* -------------------------------------------------------------- sources */
VSC_INL uint32_t loop_capacity(unsigned t) { return (uint32_t)(vsc.t[t][P_SIZE] + 1u) * VSC_LOOP_SLICE; }
VSC_INL uint32_t loop_base(unsigned t) { return (uint32_t)t * VSC_LOOP_SLICE; }
VSC_INL float loop_at(unsigned t, uint32_t i)
{
    return ulaw_table[loop_pool[(loop_base(t) + i) % (VSC_LOOP_SLICES * VSC_LOOP_SLICE)]];
}
VSC_INL void loop_put(unsigned t, uint32_t i, float x)
{
    loop_pool[(loop_base(t) + i) % (VSC_LOOP_SLICES * VSC_LOOP_SLICE)] = ulaw_enc(x);
}
VSC_INL float bank_at(struct reader *r, int slot, uint32_t frame)
{
    int c = (int)(frame / VSC_CHUNK);
    if (c != r->chunk || slot != r->slot) {
        r->p = vsc_bank_chunk((unsigned)slot, (unsigned)c);
        r->chunk = c; r->slot = slot;
    }
    return r->p ? (float)r->p[frame - (uint32_t)c * VSC_CHUNK] * (1.f / 32768.f) : 0.f;
}
/* Source of a track: a bank slot, or its loop (slot 16 or LOOP mode). */
VSC_INL int uses_loop(unsigned t)
{
    unsigned m = vsc.t[t][P_MODE];
    return m == MODE_LOOP || (m == MODE_KEYS && vsc.t[t][P_SLOT] >= VSC_BANK_SLOTS);
}
VSC_INL uint32_t src_len(unsigned t)
{
    if (uses_loop(t)) return tr[t].loop_state >= 2 ? tr[t].loop_len : 0;
    unsigned s = vsc.t[t][P_SLOT];
    return s < VSC_BANK_SLOTS ? vsc_bank_frames(s) : 0;
}
VSC_INL float src_rate(unsigned t)
{
    if (uses_loop(t)) return vsc.t[t][P_QUAL] ? 22050.f : 44100.f;
    return (float)VSC_BANK_RATE;
}

/* ------------------------------------------------------------- triggers */
static void note_on(unsigned t, unsigned note, unsigned vel);
static void note_off(unsigned t, unsigned note);

static VSC_HOT void window(unsigned t, uint32_t len, uint32_t *a, uint32_t *b)
{
    float s = tr[t].e[P_START], l = tr[t].e[P_LEN];
    uint32_t st = (uint32_t)(s * (float)len);
    uint32_t n = (uint32_t)(l * (float)(len - st));
    if (n < 32) n = 32;
    if (st + n > len) { n = len > st ? len - st : 0; }
    *a = st; *b = st + n;
}
static VSC_HOT void note_on(unsigned t, unsigned note, unsigned vel)
{
    if (t < VSC_TRACKS) tr[t].awake = VSC_WAKE_HOLD;
    struct track *k = &tr[t];
    unsigned mode = vsc.t[t][P_MODE];
    if (vsc.t[t][P_GTRIG] == GTRIG_NOTES) { k->gnote = (float)note; k->gnote_hit = 1; }   /* GRAIN: notes start grains */
    k->gates++;
    k->env_stage = 1;
    for (unsigned m = 0; m < VSC_MODS; ++m)
        if ((vsc.t[t][P_MOD(m, 0)] & 3) == MT_ADSR) { k->ms[m].stage = 1; k->ms[m].sp = 0.f; k->ms[m].from = k->ms[m].env; }
    uint32_t len = src_len(t), a, b;
    if (mode == MODE_PLAY) {
        if (!len) return;
        window(t, len, &a, &b);
        k->dir = vsc.t[t][P_SPEED] >= 64 ? 1 : -1;
        k->ipos = k->dir > 0 ? a : (b ? b - 1 : 0);
        k->fpos = 0.f; k->running = 1; k->declick = 0.f;
        k->pitch_note = (int)note - 60;
    } else if (mode == MODE_LOOP) {
        if (k->loop_state < 2 || !len) return;
        window(t, len, &a, &b);
        unsigned slice = (note + 128u - 53u) & 15u;
        k->ipos = a + (((b - a) * slice) >> 4);   /* 32-bit: (b - a) * 15 stays far below 2^32 */
        k->fpos = 0.f; k->declick = 0.f; k->running = 1;
    } else if (mode == MODE_KEYS) {
        if (!len) return;
        window(t, len, &a, &b);
        /* steal the quietest or a released voice */
        unsigned pick = 0; float best = 9.f;
        for (unsigned i = 0; i < VSC_KEYS_VOICES; ++i) {
            float score = k->v[i].on ? k->v[i].env + (k->v[i].stage < 3 ? 1.f : 0.f) : -1.f;
            if (score < best) { best = score; pick = i; }
        }
        struct voice *v = &k->v[pick];
        float ratio = exp2a(((float)note - 60.f + (float)vsc.t[t][P_PITCH] - 24.f) * (1.f / 12.f));
        float inc = ratio * src_rate(t) * (1.f / (float)VSC_SR);
        float glide = k->e[P_GLIDE];
        v->target_inc = inc;
        v->inc = (glide > 0.01f && k->last_inc > 0.f) ? k->last_inc : inc;
        k->last_inc = inc;
        v->dir = 1; v->ipos = a; v->fpos = 0.f;
        v->note = (uint8_t)note; v->stage = 0; v->on = 1; v->env = 0.f;
        float sens = k->e[P_VELO];
        v->vel = (1.f - sens) + sens * (float)vel * (1.f / 127.f);
    }
}
static VSC_HOT void note_off(unsigned t, unsigned note)
{
    struct track *k = &tr[t];
    if (k->gates) k->gates--;
    if (!k->gates) {
        k->env_stage = 3;
        for (unsigned m = 0; m < VSC_MODS; ++m)
            if ((vsc.t[t][P_MOD(m, 0)] & 3) == MT_ADSR && k->ms[m].stage && k->ms[m].stage < 4) {
                k->ms[m].stage = 4; k->ms[m].sp = 0.f; k->ms[m].from = k->ms[m].env;
            }
    }
    if (vsc.t[t][P_MODE] == MODE_KEYS)
        for (unsigned i = 0; i < VSC_KEYS_VOICES; ++i)
            if (k->v[i].on && k->v[i].note == note && k->v[i].stage < 3) k->v[i].stage = 3;
}
static VSC_HOT void loop_rec(unsigned t)
{
    struct track *k = &tr[t];
    if (vsc.t[t][P_MODE] != MODE_LOOP) return;
    switch (k->loop_state) {
    case 0: k->loop_state = 1; k->rec_pos = 0; k->rec_phase = 0.f; break;
    case 1:
        k->loop_len = k->rec_pos < 256 ? 256 : k->rec_pos;
        k->loop_state = 2; k->ipos = 0; k->fpos = 0.f; k->dir = 1; k->running = 1; break;
    case 2: k->loop_state = 3; break;
    default: k->loop_state = 2; break;
    }
}

/* ------------------------------------------------------------ parameters */
static VSC_HOT void compute_globals(void)
{
    for (unsigned g = 0; g < VSC_NG; ++g) gv[g] = (float)vsc.g[g];
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
            const struct vsc_route *ro = &vsc.route[t][r];
            if (ro->param < VSC_GDEST || ro->param >= VSC_GDEST + VSC_NG || !ro->depth) continue;
            unsigned g = ro->param - VSC_GDEST;
            float mv = vsc_meter.mod_value[vsc_src_track(t, ro->src)][ro->src & 3];
            if (ro->flags & 1) mv = mv * 2.f - 1.f;
            gv[g] += (float)ro->depth * mv;
        }
    for (unsigned g = 0; g < VSC_NG; ++g) gv[g] = clampf(gv[g], 0.f, (float)vsc_gparam_max[g]);   /* the RAM copy */
}
static VSC_HOT void compute_params(unsigned t)
{
    /* Every value from the project (or a lock, or the scene morph) is already in
     * 0..1 (no plain parameter goes above 127), so only what a route or a macro
     * moved needs clamping: the same result as clamping everything, for less work. */
    struct track *k = &tr[t];
    const uint8_t *base = vsc.t[t];
    const struct vsc_scene *sc = &vsc.scene[vsc.g[G_TARGET] % VSC_SCENES];
    float morph = sc->used ? gv[G_MORPH] * (1.f / 127.f) : 0.f;
    uint8_t en[VSC_NP];
    for (unsigned p = 0; p < P_MOD0; ++p) en[p] = is_enum[p];
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        const uint8_t *me = mod_enum[base[P_MOD(m, 0)] & 3];
        for (unsigned kk = 0; kk < VSC_MOD_PARAMS; ++kk) en[P_MOD(m, kk)] = me[kk];
    }
    for (unsigned p = P_MOD0 + 4 * VSC_MOD_PARAMS; p < VSC_NP; ++p) en[p] = is_enum[p];
    const uint8_t *lock = vsc_lock[t];
    for (unsigned p = 0; p < VSC_NP; ++p) {
        unsigned v = lock[p] != 255 ? lock[p] : base[p];
        if (en[p]) { k->e[p] = (float)v; continue; }
        float x = (float)v * (1.f / 127.f);
        if (morph > 0.f) x += ((float)sc->t[t][p] * (1.f / 127.f) - x) * morph;
        k->e[p] = x;
    }
    int moved = 0;
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
        const struct vsc_route *ro = &vsc.route[t][r];
        if (ro->param >= VSC_NP || !ro->depth) continue;   /* globals: compute_globals */
        float mv = vsc_meter.mod_value[vsc_src_track(t, ro->src)][ro->src & 3];
        if (ro->flags & 1) mv = mv * 2.f - 1.f;
        k->e[ro->param] += (float)ro->depth * (1.f / 127.f) * mv;
        moved = 1;
    }
    for (unsigned m = 0; m < VSC_MACROS; ++m) {
        float mv = gv[G_M0 + m] * (1.f / 127.f);
        if (mv <= 0.f) continue;
        for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r) {
            const struct vsc_mroute *ro = &vsc.macro[m][r];
            if (ro->track == t && ro->param < VSC_NP && ro->depth) { k->e[ro->param] += (float)ro->depth * (1.f / 127.f) * mv; moved = 1; }
        }
    }
    if (!moved) return;
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
        unsigned p = vsc.route[t][r].param;
        if (p < VSC_NP && vsc.route[t][r].depth && !en[p]) k->e[p] = clampf(k->e[p], 0.f, 1.f);
    }
    for (unsigned m = 0; m < VSC_MACROS; ++m)
        for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r) {
            const struct vsc_mroute *ro = &vsc.macro[m][r];
            if (ro->track == t && ro->param < VSC_NP && ro->depth && !en[ro->param]) k->e[ro->param] = clampf(k->e[ro->param], 0.f, 1.f);
        }
}

/* ------------------------------------------------------------- DISSECT
 * One listener for the whole engine, run once a block on DISSECT's SOURCE, and only
 * while something uses it (a track's TRIG, EXCITE, FOLLOW or TUNE, REBUILD, a FOLLOW
 * modulator on one of its inputs, HITS, or the page on screen). It hears:
 *  - three bands (below ~150 Hz, to ~2.5 kHz, above), per 32-frame sub-block;
 *  - hits: a rise in the FOCUS band against its recent level, over a threshold that
 *    SENS sets and the recent rises raise, at most one per 36 ms;
 *  - pitch: YIN on an 11 kHz, 8-bit history (lags 50 Hz .. 1 kHz), spread over 15
 *    blocks and stopped at the first clear dip, then the levels of its first eight
 *    partials (Goertzel), every 16 blocks (46 ms).
 * Ravel's rules: nothing runs when nothing listens or the source is silent, the pitch
 * search costs a few hundred multiplies a block (integer), and no new long buffers. */
#define ANA_SUBS   (VSC_BLOCK / VSC_SUB)
#define ANA_RING   1024u      /* 8-bit history at 11025 Hz: 93 ms */
#define ANA_W      256u       /* the window YIN compares */
#define ANA_TMIN   11u        /* lags: 1 kHz .. */
#define ANA_TMAX   220u       /* .. 50 Hz */
#define ANA_PERIOD 16u        /* blocks per reading */
#define ANA_LAGS   14u        /* lags a block: 15 blocks cover them all */
#define ANA_PART   8u
static struct {
    float lp1, lp2, lp3, prev[3], slow, odf_avg, agc, agc_used;
    float hit[ANA_SUBS], band[ANA_SUBS][3];
    float note;               /* the last pitch heard clearly, as a MIDI note (0: none yet) */
    float cum, best, b0, b1, b2, p1, p2;
    uint32_t dw, ws;
    unsigned dphase, blk, tau, best_tau, refr;
    uint8_t found, voiced;
    int8_t ring[ANA_RING];
} ana;
volatile uint8_t vsc_ana_view;
/* the listener's pitch now, as a note (0 when none is heard) */
VSC_INL float ana_note_now(void) { return ana.voiced ? ana.note : 0.f; }
static VSC_HOT_SMALL void ana_pitch_end(void)
{
    /* the dip YIN found (or the deepest), refined between its neighbours */
    float conf = 0.f, hz = 0.f;
    if (ana.best < 0.6f && ana.agc_used > 0.f) {
        float den = ana.b0 - 2.f * ana.b1 + ana.b2, off = 0.f;
        if (den > 1e-6f) off = clampf(0.5f * (ana.b0 - ana.b2) / den, -0.5f, 0.5f);
        float lag = (float)ana.best_tau + off;
        hz = 11025.f / lag;
        conf = clampf(1.f - ana.best * 2.5f, 0.f, 1.f);
    }
    ana.voiced = conf > 0.5f;
    vsc_meter.ana_conf = conf;
    if (!ana.voiced) {
        for (unsigned h = 0; h < ANA_PART; ++h) vsc_meter.ana_harm[h] *= 0.7f;
        return;
    }
    ana.note = 69.f + 12.f * log2a(hz * (1.f / 440.f));
    vsc_meter.ana_pitch = hz;
    /* the partials' levels over the same window: a triangle window, one Goertzel each */
    float norm = 2.f / (float)(ANA_W / 2u * ANA_W / 2u) / ana.agc_used;
    for (unsigned h = 0; h < ANA_PART; ++h) {
        float f = hz * (float)(h + 1u);
        if (f > 5000.f) { vsc_meter.ana_harm[h] = 0.f; continue; }
        float c = 2.f * cos2pi(f * (1.f / 11025.f)), s1 = 0.f, s2 = 0.f;
        for (unsigned j = 0; j < ANA_W; ++j) {
            int w = j < ANA_W / 2u ? (int)j : (int)(ANA_W - j);
            float x = (float)(ana.ring[(ana.ws + j) & (ANA_RING - 1u)] * w);
            float s0 = x + c * s1 - s2; s2 = s1; s1 = s0;
        }
        float pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        float lp = f * (1.f / 2500.f);   /* undo the two 2.5 kHz poles in front of the history */
        vsc_meter.ana_harm[h] = sqrta(pw) * norm * (1.f + lp * lp);
    }
    /* high notes: the integer lag can miss the first dip and find the second, an octave
     * low. Then the "fundamental" and every other partial are empty: move up an octave */
    float *hm = vsc_meter.ana_harm;
    if (hz < 700.f && hm[1] > 0.01f && hm[0] < hm[1] * 0.2f && hm[2] < hm[1] * 0.2f) {
        hz *= 2.f; ana.note += 12.f; vsc_meter.ana_pitch = hz;
        hm[0] = hm[1]; hm[1] = hm[3]; hm[2] = hm[5]; hm[3] = hm[7];
        for (unsigned h = 4; h < ANA_PART; ++h) hm[h] = 0.f;
    }
}
static VSC_HOT_SMALL void ana_block(const struct vsc_inputs *in, int wanted)
{
    for (unsigned s = 0; s < ANA_SUBS; ++s) ana.hit[s] = 0.f;
    vsc_meter.ana_on = (uint8_t)wanted;
    vsc_meter.ana_hit *= 0.85f;
    if (!wanted) { ana.voiced = 0; vsc_meter.ana_conf = 0.f; return; }
    unsigned sel = vsc.g[G_ASRC];
    const float *mono = sel >= IN_T1 && sel <= IN_T4 ? tr[sel - IN_T1].prev : sel == IN_MAIN ? main_prev : 0;
    const float *bt = !mono && (sel == IN_BT || sel == IN_BOTH) ? in->bt : 0;
    const float *usb = !mono && (sel == IN_USB || sel == IN_BOTH) ? in->usb : 0;
    float sens = (float)vsc.g[G_ASENS] * (1.f / 127.f);
    float base = 0.12f + 1.4f * (1.f - sens) * (1.f - sens);
    unsigned focus = vsc.g[G_AFOCUS] & 3u;
    float pk = 0.f;
    for (unsigned s = 0; s < ANA_SUBS; ++s) {
        float sl = 0.f, sm = 0.f, sh = 0.f;
        for (unsigned i = 0; i < VSC_SUB; ++i) {
            unsigned j = s * VSC_SUB + i;
            float x = mono ? mono[j] : 0.f;
            if (bt) x += (bt[2 * j] + bt[2 * j + 1]) * 0.5f;
            if (usb) x += (usb[2 * j] + usb[2 * j + 1]) * 0.5f;
            ana.lp1 += (x - ana.lp1) * 0.0211f;          /* 150 Hz */
            ana.lp2 += (x - ana.lp2) * 0.2997f;          /* 2.5 kHz */
            ana.lp3 += (ana.lp2 - ana.lp3) * 0.2997f;    /* and again, before the decimator */
            sl += absf(ana.lp1); sm += absf(ana.lp2 - ana.lp1); sh += absf(x - ana.lp2);
            float a = absf(x); if (a > pk) pk = a;
            if (++ana.dphase >= 4u) {
                ana.dphase = 0;
                float v = ana.lp3, av = absf(v);
                ana.agc = av > ana.agc ? av : ana.agc * 0.9995f;
                float g = 100.f / (ana.agc > 0.002f ? ana.agc : 0.002f);
                ana.ring[ana.dw & (ANA_RING - 1u)] = (int8_t)(int)clampf(v * g, -127.f, 127.f);
                ++ana.dw;
            }
        }
        float L[3] = {sl * (2.f / VSC_SUB), sm * (2.f / VSC_SUB), sh * (2.f / VSC_SUB)};
        float flux = 0.f, lev = 0.f;
        for (unsigned b = 0; b < 3; ++b) {
            /* each band's envelope: instant rise, ~36 ms fall, so a waveform's own ripple
             * (a low note's cycle, a saw's edge) doesn't read as a hit */
            float e = L[b] > ana.prev[b] ? L[b] : ana.prev[b] + (L[b] - ana.prev[b]) * 0.02f;
            ana.band[s][b] = clampf(e, 0.f, 1.f);
            if (!focus || focus - 1u == b) { if (e > ana.prev[b]) flux += e - ana.prev[b]; lev += e; }
            ana.prev[b] = e;
        }
        float odf = flux / (ana.slow + 0.01f);
        ana.slow += (lev - ana.slow) * 0.015f;
        ana.odf_avg += (odf - ana.odf_avg) * 0.03f;
        float thr = base + ana.odf_avg * 2.f;
        if (ana.refr) --ana.refr;
        else if (odf > thr && lev > 0.005f) {
            float st = clampf(lev * 3.f, 0.15f, 1.f);
            ana.hit[s] = st; ana.refr = 50u;
            ++vsc_meter.ana_hits;
            if (st > vsc_meter.ana_hit) vsc_meter.ana_hit = st;
        }
    }
    {   /* the meters: the last sub-block's bands, lightly smoothed */
        const float *b = ana.band[ANA_SUBS - 1u];
        vsc_meter.ana_low += (b[0] - vsc_meter.ana_low) * 0.3f;
        vsc_meter.ana_mid += (b[1] - vsc_meter.ana_mid) * 0.3f;
        vsc_meter.ana_high += (b[2] - vsc_meter.ana_high) * 0.3f;
        float sum = b[0] + b[1] + b[2];
        vsc_meter.ana_bright = sum > 0.01f ? b[2] / sum : vsc_meter.ana_bright * 0.95f;
    }
    /* the pitch: quiet sources skip it (a reading starts over when sound returns), and
     * the CPU guard's second step pauses it (the last pitch holds) */
    if (pk < 0.003f) { ana.blk = 0; ana.voiced = 0; vsc_meter.ana_conf *= 0.8f; return; }
    if (vsc_relief >= 2) { ana.blk = 0; return; }
    if (ana.blk == 0) {
        ana.ws = ana.dw - (ANA_W + ANA_TMAX + 1u);
        ana.tau = ANA_TMIN; ana.cum = 0.f; ana.found = 0; ana.best = 9.f; ana.best_tau = 0;
        ana.p1 = ana.p2 = 9.f; ana.agc_used = 100.f / (ana.agc > 0.002f ? ana.agc : 0.002f);
    }
    if (ana.blk < ANA_PERIOD - 1u) {
        for (unsigned n = 0; n < ANA_LAGS && !ana.found && ana.tau <= ANA_TMAX; ++n, ++ana.tau) {
            /* every other frame of the window (as good for this, half the work), in runs
             * where neither read wraps the ring, so the inner loop has no masking */
            int32_t d = 0;
            unsigned i0 = ana.ws & (ANA_RING - 1u), i1 = (ana.ws + ana.tau) & (ANA_RING - 1u);
            for (unsigned j = 0; j < ANA_W;) {
                unsigned run = ANA_W - j, r0 = ANA_RING - i0, r1 = ANA_RING - i1;
                if (r0 < run) run = r0;
                if (r1 < run) run = r1;
                const int8_t *p = ana.ring + i0, *q = ana.ring + i1;
                for (unsigned k = 0; k < run; k += 2) { int e = p[k] - q[k]; d += e * e; }
                run = (run + 1u) & ~1u;
                j += run; i0 = (i0 + run) & (ANA_RING - 1u); i1 = (i1 + run) & (ANA_RING - 1u);
            }
            float df = (float)d;
            ana.cum += df;
            float cm = ana.cum > 0.f ? df * (float)(ana.tau - ANA_TMIN + 1u) / ana.cum : 1.f;
            /* a dip at the previous lag: the first under 0.2 wins, else the deepest */
            if (ana.p1 < ana.p2 && ana.p1 <= cm) {
                if (ana.p1 < 0.2f || ana.p1 < ana.best) {
                    ana.best = ana.p1; ana.best_tau = ana.tau - 1u;
                    ana.b0 = ana.p2; ana.b1 = ana.p1; ana.b2 = cm;
                    if (ana.p1 < 0.2f) ana.found = 1;
                }
            }
            ana.p2 = ana.p1; ana.p1 = cm;
        }
    } else ana_pitch_end();
    if (++ana.blk >= ANA_PERIOD) ana.blk = 0;
}
/* is anything listening this block? */
static VSC_HOT_SMALL int ana_wanted(void)
{
    if (vsc_ana_view || vsc.g[G_AHITS] != AHITS_OFF) return 1;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        const uint8_t *b = vsc.t[t];
        unsigned ft = b[P_FTYPE];
        if (ft == FT_REBUILD || (ft == FT_RING && (b[P_KEXCITE] || b[P_KFOLLOW]))) return 1;
        if (b[P_GTRIG] != GTRIG_FREE || b[P_GQUANT] == GQUANT_TUNE) return 1;
        for (unsigned m = 0; m < VSC_MODS; ++m)
            if ((b[P_MOD(m, 0)] & 3) == MT_FOLLOW && b[P_MOD(m, 5)] >= FIN_LOW) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- mods
 * Four slots per track, each WAVE, RANDOM, FOLLOW or ADSR. Every modulator puts out
 * 0..1; a route reads that as is (unipolar) or as -1..1 (bipolar). Rates sync to
 * the tempo in STRAIGHT, TRIPLET or DOTTED divisions, and restart with PLAY. */
static VSC_HOTDATA const float div_cycle[10] = {32.f, 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f, 0.0625f};
static VSC_HOTDATA const float sync_mul[4] = {1.f, 1.f, 2.f / 3.f, 1.5f};
VSC_INL float mod_rate(float r, unsigned sync)
{
    if (!sync) return 0.02f * exp2a(r * 10.f);                        /* 0.02 .. 20 Hz */
    unsigned d = (unsigned)(r * 9.99f);
    return tempo_now * (1.f / 60.f) / (div_cycle[d] * sync_mul[sync & 3]);
}
VSC_INL float curve_to(float x, float c)   /* c -1..1: log .. linear .. exp */
{
    if (c > 0.f) return x + (x * x * x - x) * c;
    return x + ((1.f - (1.f - x) * (1.f - x) * (1.f - x)) - x) * -c;
}
static VSC_HOT void mods_restart(void)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned m = 0; m < VSC_MODS; ++m) { tr[t].ms[m].ph = 0.f; tr[t].ms[m].idx = 0; }
}
static VSC_HOT void run_mods(unsigned t, unsigned off, unsigned frames)
{
    struct track *k = &tr[t];
    const uint8_t *b = vsc.t[t];
    float dt = (float)frames * (1.f / (float)VSC_SR);
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        struct modst *s = &k->ms[m];
        unsigned type = b[P_MOD(m, 0)] & 3;
        const float *e = &k->e[P_MOD(m, 0)];
        float out = 0.f;
        switch (type) {
        case MT_WAVE: {
            s->ph += mod_rate(e[1], b[P_MOD(m, 9)]) * dt;
            int wrapped = s->ph >= 1.f;
            if (wrapped) { s->ph -= (float)floori(s->ph); s->held = rndu(); }
            float p = s->ph + e[3]; if (p >= 1.f) p -= 1.f;
            float sk = clampf(e[5], 0.02f, 0.98f);                       /* SKEW */
            p = p < sk ? p / sk * 0.5f : 0.5f + (p - sk) / (1.f - sk) * 0.5f;
            float v;
            switch (b[P_MOD(m, 8)]) {
            case 0: v = 0.5f - 0.5f * sin01(p < 0.75f ? p + 0.25f : p - 0.75f); break;   /* rises from 0 */
            case 1: v = p < 0.5f ? p * 2.f : 2.f - p * 2.f; break;
            case 2: v = p < 0.5f ? 1.f : 0.f; break;
            case 3: v = 1.f - p; break;
            case 4: v = p; break;
            default: v = s->held; break;
            }
            float fold = e[6];
            if (fold > 0.f) {                                            /* FOLD */
                float x = (v * 2.f - 1.f) * (1.f + fold * 3.f);
                for (int i = 0; i < 4 && (x > 1.f || x < -1.f); ++i) x = x > 1.f ? 2.f - x : -2.f - x;
                v = (x + 1.f) * 0.5f;
            }
            v = curve_to(clampf(v, 0.f, 1.f), e[7] * 2.f - 1.f);         /* CURVE */
            out = v * e[2] + (e[4] * 2.f - 1.f);                         /* AMOUNT, OFFSET */
            break;
        }
        case MT_RANDOM: {
            unsigned len = b[P_MOD(m, 5)] % 16u + 1u;
            s->ph += mod_rate(e[1], b[P_MOD(m, 9)]) * dt;
            if (s->ph >= 1.f) {
                s->ph -= (float)floori(s->ph);
                s->idx = (uint8_t)((s->idx + 1) % len);
                if (rndu() < e[6] || s->steps[s->idx] == 0) s->steps[s->idx] = (uint8_t)(1u + (rnd() >> 24) % 255u);   /* VARIATION */
            }
            unsigned at = (s->idx + (unsigned)(e[3] * (float)len)) % len;       /* PHASE */
            float target = (float)s->steps[at] * (1.f / 255.f);
            float sm = e[7];
            s->val += (target - s->val) * (sm < 0.01f ? 1.f : clampf(dt * (1.f / (0.002f + sm * sm * 1.5f)), 0.f, 1.f));
            out = s->val * e[2] + (e[4] * 2.f - 1.f);
            break;
        }
        case MT_FOLLOW: {
            unsigned src = b[P_MOD(m, 5)];
            if (src >= FIN_COUNT) src = FIN_TRACK;
            float pk = 0.f;
            if (src >= FIN_LOW) {
                /* what DISSECT's listener hears, this sub-block */
                unsigned sub = off / VSC_SUB; if (sub >= ANA_SUBS) sub = ANA_SUBS - 1u;
                const float *bd = ana.band[sub];
                switch (src) {
                case FIN_LOW: pk = bd[0]; break;
                case FIN_MID: pk = bd[1]; break;
                case FIN_HIGH: pk = bd[2]; break;
                case FIN_HITS: pk = ana.hit[sub]; break;
                case FIN_PITCH: pk = ana.note > 0.f ? clampf((ana.note - 33.f) * (1.f / 60.f), 0.f, 1.f) : 0.f; break;   /* A1 .. A6, held */
                default: { float sum = bd[0] + bd[1] + bd[2]; pk = sum > 0.01f ? clampf(bd[2] / sum * 2.f, 0.f, 1.f) : 0.f; } break;
                }
            } else if (src >= 2 && src <= 5) pk = vsc_meter.peak[src - 2];
            else if (src == 6) pk = vsc_meter.main_peak;
            else {
                const float *buf = src == 0 ? k->out + off : src == 7 ? k->in + off : 0;
                if (!buf) pk = k->mat_pk;
                else {
                    float f = e[6] * 2.f - 1.f, c = 0.02f + 0.5f * (1.f - absf(f));
                    for (unsigned i = 0; i < frames; ++i) {
                        k->fol_lp += (buf[i] - k->fol_lp) * c;
                        float v = f < -0.02f ? k->fol_lp : f > 0.02f ? buf[i] - k->fol_lp : buf[i];
                        float a = absf(v); if (a > pk) pk = a;
                    }
                }
            }
            if (src != FIN_PITCH && src != FIN_BRIGHT) pk *= 0.5f + 4.f * e[8];   /* GAIN (levels only) */
            float rise = 0.001f + 0.5f * e[3] * e[3], fall = 0.01f + 2.f * e[4] * e[4];
            float c = pk > s->env ? dt / (rise + dt) : dt / (fall + dt);
            s->env += (pk - s->env) * c;
            float v = clampf(s->env, 0.f, 1.f);
            /* DELAY: a short history, up to ~0.7 s */
            s->dl[s->dlw] = (uint8_t)(v * 255.f); s->dlw = (uint8_t)((s->dlw + 1) % 24);
            unsigned back = (unsigned)(e[1] * 23.f);
            if (back) v = (float)s->dl[(s->dlw + 24 - 1 - back) % 24] * (1.f / 255.f);
            if (e[7] > 0.01f) {                                          /* HOLD: sample and hold */
                s->hold_ph += 0.5f * exp2a(e[7] * 6.f) * dt;
                if (s->hold_ph >= 1.f) { s->hold_ph -= (float)floori(s->hold_ph); s->held = v; }
                v = s->held;
            }
            k->follow = v;
            out = v * e[2];
            break;
        }
        default: {   /* ADSR */
            float A = 0.001f + 4.f * e[1] * e[1], D = 0.005f + 4.f * e[2] * e[2], S = e[3], R = 0.005f + 6.f * e[4] * e[4];
            switch (s->stage) {
            case 1: s->sp += dt / A; if (s->sp >= 1.f) { s->sp = 0.f; s->stage = 2; s->env = 1.f; }
                    else s->env = s->from + (1.f - s->from) * curve_to(s->sp, e[5] * 2.f - 1.f); break;
            case 2: s->sp += dt / D; if (s->sp >= 1.f) { s->stage = 3; s->env = S; }
                    else s->env = 1.f - (1.f - S) * curve_to(s->sp, e[6] * 2.f - 1.f); break;
            case 3: s->env = S; break;
            case 4: s->sp += dt / R; if (s->sp >= 1.f) { s->stage = 0; s->env = 0.f; }
                    else s->env = s->from * (1.f - curve_to(s->sp, e[7] * 2.f - 1.f)); break;
            default: s->env = 0.f; break;
            }
            out = s->env * e[8];
            break;
        }
        }
        vsc_meter.mod_value[t][m] = clampf(out, 0.f, 1.f);
        vsc_meter.mod_phase[t][m] = type == MT_WAVE ? s->ph : type == MT_RANDOM ? ((float)s->idx + s->ph) / (float)(b[P_MOD(m, 5)] % 16u + 1u) : 0.f;
    }
}

/* ------------------------------------------------------------ material */
VSC_INL float play_inc(unsigned t)
{
    struct track *k = &tr[t];
    float sp = (k->e[P_SPEED] - 0.5039f) * 4.0315f; /* 64 -> 0, 96 -> 1 */
    float semis = (float)vsc.t[t][P_PITCH] - 24.f + (float)k->pitch_note;
    return sp * exp2a(semis * (1.f / 12.f)) * src_rate(t) * (1.f / (float)VSC_SR);
}
/* Advances a playhead within [a,b) by inc (signed). Returns 0 when a ONCE
 * playhead runs off the end. */
VSC_INL int advance(uint32_t *ipos, float *fpos, int8_t *dir, float inc, uint32_t a, uint32_t b, unsigned loopm)
{
    float step = inc * (float)*dir;
    float f = *fpos + step;
    int whole = floori(f);
    *fpos = f - (float)whole;
    /* 32-bit and no division: positions are far below 2^31 and a step moves only a
     * few frames, so wrapping by subtraction ends at once (64-bit maths and the
     * compiler's divide helpers live in flash, which the audio path must not touch) */
    int32_t np = (int32_t)*ipos + whole;
    if (np >= (int32_t)b || np < (int32_t)a) {
        if (loopm == 0) return 0;
        int32_t span = (int32_t)(b - a);
        if (span <= 0) return 0;
        if (loopm == 1) {
            for (unsigned guard = 0; np >= (int32_t)b && guard < 64; ++guard) np -= span;
            for (unsigned guard = 0; np < (int32_t)a && guard < 64; ++guard) np += span;
            if (np < (int32_t)a || np >= (int32_t)b) np = (int32_t)a;
        } else {
            *dir = (int8_t)-*dir;
            np = np >= (int32_t)b ? (int32_t)b - 1 : (int32_t)a;
        }
    }
    *ipos = (uint32_t)np;
    return 1;
}
static VSC_HOT void material(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *out = k->out + off;
    const float *in = k->in + off;
    float gain = k->e[P_GAIN] * k->e[P_GAIN] * 2.f;
    unsigned mode = vsc.t[t][P_MODE], loopm = vsc.t[t][P_LOOPM];
    if (mode == MODE_THRU) {
        for (unsigned i = 0; i < n; ++i) out[i] = in[i] * gain;
        return;
    }
    if (mode == MODE_LOOP) {
        uint32_t cap = loop_capacity(t);
        unsigned lo = vsc.t[t][P_QUAL];
        if (k->loop_state == 1) { /* recording: monitor the input */
            for (unsigned i = 0; i < n; ++i) {
                float x = in[i];
                if (!lo || (k->rec_pos & 0x80000000u) == 0) {
                    k->rec_phase += lo ? 0.5f : 1.f;
                    if (k->rec_phase >= 1.f) {
                        k->rec_phase -= 1.f;
                        if (k->rec_pos < cap) loop_put(t, k->rec_pos++, x);
                    }
                }
                out[i] = x * gain;
            }
            if (k->rec_pos >= cap) loop_rec(t);
            return;
        }
        uint32_t len = src_len(t), a, b;
        if (!len) { for (unsigned i = 0; i < n; ++i) out[i] = 0.f; return; }
        window(t, len, &a, &b);
        if (k->ipos < a || k->ipos >= b) k->ipos = a;
        float inc = play_inc(t) * (1.f / src_rate(t)) * src_rate(t);
        float dub = k->e[P_DUB];
        if (inc < 0.f) { inc = -inc; if (k->dir > 0 && loopm != 2) k->dir = -1; }
        else if (k->dir < 0 && loopm != 2) k->dir = 1;
        for (unsigned i = 0; i < n; ++i) {
            if (!k->running) { out[i] = 0.f; continue; }
            uint32_t j = k->ipos;
            uint32_t j2 = j + 1 < b ? j + 1 : a;
            float x = loop_at(t, j);
            float y = x + (loop_at(t, j2) - x) * k->fpos;
            if (k->declick < 1.f) { y *= k->declick; k->declick += 1.f / 64.f; }
            out[i] = y * gain;
            if (k->loop_state == 3) loop_put(t, j, x * dub + in[i]);
            if (!advance(&k->ipos, &k->fpos, &k->dir, inc, a, b, loopm)) k->running = 0;
        }
        return;
    }
    if (mode == MODE_PLAY) {
        uint32_t len = src_len(t), a, b;
        if (!len || !k->running) { for (unsigned i = 0; i < n; ++i) out[i] = 0.f; return; }
        window(t, len, &a, &b);
        if (k->ipos < a || k->ipos >= b) k->ipos = a;
        float inc = play_inc(t);
        if (inc < 0.f) { inc = -inc; if (k->dir > 0 && loopm != 2) k->dir = -1; }
        else if (k->dir < 0 && loopm != 2) k->dir = 1;
        int slot = vsc.t[t][P_SLOT];
        for (unsigned i = 0; i < n; ++i) {
            if (!k->running) { out[i] = 0.f; continue; }
            uint32_t j = k->ipos;
            uint32_t j2 = j + 1 < b ? j + 1 : j;
            float x = bank_at(&k->rd, slot, j);
            float y = x + (bank_at(&k->rd, slot, j2) - x) * k->fpos;
            if (k->declick < 1.f) { y *= k->declick; k->declick += 1.f / 64.f; }
            out[i] = y * gain;
            if (!advance(&k->ipos, &k->fpos, &k->dir, inc, a, b, loopm)) k->running = 0;
        }
        return;
    }
    /* KEYS */
    for (unsigned i = 0; i < n; ++i) out[i] = 0.f;
    uint32_t len = src_len(t), a, b;
    if (!len) return;
    window(t, len, &a, &b);
    float dt = 1.f / (float)VSC_SR;
    float atk = 0.001f + 2.f * k->e[P_ATK] * k->e[P_ATK];
    float dec = 0.005f + 3.f * k->e[P_DEC] * k->e[P_DEC];
    float sus = k->e[P_SUS];
    float rel = 0.005f + 4.f * k->e[P_REL] * k->e[P_REL];
    float glide = k->e[P_GLIDE];
    float gl = glide > 0.01f ? 1.f - exp2a(-(float)n / (glide * glide * 8000.f + 1.f)) : 1.f;
    int slot = vsc.t[t][P_SLOT];
    for (unsigned vi = 0; vi < VSC_KEYS_VOICES; ++vi) {
        struct voice *v = &k->v[vi];
        if (!v->on) continue;
        v->inc += (v->target_inc - v->inc) * gl;
        float ia = dt / atk, id = dt / dec, ir = dt / rel;
        for (unsigned i = 0; i < n; ++i) {
            switch (v->stage) {
            case 0: v->env += ia; if (v->env >= 1.f) { v->env = 1.f; v->stage = 1; } break;
            case 1: v->env += (sus - v->env) * id * 4.f; break;
            default: v->env -= v->env * ir * 4.f + 0.00002f; if (v->env <= 0.f) { v->env = 0.f; v->on = 0; } break;
            }
            if (!v->on) break;
            uint32_t j = v->ipos;
            uint32_t j2 = j + 1 < b ? j + 1 : j;
            float x, y;
            if (slot >= VSC_BANK_SLOTS) { x = loop_at(t, j); y = loop_at(t, j2); }
            else { x = bank_at(&k->rd, slot, j); y = bank_at(&k->rd, slot, j2); }
            out[i] += (x + (y - x) * v->fpos) * v->env * v->vel * gain;
            if (!advance(&v->ipos, &v->fpos, &v->dir, v->inc, a, b, loopm)) { v->on = 0; break; }
        }
    }
}

/* -------------------------------------------------------------- grains */
static VSC_HOTDATA const int8_t quant_steps[4] = {0, 1, 12, 7};
/* DISSECT on GRAIN: TRIG HITS starts a burst of grains (RATE sets how many, 1..8, half a
 * grain apart) at each hit the listener hears; NOTES does the same for each note (keys,
 * steps, CAROM), played at the note. The grains start where a remembered hit fell in
 * this track's buffer: SCAN picks how many hits back, SPRAY scatters among older ones.
 * QUANT TUNE pitches each grain so the pitch heard at its hit lands on the note (NOTES)
 * or on C (HITS, FREE: the pitch heard now). FREE is as before. */
static VSC_HOT void grains(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    float mix = k->e[P_GMIX];
    int gsend = vsc.t[t][P_GWMODE] == WET_SEND;
    uint8_t *ring = grain_ring[t];
    int freeze = vsc.t[t][P_GFREEZE];
    float fb = k->e[P_GFDBK] * 0.9f;
    unsigned trig = vsc.t[t][P_GTRIG];
    {   /* a hit heard in this sub-block marks the place it falls in the ring */
        float hit = ana.hit[off / VSC_SUB < ANA_SUBS ? off / VSC_SUB : 0];
        if (hit > 0.f && !freeze) {
            struct gev *ev = &k->ev[k->ev_w];
            ev->at = k->gtotal; ev->note = ana_note_now();
            k->ev_w = (uint8_t)((k->ev_w + 1u) % VSC_GEVENTS);
            if (k->ev_n < VSC_GEVENTS) ++k->ev_n;
            if (trig == GTRIG_HITS) { k->gburst = (uint8_t)(1u + (unsigned)(k->e[P_GDENS] * 7.99f)); k->gwait = 0; }
        }
        if (k->gnote_hit) {
            k->gnote_hit = 0;
            if (trig == GTRIG_NOTES) { k->gburst = (uint8_t)(1u + (unsigned)(k->e[P_GDENS] * 7.99f)); k->gwait = 0; }
        }
    }
    if (mix <= 0.001f && fb <= 0.f) {
        /* Keep the ring filled so the grains have material when MIX rises. */
        if (!freeze) {
            for (unsigned i = 0; i < n; ++i) { ring[k->gw] = ulaw_enc(x[i]); if (++k->gw >= VSC_GRAIN_FRAMES) k->gw = 0; }
            k->gtotal += n;
        }
        for (unsigned g = 0; g < VSC_GRAINS; ++g) k->g[g].active = 0;
        k->gburst = 0;
        return;
    }
    float size = k->e[P_GSIZE];
    uint32_t glen = (uint32_t)((0.008f + size * size * 0.35f) * (float)VSC_SR);
    if (glen > VSC_GRAIN_FRAMES / 3) glen = VSC_GRAIN_FRAMES / 3;
    float dens = k->e[P_GDENS];
    float per_sec = 0.5f + dens * dens * 80.f;
    float spawn = per_sec / (float)VSC_SR;
    float shape = k->e[P_GSHAPE];
    unsigned qm = vsc.t[t][P_GQUANT];
    int32_t gap = (int32_t)(glen / 2u > 64u ? glen / 2u : 64u);
    for (unsigned i = 0; i < n; ++i) {
        int fire = 0;
        if (trig == GTRIG_FREE) {
            k->gphase += spawn;
            if (k->gphase >= 1.f) { k->gphase -= 1.f; fire = 1; }
        } else if (k->gburst && --k->gwait <= 0) { fire = 1; --k->gburst; k->gwait = gap; }
        if (fire) {
            /* relief 2: new grains only in half the slots (playing ones finish) */
            unsigned slots = vsc_relief >= 2 ? VSC_GRAINS / 2 : VSC_GRAINS;
            for (unsigned g = 0; g < slots; ++g) {
                struct grain *gr = &k->g[g];
                if (gr->active) continue;
                float semis = k->e[P_GPITCH] * 48.f - 24.f + rndb() * k->e[P_GDETUNE] * 12.f;
                int q = qm < 4 ? quant_steps[qm] : 0;
                if (q) { float s = semis / (float)q; semis = (float)floori(s + 0.5f) * (float)q; }
                /* which remembered hit (HITS, NOTES): SCAN back, SPRAY among older ones */
                const struct gev *ev = 0;
                if (trig != GTRIG_FREE && k->ev_n) {
                    unsigned back_n = (unsigned)(k->e[P_GTIME] * 7.99f) + (unsigned)(rndu() * k->e[P_GSPRAY] * 7.99f);
                    if (back_n >= k->ev_n) back_n = k->ev_n - 1u;
                    ev = &k->ev[(k->ev_w + VSC_GEVENTS - 1u - back_n) % VSC_GEVENTS];
                }
                if (trig == GTRIG_NOTES && qm != GQUANT_TUNE) semis += k->gnote - 60.f;
                if (qm == GQUANT_TUNE) {
                    float heard = trig == GTRIG_FREE ? ana_note_now() : ev ? ev->note : 0.f;
                    float want = trig == GTRIG_NOTES ? k->gnote : 60.f;
                    if (heard > 0.f) semis += clampf(want - heard, -36.f, 36.f);
                    else if (trig == GTRIG_NOTES) semis += k->gnote - 60.f;
                }
                float ratio = exp2a(semis * (1.f / 12.f));
                int rev = rndu() < k->e[P_GREV];
                float need = (float)glen * (ratio > 1.f ? ratio : 1.f) + 64.f;
                float room = (float)VSC_GRAIN_FRAMES - need - 64.f;
                float back = need + k->e[P_GTIME] * room * 0.9f + rndu() * k->e[P_GSPRAY] * room * 0.5f;
                if (ev) {
                    /* start at the hit; far enough behind the write head not to overtake it */
                    float age = (float)(k->gtotal + i - ev->at);
                    float lead = rev ? (float)glen * ratio + 32.f : ratio > 1.f ? (float)glen * (ratio - 1.f) + 32.f : 16.f;
                    float b2 = age > lead ? age : lead;
                    if (b2 + need < (float)VSC_GRAIN_FRAMES - 64.f) back = b2;   /* else the hit is overwritten: as FREE */
                }
                if (back > (float)VSC_GRAIN_FRAMES - 64.f) back = (float)VSC_GRAIN_FRAMES - 64.f;
                float start = (float)k->gw - back;
                if (rev) start += (float)glen * ratio;
                while (start < 0.f) start += (float)VSC_GRAIN_FRAMES;
                gr->pos = start; gr->inc = rev ? -ratio : ratio;
                gr->len = glen; gr->t = 0; gr->active = 1;
                gr->amp = 0.85f + 0.15f * rndu();
                break;
            }
        }
        float acc = 0.f;
        for (unsigned g = 0; g < VSC_GRAINS; ++g) {
            struct grain *gr = &k->g[g];
            if (!gr->active) continue;
            float ph = (float)gr->t / (float)gr->len;
            float hann = 4.f * ph * (1.f - ph);
            float w;
            if (shape < 0.5f) {
                float d = 1.f - ph, perc = (ph < 0.03f ? ph * 33.3f : d * d);
                w = perc + (hann - perc) * (shape * 2.f);
            } else {
                float rv = ph > 0.97f ? (1.f - ph) * 33.3f : ph * ph;
                w = hann + (rv - hann) * (shape * 2.f - 1.f);
            }
            int ip = floori(gr->pos);
            float fr = gr->pos - (float)ip;
            uint32_t i0 = (uint32_t)ip % VSC_GRAIN_FRAMES, i1 = (i0 + 1) % VSC_GRAIN_FRAMES;
            float a = ulaw_table[ring[i0]], b = ulaw_table[ring[i1]];
            acc += (a + (b - a) * fr) * w * gr->amp;
            gr->pos += gr->inc;
            if (gr->pos >= (float)VSC_GRAIN_FRAMES) gr->pos -= (float)VSC_GRAIN_FRAMES;
            else if (gr->pos < 0.f) gr->pos += (float)VSC_GRAIN_FRAMES;
            if (++gr->t >= gr->len) gr->active = 0;
        }
        acc *= 0.6f;
        if (!freeze) {
            ring[k->gw] = ulaw_enc(tanha(x[i] + k->gfb * fb));
            if (++k->gw >= VSC_GRAIN_FRAMES) k->gw = 0;
        }
        k->gfb = acc;
        x[i] = gsend ? x[i] + acc * mix : x[i] + (acc - x[i]) * mix;
    }
    if (!freeze) k->gtotal += n;
}

/* -------------------------------------------------------------- filter */
static VSC_HOTDATA const int8_t scale_iv[8][4] = {
    {0, 12, 24, 36}, {0, 7, 12, 19}, {0, 4, 7, 11}, {0, 3, 7, 10},
    {0, 5, 7, 12}, {0, 12, 19, 24}, {0, 2, 4, 6}, {0, 1, 2, 3}};
VSC_INL float svf_g(float hz)
{
    hz = clampf(hz, 16.f, 0.45f * (float)VSC_SR);
    return tanpre(3.14159265f * hz / (float)VSC_SR);
}
/* ---------------------------------------------------------------- RING
 * A bank of tuned resonators in the FILTER slot, after the S-4's RING: the bands sit
 * on a scale from PITCH, CUTOFF and SLOPE shape which of them speak (low, band or
 * high), RESO sharpens that shape, DECAY sets how long they ring, WAVES sweeps a
 * swell across the bands and NOISE flickers them at random. D.MODE puts that shaping
 * before the ringing (SUSTAIN: bands ring on after they close) or after it (CHOKE:
 * closing a band silences it). TILT and DETUNE spread the bands across the stereo
 * field, TONE tilts the result, WET blends it with the dry signal.
 * Each band is a constant-peak-gain two-pole resonator: about five multiplies a frame. */
static VSC_HOTDATA const uint16_t ring_scales[8] = {
    0xfff,                                  /* CHROM */
    (1<<0)|(1<<2)|(1<<4)|(1<<5)|(1<<7)|(1<<9)|(1<<11),   /* MAJOR */
    (1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<8)|(1<<10),   /* MINOR */
    (1<<0)|(1<<2)|(1<<4)|(1<<7)|(1<<9),                  /* PENTA */
    (1<<0)|(1<<3)|(1<<5)|(1<<6)|(1<<7)|(1<<10),          /* BLUES */
    (1<<0)|(1<<2)|(1<<4)|(1<<6)|(1<<8)|(1<<10),          /* WHOLE */
    (1<<0)|(1<<7),                                       /* FIFTHS */
    (1<<0)};                                             /* OCTAVE */
/* HARM, MEMBR, BAR: 12 log2 of the partials' ratios (1..10; Bessel zeros for a drum
 * head's modes; a free bar's) */
static VSC_HOTDATA const float ring_ratio_semis[3][VSC_RING_BANDS] = {
    {0.00f, 12.00f, 19.02f, 24.00f, 27.86f, 31.02f, 33.69f, 36.00f, 38.04f, 39.86f},
    {0.00f, 8.07f, 13.14f, 14.39f, 16.89f, 18.54f, 19.90f, 21.69f, 22.18f, 24.26f},
    {0.00f, 17.55f, 29.21f, 37.91f, 44.86f, 50.64f, 55.60f, 59.93f, 63.78f, 67.25f}};
static VSC_HOT void ring(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off, *sd = k->side;
    const float dt = (float)n * (1.f / (float)VSC_SR);
    /* pitches: ten bands rising ~0.75 octave each from C2, snapped upward onto the scale */
    if (k->kmode != FT_RING) {   /* REBUILD had this state: start clean */
        k->kmode = FT_RING; k->kok = 0;
        for (unsigned b = 0; b < VSC_RING_BANDS; ++b) { k->ky1[b] = k->ky2[b] = 0.f; k->kgain[b] = 0.f; }
    }
    unsigned ksc = vsc.t[t][P_KSCALE];
    /* SCALE: a scale's notes, or HARM, MEMBR, BAR (fixed ratios: a string's, a drum
     * head's, a struck bar's partials) */
    unsigned mask = ksc >= KSCALE_RATIOS ? 0x1000u << ((ksc - KSCALE_RATIOS) % 3u) : ring_scales[ksc & 7];
    float semis0 = clampf(k->e[P_KPITCH] * 127.f - 24.f, -24.f, 24.f);
    if (vsc.t[t][P_KFOLLOW]) {
        /* FOLLOW: the lowest band on the pitch the listener hears, to the semitone (so the
         * tuning is only remade when the note changes), held through silence; PITCH shifts it */
        if (ana.voiced) k->kfol = (float)floori(ana.note + 0.5f);
        if (k->kfol > 0.f) semis0 = clampf(k->kfol - 36.f + semis0, -24.f, 48.f);
    }
    float decay = k->e[P_KDECAY], t60 = 0.03f * exp2a(decay * 7.f);
    float r = exp2a(-9.966f / (t60 * (float)VSC_SR));          /* 60 dB over t60 */
    float b0 = (1.f - r * r) * 0.5f, a2 = r * r;
    float cut = k->e[P_FCUT], reso = k->e[P_FRES], slope = k->e[P_KSLOPE];
    float width = 0.04f + (1.f - reso) * (1.f - reso) * 0.4f;
    float waves = k->e[P_KWAVES], noise = k->e[P_KNOISE];
    float tilt = k->e[P_KTILT] * 2.f - 1.f, det = k->e[P_KDETUN];
    int choke = vsc.t[t][P_KDMODE] == RING_CHOKE;
    k->kwph += 0.05f * exp2a(k->e[P_KWRATE] * 9.f) * dt; k->kwph -= (float)floori(k->kwph);
    float nhz = 0.2f * exp2a(k->e[P_KNRATE] * 7.f);
    k->knph += nhz * dt;
    int renoise = k->knph >= 1.f; if (renoise) k->knph -= (float)floori(k->knph);
    float nsm = clampf(nhz * dt * 4.f, 0.f, 1.f);
    float bin[VSC_RING_BANDS], a1[VSC_RING_BANDS], wl[VSC_RING_BANDS], wr[VSC_RING_BANDS];
    int last = -1;
    /* the tuning only changes when what it is made from does: same values, made once */
    if (!k->kok || k->kmask != mask || k->krk[0] != semis0 || k->krk[1] != det || k->krk[2] != r) {
        for (unsigned b = 0; b < VSC_RING_BANDS; ++b) {
            float fs;
            if (mask >= 0x1000u) fs = ring_ratio_semis[(ksc - KSCALE_RATIOS) % 3u][b];
            else {
                int s = (int)b * 9; if (s <= last) s = last + 1;
                while (!(mask >> (s % 12) & 1)) ++s;
                last = s;
                fs = (float)s;
            }
            float sign = (b & 1) ? 1.f : -1.f;
            float semis = fs + semis0 + sign * det * 0.5f;     /* DETUNE: up to half a semitone apart */
            float hz = 65.406f * exp2a(semis * (1.f / 12.f));
            if (hz > 0.42f * (float)VSC_SR) hz = 0.42f * (float)VSC_SR;
            k->ka1[b] = 2.f * r * cos2pi(hz * (1.f / (float)VSC_SR));
            /* where this band sits on the cutoff's scale (20 Hz .. 20 kHz over 0..1) */
            k->ku[b] = (semis + 20.51f) * (1.f / 119.4f);                  /* log2(hz/20)/9.95 */
        }
        k->kok = 1; k->kmask = (uint16_t)mask; k->krk[0] = semis0; k->krk[1] = det; k->krk[2] = r;
    }
    for (unsigned b = 0; b < VSC_RING_BANDS; ++b) {
        float sign = (b & 1) ? 1.f : -1.f;
        a1[b] = k->ka1[b];
        float u = k->ku[b];
        vsc_meter.ring_x[t][b] = (uint8_t)clampf(u * 239.f, 0.f, 239.f);
        float d = (u - cut) / width, bp = 1.f / (1.f + d * d); bp *= bp;
        float lp = d <= 0.f ? 1.f : bp, hp = d >= 0.f ? 1.f : bp;
        float g = slope < 0.5f ? lp + (bp - lp) * slope * 2.f : bp + (hp - bp) * (slope - 0.5f) * 2.f;
        g *= 1.f + reso * 2.f * bp;
        float wp = k->kwph + (float)b * (1.f / (float)VSC_RING_BANDS); if (wp >= 1.f) wp -= 1.f;
        float wave = 0.5f + 0.5f * sin01(wp);
        g *= 1.f - waves + waves * wave;
        if (renoise || k->knt[b] == 0.f) { k->knt[b] = 0.05f + 0.95f * rndu(); if (k->knz[b] == 0.f) k->knz[b] = k->knt[b]; }
        k->knz[b] += (k->knt[b] - k->knz[b]) * nsm;
        g *= 1.f - noise + noise * k->knz[b];
        k->kgain[b] += (g - k->kgain[b]) * 0.3f;                       /* no zipper at block edges */
        g = k->kgain[b];
        float pan = clampf(0.5f + tilt * ((float)b * (1.f / (VSC_RING_BANDS - 1)) - 0.5f) + sign * det * 0.35f, 0.f, 1.f);
        float in_g = choke ? 1.f : g, out_g = choke ? g : 1.f;
        bin[b] = b0 * in_g;
        wl[b] = out_g * (1.f - pan); wr[b] = out_g * pan;
        vsc_meter.ring_amp[t][b] = (uint8_t)(clampf(g * (0.35f + 6.f * absf(k->ky1[b])), 0.f, 1.f) * 255.f);
    }
    float wet = k->e[P_FMIX], tone = k->e[P_KTONE] * 2.f - 1.f;
    int send = vsc.t[t][P_FWMODE] == WET_SEND;
    float makeup = (2.5f + reso * 2.f) * (1.f + decay * decay * 4.f);   /* long rings are narrow: give them level back */
    /* relief 2: every other band only (about 3 dB less of them, made up) */
    unsigned bstep = vsc_relief >= 2 ? 2u : 1u;
    if (bstep == 2u) makeup *= 1.41f;
    float x1 = k->kx1, x2 = k->kx2, tl = k->ktone;
    /* Band by band over the sub-block, each band's state in registers: the same sums
     * in the same order (band 0 first) as sample by sample, for far fewer loads and
     * stores. n is at most VSC_SUB. */
    float dxs[VSC_SUB], Ls[VSC_SUB], Rs[VSC_SUB];
    float kexc = k->e[P_KEXCITE];
    if (kexc > 0.f) {
        /* EXCITE: the listener's hits strike the bank (a short noise burst as strong as the
         * hit), and the track's own sound feeds it less, to none at full EXCITE */
        float hit = ana.hit[off / VSC_SUB < ANA_SUBS ? off / VSC_SUB : 0];
        if (hit > k->kburst) k->kburst = hit;
        /* a burst carries little energy into each narrow band: scaled so a full hit rings
         * near full level whatever DECAY is (b0 is the bands' input gain) */
        float dry = 1.f - kexc, strike = kexc * clampf(0.0125f / (b0 * makeup), 1.f, 40.f);
        for (unsigned i = 0; i < n; ++i) {
            float v0 = x[i] * dry + rndb() * k->kburst * strike;
            k->kburst *= 0.994f;                                          /* ~4 ms */
            dxs[i] = v0 - x2; x2 = x1; x1 = v0; Ls[i] = 0.f; Rs[i] = 0.f;
        }
        if (k->kburst < 1e-5f) k->kburst = 0.f;
    } else
        for (unsigned i = 0; i < n; ++i) { float v0 = x[i]; dxs[i] = v0 - x2; x2 = x1; x1 = v0; Ls[i] = 0.f; Rs[i] = 0.f; }
    /* the bands that speak: relief 2 keeps every other one; a band closed to the input
     * (SUSTAIN) whose output, made up, is under -100 dB and has rung out adds nothing and is left resting */
    uint8_t on[VSC_RING_BANDS]; unsigned non = 0;
    for (unsigned b = 0; b < VSC_RING_BANDS; b += bstep) {
        if (!choke && absf(k->kgain[b]) * makeup < 1e-5f && (absf(k->ky1[b]) + absf(k->ky2[b])) * makeup < 1e-5f) {
            k->ky1[b] = 0.f; k->ky2[b] = 0.f; continue;
        }
        on[non++] = (uint8_t)b;
    }
    unsigned j = 0;
    for (; j + 1u < non; j += 2u) {                          /* two bands a pass: half the sums' traffic */
        unsigned b = on[j], c = on[j + 1u];
        float p1 = k->ky1[b], p2 = k->ky2[b], pb = bin[b], pa = a1[b], pl = wl[b], pr = wr[b];
        float q1 = k->ky1[c], q2 = k->ky2[c], qb = bin[c], qa = a1[c], ql = wl[c], qr = wr[c];
        for (unsigned i = 0; i < n; ++i) {
            float dx = dxs[i];
            float y = pb * dx + pa * p1 - a2 * p2;
            p2 = p1; p1 = y;
            float z = qb * dx + qa * q1 - a2 * q2;
            q2 = q1; q1 = z;
            float L = Ls[i], R = Rs[i];
            L += y * pl; R += y * pr;
            L += z * ql; R += z * qr;
            Ls[i] = L; Rs[i] = R;
        }
        k->ky1[b] = p1; k->ky2[b] = p2; k->ky1[c] = q1; k->ky2[c] = q2;
    }
    if (j < non) {                                           /* an odd one left */
        unsigned b = on[j];
        float y1 = k->ky1[b], y2 = k->ky2[b], bb = bin[b], aa = a1[b], gl = wl[b], gr = wr[b];
        for (unsigned i = 0; i < n; ++i) {
            float y = bb * dxs[i] + aa * y1 - a2 * y2;
            y2 = y1; y1 = y;
            Ls[i] += y * gl; Rs[i] += y * gr;
        }
        k->ky1[b] = y1; k->ky2[b] = y2;
    }
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], L = Ls[i], R = Rs[i];
        float mid = (L + R) * makeup, dif = (L - R) * makeup;
        tl += (mid - tl) * 0.11f;                                        /* ~800 Hz */
        mid = tone < 0.f ? mid + (tl - mid) * -tone : mid - tl * tone;
        mid = tanha(mid); dif = tanha(dif) * 0.7f;
        x[i] = send ? v0 + mid * wet : v0 + (mid - v0) * wet;
        sd[i] = dif * wet;
    }
    k->kx1 = x1; k->kx2 = x2; k->ktone = tl;
    /* keep denormals and runaway out of the bank */
    for (unsigned b = 0; b < VSC_RING_BANDS; ++b) {
        if (absf(k->ky1[b]) < 1e-15f) k->ky1[b] = 0.f;
        if (absf(k->ky2[b]) < 1e-15f) k->ky2[b] = 0.f;
        k->ky1[b] = clampf(k->ky1[b], -8.f, 8.f); k->ky2[b] = clampf(k->ky2[b], -8.f, 8.f);
    }
}

/* --------------------------------------------------------------- REBUILD
 * DISSECT's resynthesis, in the FILTER slot: up to eight sine partials at the pitch the
 * listener hears, at its partials' levels. SHIFT moves them (semitones), STRTCH spreads
 * them apart (inharmonic), TILT gives more or fewer of the upper ones, DETUNE pulls odd
 * and even apart in pitch and across the stereo field. SMEAR is how slowly they follow;
 * UNVOIC what they do when no pitch is heard: HOLD the last, or FADE. SNAP puts the pitch
 * on the nearest semitone. WET blends with the track's own sound, TONE tilts as RESON's.
 * Each partial is a rotating phasor (four multiplies a frame, renormalised once a
 * sub-block); silent ones are skipped. */
static VSC_HOTDATA const float log2_h[8] = {0.f, 1.f, 1.5849625f, 2.f, 2.3219281f, 2.5849625f, 2.8073549f, 3.f};
static VSC_HOT_SMALL void rebuild(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off, *sd = k->side;
    /* the partials live in RESON's band state: ky1/ky2 the phasor, kgain the level,
     * ka1 the frequency, ku the level they head for */
    if (k->kmode != FT_REBUILD) {
        k->kmode = FT_REBUILD; k->kok = 0;
        for (unsigned b = 0; b < VSC_RING_BANDS; ++b) { k->ky1[b] = 1.f; k->ky2[b] = 0.f; k->kgain[b] = 0.f; k->ka1[b] = 0.f; k->ku[b] = 0.f; }
    }
    unsigned parts = (vsc.t[t][P_KSCALE] > 7u ? 7u : vsc.t[t][P_KSCALE]) + 1u;
    if (vsc_relief >= 2 && parts > 4u) parts = 4u;                 /* relief 2: four at most */
    float shift = k->e[P_KPITCH] * 127.f - 24.f;
    float stretch = 1.f + k->e[P_KWAVES] * 0.15f, tilt = (k->e[P_KTILT] * 2.f - 1.f) * 1.5f;
    float det = k->e[P_KDETUN], smear = k->e[P_KDECAY];
    float dt = (float)n * (1.f / (float)VSC_SR), tau = 0.004f + 2.f * smear * smear, c = dt / (tau + dt);
    int fade = vsc.t[t][P_KDMODE] == RING_CHOKE, voiced = ana.voiced;
    float note = ana.note;                                        /* held through silence */
    if (vsc.t[t][P_KFOLLOW] && note > 0.f) note = (float)floori(note + 0.5f);   /* SNAP */
    float f0 = note > 0.f ? 440.f * exp2a((note - 69.f + shift) * (1.f / 12.f)) : 0.f;
    float wet = k->e[P_FMIX], tone = k->e[P_KTONE] * 2.f - 1.f;
    int send = vsc.t[t][P_FWMODE] == WET_SEND;
    float Ls[VSC_SUB], Rs[VSC_SUB];
    for (unsigned i = 0; i < n; ++i) { Ls[i] = 0.f; Rs[i] = 0.f; }
    for (unsigned h = 0; h < VSC_RING_BANDS; ++h) {
        if (h >= parts) { k->kgain[h] = 0.f; vsc_meter.ring_amp[t][h] = 0; continue; }
        float sign = (h & 1u) ? 1.f : -1.f;
        float tf = k->ka1[h];
        if (f0 > 0.f) {
            tf = f0 * exp2a(log2_h[h] * stretch) * (1.f + sign * det * 0.006f);   /* DETUNE: up to 10 cents */
            if (voiced) k->ku[h] = vsc_meter.ana_harm[h] * exp2a(log2_h[h] * tilt);
        }
        float ta = fade && !voiced ? 0.f : k->ku[h];
        if (tf > 0.45f * (float)VSC_SR) ta = 0.f;
        float f = k->ka1[h] > 0.f ? k->ka1[h] + (tf - k->ka1[h]) * c : tf;
        k->ka1[h] = f;
        float a0 = k->kgain[h], a1 = a0 + (ta - a0) * c;
        if (a1 < 1e-7f) a1 = 0.f;                                 /* no denormals */
        k->kgain[h] = a1;
        float u = f > 20.f ? log2a(f * (1.f / 20.f)) * (1.f / 9.95f) : 0.f;
        vsc_meter.ring_x[t][h] = (uint8_t)clampf(u * 239.f, 0.f, 239.f);
        vsc_meter.ring_amp[t][h] = (uint8_t)(clampf(a1 * 4.f, 0.f, 1.f) * 255.f);
        float p = f * (1.f / (float)VSC_SR);
        if ((a0 < 1e-5f && a1 < 1e-5f) || p <= 0.f || p > 0.45f) continue;   /* silent: skipped */
        float cs = cos2pi(p), sn = cos2pi(absf(p - 0.25f));                  /* sin(2 pi p), p <= 0.5 */
        float re = k->ky1[h], im = k->ky2[h], a = a0, da = (a1 - a0) / (float)n;
        float pan = 0.5f + sign * det * 0.45f, gl = 1.f - pan, gr = pan;
        for (unsigned i = 0; i < n; ++i) {
            float r2 = re * cs - im * sn;
            im = re * sn + im * cs; re = r2;
            a += da;
            float y = im * a;
            Ls[i] += y * gl; Rs[i] += y * gr;
        }
        float m = 1.5f - 0.5f * (re * re + im * im);
        k->ky1[h] = re * m; k->ky2[h] = im * m;
    }
    float tl = k->ktone;
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], L = Ls[i], R = Rs[i];
        float mid = L + R, dif = L - R;
        tl += (mid - tl) * 0.11f;
        mid = tone < 0.f ? mid + (tl - mid) * -tone : mid - tl * tone;
        mid = tanha(mid); dif = tanha(dif) * 0.35f;
        x[i] = send ? v0 + mid * wet : v0 + (mid - v0) * wet;
        sd[i] = dif * wet;
    }
    k->ktone = tl;
}

static VSC_HOT void filter(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    unsigned type = vsc.t[t][P_FTYPE];
    for (unsigned i = 0; i < n; ++i) k->side[i] = 0.f;
    if (type == FT_OFF) return;
    if (type == FT_RING) { ring(t, off, n); return; }
    if (type == FT_REBUILD) { rebuild(t, off, n); return; }
    float *x = k->out + off;
    float mix = k->e[P_FMIX];
    int send = vsc.t[t][P_FWMODE] == WET_SEND;
    float cut = 20.f * exp2a(k->e[P_FCUT] * 9.95f);
    float res = k->e[P_FRES];
    if (type != FT_PEAKS) {
        float g = svf_g(cut), kk = 2.f - 1.96f * res;
        float a1 = 1.f / (1.f + g * (g + kk)), a2 = g * a1, a3 = g * a2;
        float ic1 = k->s1[0], ic2 = k->s2[0];
        for (unsigned i = 0; i < n; ++i) {
            float v0 = x[i];
            float v3 = v0 - ic2;
            float v1 = a1 * ic1 + a2 * v3;
            float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
            float y = type == FT_LP ? v2 : type == FT_BP ? v1 * kk : v0 - kk * v1 - v2;
            x[i] = send ? v0 + y * mix : v0 + (y - v0) * mix;
        }
        k->s1[0] = ic1; k->s2[0] = ic2;
        return;
    }
    /* PEAKS: four resonant bands on a scale */
    const int8_t *iv = scale_iv[vsc.t[t][P_FSCALE] & 7];
    float spread = k->e[P_FSPREAD] * 2.f;
    float kk = 0.25f - 0.245f * res;
    float a1[4], a2[4], a3[4];
    for (unsigned b = 0; b < 4; ++b) {
        float hz = cut * 0.25f * exp2a((float)iv[b] * spread * (1.f / 12.f));
        float g = svf_g(hz);
        a1[b] = 1.f / (1.f + g * (g + kk)); a2[b] = g * a1[b]; a3[b] = g * a2[b];
    }
    float norm = kk * 1.6f + 0.02f;
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], acc = 0.f;
        for (unsigned b = 0; b < 4; ++b) {
            float v3 = v0 - k->s2[b];
            float v1 = a1[b] * k->s1[b] + a2[b] * v3;
            float v2 = k->s2[b] + a2[b] * k->s1[b] + a3[b] * v3;
            k->s1[b] = 2.f * v1 - k->s1[b]; k->s2[b] = 2.f * v2 - k->s2[b];
            acc += v1;
        }
        acc = tanha(acc * norm * 2.f);
        x[i] = send ? v0 + acc * mix : v0 + (acc - v0) * mix;
    }
}

/* --------------------------------------------------------------- color
 * In the order noise -> drive -> crush -> compress, then a two-band TILT.
 * NOISE follows the input's envelope (N.DEC its fall) through a band-pass at N.TONE;
 * CRUSH reduces bits, sample rate (with jitter) or both; COMP is one knob for
 * threshold and ratio; TILT trades the bands either side of XOVER. */
static VSC_HOTDATA const float xover_c[3] = {0.0113f, 0.0884f, 0.3007f};   /* 80 Hz, 650 Hz, 2.5 kHz */
static VSC_HOT void color(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    float drive = k->e[P_DRIVE], comp = k->e[P_COMP], crush = k->e[P_CRUSH];
    float noise = k->e[P_NOISE], tilt = k->e[P_TILT] * 2.f - 1.f;
    float wet = k->e[P_CMIX];
    int send = vsc.t[t][P_CWMODE] == WET_SEND;
    if (drive < 0.002f && comp < 0.002f && crush < 0.002f && noise < 0.002f && tilt > -0.02f && tilt < 0.02f) return;
    float pre = 1.f + drive * drive * 24.f, post = 1.f / (1.f + drive * 2.5f);
    unsigned cmode = vsc.t[t][P_CMODE] % 3;
    float levels = exp2a(1.f + (1.f - crush) * 14.f);
    float hold_inc = 1.f / (1.f + crush * crush * 48.f);
    int do_bits = crush > 0.002f && cmode != 1, do_redux = crush > 0.002f && cmode != 0;
    unsigned ntype = vsc.t[t][P_NTYPE] % 3;
    float ndec = 0.0002f + 0.02f * (1.f - k->e[P_NDECAY]) * (1.f - k->e[P_NDECAY]);
    float ng = svf_g(80.f * exp2a(k->e[P_NTONE] * 7.5f)), nk = 1.2f;
    float na1 = 1.f / (1.f + ng * (ng + nk)), na2 = ng * na1, na3 = ng * na2;
    float xc = xover_c[vsc.t[t][P_XOVER] % 3];
    float n2 = noise * noise;
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], v = v0;
        if (noise > 0.002f) {
            float a = absf(v0);
            k->nz_env += (a - k->nz_env) * (a > k->nz_env ? 0.05f : ndec);
            float w;
            if (ntype == 2) { if (rndu() < n2 * 0.01f) k->dust = rndb(); w = k->dust; k->dust *= 0.5f; }
            else {
                w = rndb();
                if (ntype == 1) {   /* pink, three poles */
                    k->nz_p0 = 0.99765f * k->nz_p0 + w * 0.0990460f;
                    k->nz_p1 = 0.96300f * k->nz_p1 + w * 0.2965164f;
                    k->nz_p2 = 0.57000f * k->nz_p2 + w * 1.0526913f;
                    w = (k->nz_p0 + k->nz_p1 + k->nz_p2 + w * 0.1848f) * 0.25f;
                }
            }
            float v3 = w - k->nz_s2, v1 = na1 * k->nz_s1 + na2 * v3, v2 = k->nz_s2 + na2 * k->nz_s1 + na3 * v3;
            k->nz_s1 = 2.f * v1 - k->nz_s1; k->nz_s2 = 2.f * v2 - k->nz_s2;
            v += v1 * nk * n2 * 2.f * (0.05f + k->nz_env * 3.f);
        }
        if (drive > 0.002f) v = tanha(v * pre) * (1.f + drive) * post * 1.4f;
        if (do_redux) {
            k->hold_phase += hold_inc * (0.85f + 0.3f * rndu());
            if (k->hold_phase >= 1.f) { k->hold_phase -= 1.f; k->hold = v; }
            v = k->hold;
        }
        if (do_bits) v = (float)floori(v * levels + 0.5f) / levels;
        if (comp > 0.002f) {
            float a = absf(v);
            k->comp_env += (a - k->comp_env) * (a > k->comp_env ? 0.02f : 0.0005f);
            v *= (1.f + comp * 2.5f) / (1.f + comp * 10.f * k->comp_env);
        }
        k->tone_lp += (v - k->tone_lp) * xc;
        float hi = v - k->tone_lp;
        v = k->tone_lp * (1.f - tilt * 0.9f) + hi * (1.f + tilt * 1.5f);
        x[i] = send ? v0 + v * wet : v0 + (v - v0) * wet;
    }
}

/* MIX's per-track filter: centre is open, left sweeps a low-pass down, right a high-pass up */
static VSC_HOT void djfilter(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float f = k->e[P_DJF] * 2.f - 1.f;
    if (f > -0.02f && f < 0.02f) { k->djs1 = k->djs2 = 0.f; return; }
    float hz = f < 0.f ? 20000.f * exp2a(f * 9.f) : 20.f * exp2a(f * 9.f);
    float g = svf_g(hz), kk = 1.2f;
    float a1 = 1.f / (1.f + g * (g + kk)), a2 = g * a1, a3 = g * a2;
    float *x = k->out + off;
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], v3 = v0 - k->djs2, v1 = a1 * k->djs1 + a2 * v3, v2 = k->djs2 + a2 * k->djs1 + a3 * v3;
        k->djs1 = 2.f * v1 - k->djs1; k->djs2 = 2.f * v2 - k->djs2;
        x[i] = f < 0.f ? v2 : v0 - kk * v1 - v2;
    }
}

/* --------------------------------------------------------------- space */
static VSC_HOTDATA const float div_beats[9] = {0.125f, 1.f / 6.f, 0.25f, 1.f / 3.f, 0.375f, 0.5f, 2.f / 3.f, 0.75f, 1.f};
VSC_INL float rv_tap(unsigned bank, unsigned len, unsigned base, float in, float fbk, float damp)
{
    uint32_t i = rv_idx[bank];
    float y = (float)rv_buf[base + i] * (1.f / 32767.f);
    rv_lp[bank] += (y - rv_lp[bank]) * (1.f - damp);
    float w = clampf(in + rv_lp[bank] * fbk, -1.f, 1.f);
    rv_buf[base + i] = (int16_t)(w * 32767.f);
    if (++i >= len) i = 0;
    rv_idx[bank] = i;
    return y;
}
VSC_INL float rv_ap(unsigned bank, unsigned len, unsigned base, float in)
{
    uint32_t i = rv_idx[bank];
    float d = (float)rv_buf[base + i] * (1.f / 32767.f);
    float w = clampf(in + d * 0.5f, -1.f, 1.f);
    rv_buf[base + i] = (int16_t)(w * 32767.f);
    if (++i >= len) i = 0;
    rv_idx[bank] = i;
    return d - w * 0.5f;
}
static VSC_HOT void space(float *l, float *r, unsigned n)
{
    const uint8_t *g = vsc.g;
    unsigned long_mode = g[G_DLONG];
    float beats = div_beats[g[G_DTIME] % 9u] * 4.f;
    float frames = beats * 60.f / tempo_now * (float)VSC_SR;
    float cap = (float)(VSC_DELAY_FRAMES - 4) * (long_mode ? 2.f : 1.f);
    while (frames > cap) frames *= 0.5f;
    float stored = long_mode ? frames * 0.5f : frames;
    float fb = gv[G_DFEED] * (1.f / 127.f) * 0.95f;
    float dtone = (gv[G_DTONE] - 64.f) * (1.f / 64.f);
    float dlev = gv[G_DLEVEL] * (1.f / 127.f);
    float rfb = 0.70f + 0.28f * gv[G_RSIZE] * (1.f / 127.f);
    float damp = 0.05f + 0.6f * gv[G_RDAMP] * (1.f / 127.f);
    float rlev = gv[G_RLEVEL] * (1.f / 127.f);
    /* FREEZE: no new input, the delay recirculates and the reverb holds */
    float in_gain = 1.f;
    if (space_frozen) { in_gain = 0.f; fb = 1.f; rfb = 0.997f; damp = 0.05f; }
    /* the reverb rests at level 0, and under relief 1 (not while frozen: it is the held sound);
     * coming back it starts clean */
    static uint8_t rv_rest;
    int rv_on = space_frozen || (rlev > 0.002f && vsc_relief < 1);
    if (!rv_on) rv_rest = 1;
    else if (rv_rest) { rv_rest = 0; vsc_memset(rv_buf, 0, sizeof rv_buf); for (unsigned b = 0; b < 4; ++b) rv_lp[b] = 0.f; }
    for (unsigned i = 0; i < n; ++i) {
        float in = (space_l[i] + space_r[i]) * 0.5f * in_gain;
        /* delay */
        float rd = (float)dl_w - stored;
        while (rd < 0.f) rd += (float)VSC_DELAY_FRAMES;
        int ri = floori(rd); float fr = rd - (float)ri;
        uint32_t a = (uint32_t)ri % VSC_DELAY_FRAMES, b = (a + 1) % VSC_DELAY_FRAMES;
        float tap = ((float)delay_line[a] + ((float)delay_line[b] - (float)delay_line[a]) * fr) * (1.f / 32767.f);
        float rd2 = (float)dl_w - stored * 0.75f;
        while (rd2 < 0.f) rd2 += (float)VSC_DELAY_FRAMES;
        float tap2 = (float)delay_line[(uint32_t)floori(rd2) % VSC_DELAY_FRAMES] * (1.f / 32767.f);
        dl_lp += (tap - dl_lp) * (0.15f + 0.35f * (dtone + 1.f));
        float fbs = dtone < 0.f ? dl_lp : tap + (tap - dl_lp) * dtone * 0.5f;
        float w = tanha(in + fbs * fb);
        if (!long_mode || (dl_half ^= 1)) {
            delay_line[dl_w] = (int16_t)(clampf(w, -1.f, 1.f) * 32767.f);
            if (++dl_w >= VSC_DELAY_FRAMES) dl_w = 0;
        }
        if (!rv_on) { l[i] += tap * dlev; r[i] += tap2 * dlev; continue; }
        /* reverb, fed by the dry send and the delay */
        float rin = (in + tap * 0.4f) * 0.3f;
        unsigned o = 0;
        float c = rv_tap(0, RV_C0, o, rin, rfb, damp); o += RV_C0;
        c += rv_tap(1, RV_C1, o, rin, rfb, damp); o += RV_C1;
        float c2 = rv_tap(2, RV_C2, o, rin, rfb, damp); o += RV_C2;
        c2 += rv_tap(3, RV_C3, o, rin, rfb, damp); o += RV_C3;
        float sum = c + c2, diff = c - c2;
        float yl = rv_ap(4, RV_A0, o, sum); o += RV_A0;
        yl = rv_ap(5, RV_A1, o, yl); o += RV_A1;
        float yr = rv_ap(6, RV_A2, o, sum * 0.7f + diff * 0.3f); o += RV_A2;
        yr = rv_ap(7, RV_A3, o, yr);
        l[i] += tap * dlev + yl * rlev;
        r[i] += tap2 * dlev + yr * rlev;
    }
}

/* ------------------------------------------------------------ carom */
/* CAROM: notes become balls in a unit box. Each wall a ball hits fires its
 * note: top and bottom as it is, left and right shifted by the WALLS interval.
 * SPEED, GRAVTY (bipolar: down or up), LOSS (energy lost per bounce), SPREAD
 * (how far launch angles fan out), JITTER (random deflection at each wall),
 * QUANT (free, or held for the next 1/16 or 1/8), GATE (note length). */
struct ball { float x, y, vx, vy; uint8_t note, vel, on, pad; };
static struct ball balls[VSC_TRACKS][VSC_BALLS];
static uint8_t ball_next[VSC_TRACKS], ball_seeded[VSC_TRACKS];
struct pend { uint8_t note, vel; };
static struct pend pending[VSC_TRACKS][16];
static uint8_t pending_n[VSC_TRACKS];
struct release { uint8_t note, on; int32_t left; };
static struct release rel[VSC_TRACKS][12];
static VSC_HOTDATA const int8_t wall_iv[3] = {12, 0, 7};

static VSC_HOT void fire_note(unsigned t, unsigned note, unsigned vel)
{
    if (vsc.t[t][P_MUTE]) return;
    note_on(t, note, vel);
    float g = tr[t].e[P_RGATE];
    int32_t frames = (int32_t)((0.02f + g * g * 0.6f) * (float)VSC_SR);
    for (unsigned i = 0; i < 12; ++i)
        if (!rel[t][i].on) { rel[t][i].on = 1; rel[t][i].note = (uint8_t)note; rel[t][i].left = frames; return; }
    note_off(t, note);   /* no room to track it: make it a click */
}
static VSC_HOT void ball_launch(unsigned t, struct ball *b, unsigned k, float x, float y)
{
    /* the golden angle spaces successive launches; SPREAD fans them out */
    float spread = tr[t].e[P_RSPREAD];
    float ph = (float)k * 0.381966f * spread + 0.125f + rndu() * spread * 0.08f;
    ph -= (float)floori(ph);
    float c = sin01(ph + 0.25f), sn = sin01(ph);
    b->x = clampf(x, 0.02f, 0.98f); b->y = clampf(y, 0.02f, 0.98f);
    b->vx = c; b->vy = sn;
}
static VSC_HOT void ball_add(unsigned t, unsigned note, unsigned vel)
{
    unsigned max = balls_allowed(t);
    unsigned k = ball_next[t] % max;
    struct ball *b = &balls[t][k];
    b->note = (uint8_t)note; b->vel = (uint8_t)(vel ? vel : 100); b->on = 1;
    ball_launch(t, b, k + note, 0.5f, 0.5f);
    ball_next[t] = (uint8_t)((k + 1) % max);
}
static VSC_HOT void ball_seed(unsigned t)
{
    unsigned len = vsc.length[t] ? vsc.length[t] : 16, k = 0, max = balls_allowed(t);
    for (unsigned i = 0; i < VSC_BALLS; ++i) balls[t][i].on = 0;
    for (unsigned s = 0; s < len && k < max; ++s) {
        const struct vsc_step *st = &vsc.step[t][s];
        if (!st->on) continue;
        struct ball *b = &balls[t][k];
        b->note = st->note; b->vel = st->vel; b->on = 1;
        ball_launch(t, b, k, ((float)s + 0.5f) / (float)len, 1.f - (float)st->vel / 127.f);
        ++k;
    }
    ball_next[t] = (uint8_t)(k % max);
    ball_seeded[t] = 1;
}
static VSC_HOT void ball_hit(unsigned t, struct ball *b, int shift)
{
    int note = (int)b->note + shift;
    if (note < 0 || note > 127) note = b->note;
    if (vsc.t[t][P_RQUANT] && pending_n[t] < 16) {
        pending[t][pending_n[t]].note = (uint8_t)note; pending[t][pending_n[t]].vel = b->vel; ++pending_n[t];
    } else fire_note(t, (unsigned)note, b->vel);
    /* JITTER: a random turn at the wall */
    float j = tr[t].e[P_RJITTER] * rndb() * 0.9f;
    if (j != 0.f) {
        float c = 1.f - j * j * 0.5f, sn = j, vx = b->vx;
        b->vx = vx * c - b->vy * sn; b->vy = vx * sn + b->vy * c;
    }
}
static VSC_HOT void carom(unsigned t, unsigned n)
{
    struct track *k = &tr[t];
    if (!ball_seeded[t]) {
        int any = 0;
        for (unsigned i = 0; i < VSC_BALLS; ++i) any |= balls[t][i].on;
        if (!any) ball_seed(t); else ball_seeded[t] = 1;
    }
    float dt = (float)n * (1.f / (float)VSC_SR);
    float speed = 0.04f + k->e[P_RSPEED] * k->e[P_RSPEED] * 2.4f;     /* box widths per second */
    float grav = (k->e[P_RGRAV] - 0.5f) * 2.f * 2.5f;                  /* + pulls down */
    float keep = 1.f - k->e[P_RLOSS] * 0.6f;
    int iv = wall_iv[vsc.t[t][P_RWALLS] % 3];
    unsigned max = balls_allowed(t);
    for (unsigned i = 0; i < VSC_BALLS; ++i) {
        struct ball *b = &balls[t][i];
        if (!b->on || i >= max) { vsc_meter.ball_on[t][i] = 0; continue; }
        b->vy += grav * dt;
        b->x += b->vx * speed * dt;
        b->y += b->vy * speed * dt;
        /* a hit fires only if the ball still has some speed into the wall,
         * so a ball resting on the floor stays quiet */
        /* a wall fires only when the ball meets it with some speed, so a ball
         * that has come to rest stays quiet; the flash marks real hits */
        if (b->x < 0.f) { b->x = -b->x; if (b->vx < -0.05f) { ball_hit(t, b, -iv); vsc_meter.ball_hit[t][i] = 255; } b->vx = -b->vx * keep; }
        else if (b->x > 1.f) { b->x = 2.f - b->x; if (b->vx > 0.05f) { ball_hit(t, b, iv); vsc_meter.ball_hit[t][i] = 255; } b->vx = -b->vx * keep; }
        if (b->y < 0.f) { b->y = -b->y; if (b->vy < -0.05f) { ball_hit(t, b, 0); vsc_meter.ball_hit[t][i] = 255; } b->vy = -b->vy * keep; }
        else if (b->y > 1.f) { b->y = 2.f - b->y; if (b->vy > 0.05f) { ball_hit(t, b, 0); vsc_meter.ball_hit[t][i] = 255; } b->vy = -b->vy * keep; }
        /* under gravity a slow ball settles against the wall it is pressed into */
        if (grav > 0.f && b->y > 0.995f && b->vy > -0.06f && b->vy < 0.06f) { b->y = 1.f; b->vy = 0.f; }
        if (grav < 0.f && b->y < 0.005f && b->vy > -0.06f && b->vy < 0.06f) { b->y = 0.f; b->vy = 0.f; }
        b->x = clampf(b->x, 0.f, 1.f); b->y = clampf(b->y, 0.f, 1.f);
        vsc_meter.ball_on[t][i] = 1;
        vsc_meter.ball_x[t][i] = (uint8_t)(b->x * 255.f);
        vsc_meter.ball_y[t][i] = (uint8_t)(b->y * 255.f);
        if (vsc_meter.ball_hit[t][i] == 255) vsc_meter.ball_hit[t][i] = 254;
        else if (vsc_meter.ball_hit[t][i] > 6) vsc_meter.ball_hit[t][i] -= 6; else vsc_meter.ball_hit[t][i] = 0;
    }
}
static VSC_HOT void releases(unsigned t, unsigned n)
{
    for (unsigned i = 0; i < 12; ++i)
        if (rel[t][i].on && (rel[t][i].left -= (int32_t)n) <= 0) { rel[t][i].on = 0; note_off(t, rel[t][i].note); }
}

/* ----------------------------------------------------------- sequencer */
static VSC_HOT void fire_step(void)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        unsigned len = vsc.length[t] ? vsc.length[t] : 16;
        unsigned s = seq_step % len;
        vsc_meter.step[t] = (uint8_t)s;
        const struct vsc_step *st = &vsc.step[t][s];
        if (vsc.t[t][P_SEQM] == SEQ_CAROM) {
            /* quantised wall hits wait for the next 1/16 (or 1/8) */
            if (pending_n[t] && (vsc.t[t][P_RQUANT] == 1 || !(seq_step & 1))) {
                for (unsigned i = 0; i < pending_n[t]; ++i) fire_note(t, pending[t][i].note, pending[t][i].vel);
                pending_n[t] = 0;
            }
            continue;
        }
        for (unsigned p = 0; p < VSC_NP; ++p) vsc_lock[t][p] = 255;
        if (!st->on) continue;
        for (unsigned l = 0; l < 2; ++l) if (st->lp[l] < VSC_NP) vsc_lock[t][st->lp[l]] = st->lv[l];
        if (vsc.t[t][P_MUTE]) continue;
        if (tr[t].gate_left > 0) note_off(t, tr[t].gate_note);
        note_on(t, st->note, st->vel);
        tr[t].gate_note = st->note;
        float step_frames = 15.f / tempo_now * (float)VSC_SR;
        tr[t].gate_left = (int32_t)(step_frames * 0.5f);
    }
}
/* External clock: MIDI always, AUTO while clock is arriving, INT never. */
VSC_INL int follow_clock(void)
{
    unsigned c = vsc.g[G_CLOCK];
    return c == CLOCK_MIDI || (c == CLOCK_AUTO && vsc_meter.ext_clock);
}
static VSC_HOT void sequencer(unsigned n, unsigned sub)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        if (tr[t].gate_left > 0) {
            tr[t].gate_left -= (int32_t)n;
            if (tr[t].gate_left <= 0) { tr[t].gate_left = 0; note_off(t, tr[t].gate_note); }
        }
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        releases(t, n);
        if (seq_running && vsc.t[t][P_SEQM] == SEQ_CAROM) carom(t, n);
    }
    if (!seq_running) return;
    if (vsc.g[G_AHITS] == AHITS_STEPS) {   /* DISSECT HITS STEPS: the music's hits clock the steps */
        if (ana.hit[sub] > 0.f) { ++seq_step; metro_tick(); fire_step(); }
        return;
    }
    if (follow_clock()) { /* MIDI clock: steps fire on every sixth pulse */
        while (seq_ext_pending) { --seq_ext_pending; metro_tick(); fire_step(); ++seq_step; }
        return;
    }
    float step_frames = 15.f / tempo_now * (float)VSC_SR;
    float swing = gv[G_SWING] * (1.f / 127.f) * 0.5f * step_frames;
    float this_len = (seq_step & 1) ? step_frames - swing : step_frames + swing;
    seq_phase += (float)n;
    if (seq_phase >= this_len) {
        seq_phase -= this_len;
        ++seq_step;
        metro_tick();
        fire_step();
    }
}
/* METRO: a click on every beat, louder on the bar, straight to the output */
static float metro_env, metro_ph, metro_inc;
static VSC_HOT void metro_tick(void)
{
    if (!vsc_dev.metro || (seq_step & 3u)) return;
    metro_env = 1.f; metro_ph = 0.f;
    metro_inc = ((seq_step & 15u) == 0 ? 2400.f : 1600.f) / (float)VSC_SR;
}
static VSC_HOT void seq_start(int from_zero)
{
    if (from_zero) { seq_step = 0; seq_phase = 0.f; seq_clock_count = 0; }
    seq_running = 1;
    vsc_meter.playing = 1;
    if (from_zero) mods_restart();
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        if (vsc.t[t][P_MODE] == MODE_PLAY && !tr[t].running) note_on(t, 60, 100), note_off(t, 60);
    fire_step();
}
static VSC_HOT void seq_stop(void)
{
    seq_running = 0; seq_ext_pending = 0;
    vsc_meter.playing = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        if (tr[t].gate_left > 0) { note_off(t, tr[t].gate_note); tr[t].gate_left = 0; }
        for (unsigned i = 0; i < 12; ++i) if (rel[t][i].on) { rel[t][i].on = 0; note_off(t, rel[t][i].note); }
        pending_n[t] = 0;
        for (unsigned p = 0; p < VSC_NP; ++p) vsc_lock[t][p] = 255;
    }
}

/* -------------------------------------------------------------- events */
/* MIDI CC: a learned mapping first; otherwise the fixed map on the track's channel
 * (1-4), and the globals on channel 16. Values scale to each parameter's range. */
static VSC_HOTDATA const uint8_t cc_track[128] = {
    [7] = P_LEVEL + 1, [10] = P_PAN + 1, [11] = P_SEND + 1, [12] = P_DJF + 1,
    [14] = P_START + 1, [15] = P_LEN + 1, [16] = P_SPEED + 1, [17] = P_PITCH + 1, [18] = P_GAIN + 1, [19] = P_DUB + 1,
    [20] = P_GPITCH + 1, [21] = P_GDENS + 1, [22] = P_GSIZE + 1, [23] = P_GSHAPE + 1, [24] = P_GTIME + 1,
    [25] = P_GSPRAY + 1, [26] = P_GDETUNE + 1, [27] = P_GMIX + 1, [28] = P_GFDBK + 1, [29] = P_GREV + 1,
    [70] = P_FCUT + 1, [71] = P_FRES + 1, [72] = P_KDECAY + 1, [73] = P_KPITCH + 1, [74] = P_KSLOPE + 1,
    [75] = P_KTONE + 1, [76] = P_FMIX + 1, [77] = P_KWAVES + 1, [78] = P_KNOISE + 1, [79] = P_KTILT + 1,
    [80] = P_DRIVE + 1, [81] = P_COMP + 1, [82] = P_CRUSH + 1, [83] = P_TILT + 1, [84] = P_NOISE + 1,
    [85] = P_NDECAY + 1, [86] = P_NTONE + 1, [87] = P_CMIX + 1,
    [102] = P_MOD0 + 2 + 1, [103] = P_MOD0 + 12 + 1, [104] = P_MOD0 + 22 + 1, [105] = P_MOD0 + 32 + 1,  /* mod AMOUNTs */
};
static VSC_HOTDATA const uint8_t cc_global[128] = {
    [7] = G_MAIN + 1, [9] = G_MCOMP + 1, [20] = G_DLEVEL + 1, [22] = G_DFEED + 1, [23] = G_DTONE + 1,
    [24] = G_RLEVEL + 1, [25] = G_RSIZE + 1, [26] = G_RDAMP + 1, [27] = G_MORPH + 1,
    [28] = G_M0 + 1, [29] = G_M1 + 1, [30] = G_M2 + 1, [31] = G_M3 + 1,
    [102] = G_M4 + 1, [103] = G_M5 + 1, [104] = G_M6 + 1, [105] = G_M7 + 1, [106] = G_SWING + 1,
};
/* pickup state for learned controls; released when the sound changes underneath them */
static uint8_t learn_held[VSC_LEARN];
static int16_t learn_last[VSC_LEARN];
VSC_HOT void vsc_learn_release(void) { for (unsigned l = 0; l < VSC_LEARN; ++l) { learn_held[l] = 0; learn_last[l] = -1; } }
/* the parameter's value on the CC's 0..127 scale, -1 if it has none */
static VSC_HOT int cc_value_now(unsigned track, unsigned code)
{
    if (code < VSC_NP) {
        if (track >= VSC_TRACKS) return -1;
        unsigned max = code >= P_MOD0 && code < P_MOD0 + 4 * VSC_MOD_PARAMS ? 127u : vsc_tparam_max[code];
        return max ? (int)((vsc.t[track][code] * 127u + max / 2u) / max) : -1;
    }
    if (code >= VSC_GDEST && code < VSC_GDEST + VSC_NG) {
        unsigned g = code - VSC_GDEST, max = vsc_gparam_max[g];
        return max ? (int)((vsc.g[g] * 127u + max / 2u) / max) : -1;
    }
    return -1;
}
static VSC_HOT void set_from_cc(unsigned track, unsigned code, unsigned value)
{
    if (code < VSC_NP) {
        if (track >= VSC_TRACKS) return;
        unsigned max = code >= P_MOD0 && code < P_MOD0 + 4 * VSC_MOD_PARAMS ? 127u : vsc_tparam_max[code];
        vsc.t[track][code] = (uint8_t)((value * max + 63u) / 127u);
    } else if (code >= VSC_GDEST && code < VSC_GDEST + VSC_NG) {
        unsigned g = code - VSC_GDEST, max = vsc_gparam_max[g];
        vsc.g[g] = (uint8_t)((value * max + 63u) / 127u);
    }
}
static VSC_HOT void midi_cc(unsigned ch, unsigned cc, unsigned value)
{
    if (vsc_learn_armed) {
        struct vsc_learn *slot = 0;
        for (unsigned l = 0; l < VSC_LEARN; ++l) {
            struct vsc_learn *L = &vsc_dev.learn[l];
            if (L->param != 255 && L->ch == ch && L->cc == cc) { slot = L; break; }
            if (L->param == 255 && !slot) slot = L;
        }
        if (!slot) slot = &vsc_dev.learn[VSC_LEARN - 1];
        slot->ch = (uint8_t)ch; slot->cc = (uint8_t)cc; slot->track = vsc_learn_track; slot->param = vsc_learn_param;
        vsc_learn_armed = 0; ++vsc_learn_seen; ++vsc_dev_changed;
        learn_held[slot - vsc_dev.learn] = 1;          /* the control that was just moved has it */
    }
    int learned = 0;
    for (unsigned l = 0; l < VSC_LEARN; ++l) {
        const struct vsc_learn *L = &vsc_dev.learn[l];
        if (L->param == 255 || L->ch != ch || L->cc != cc) continue;
        learned = 1;
        /* pickup: a control takes hold once it reaches (or passes) the parameter's value,
         * so a page, project or scene change never makes a value jump */
        if (!learn_held[l]) {
            int now = cc_value_now(L->track, L->param);
            int prev = learn_last[l];
            if (now < 0 || (prev >= 0 && ((prev <= now && (int)value >= now) || (prev >= now && (int)value <= now))) ||
                ((int)value - now <= 1 && now - (int)value <= 1)) learn_held[l] = 1;
            learn_last[l] = (int16_t)value;
            if (!learn_held[l]) continue;
        }
        set_from_cc(L->track, L->param, value);
    }
    if (learned || cc >= 128) return;
    if (cc == 123 || cc == 120) { handle_panic(); return; }      /* all notes / sound off */
    if (ch < VSC_TRACKS && cc_track[cc]) set_from_cc(ch, cc_track[cc] - 1u, value);
    else if (ch == 15 && cc_global[cc]) set_from_cc(0, VSC_GDEST + cc_global[cc] - 1u, value);
}
static VSC_HOT void handle_panic(void)
{
    for (unsigned i = 0; i < VSC_TRACKS; ++i) {
        for (unsigned v = 0; v < VSC_KEYS_VOICES; ++v) tr[i].v[v].on = 0;
        tr[i].gates = 0; tr[i].env_stage = 3;
        for (unsigned m = 0; m < VSC_MODS; ++m) if (tr[i].ms[m].stage) tr[i].ms[m].stage = 4;
    }
}
static VSC_HOT void handle(uint32_t e)
{
    unsigned type = e & 15u, t = (e >> 4) & 15u, a = (e >> 8) & 255u, b = (e >> 16) & 255u;
    if (type == EV_CC) { midi_cc(t, a, b); return; }
    if (type == EV_PC) { if (a < VSC_SCENES) { vsc_scene_recall(a); vsc_learn_release(); } return; }
    if (t >= VSC_TRACKS) t = 0;
    /* transport from MIDI counts only when the clock setting allows it */
    int ext_ok = vsc.g[G_CLOCK] != CLOCK_INT;
    switch (type) {
    case EV_NOTE_ON: if (b) note_on(t, a, b); else note_off(t, a); break;
    case EV_NOTE_OFF: note_off(t, a); break;
    case EV_PLAY: if (seq_running) seq_stop(); else seq_start(1); break;
    case EV_STOP: if (!from_midi || ext_ok) seq_stop(); break;
    case EV_START: if (ext_ok) { seq_start(1); seq_ext_pending = 0; seq_clock_count = 0; } break;
    case EV_CONT: if (ext_ok) seq_start(0); break;
    case EV_CLOCK: {
        uint32_t gap = frame_counter - ext_prev_at;
        ext_prev_at = frame_counter;
        if (ext_last < (uint32_t)VSC_SR && gap > 0 && gap < (uint32_t)VSC_SR) {
            ext_interval = ext_interval > 0.f ? ext_interval + ((float)gap - ext_interval) * 0.08f : (float)gap;
        }
        ext_last = 0; vsc_meter.ext_clock = 1;
        if (follow_clock() && seq_running && ++seq_clock_count >= 6) { seq_clock_count = 0; if (seq_ext_pending < 4) ++seq_ext_pending; }
        break;
    }
    case EV_REC: loop_rec(t); break;
    case EV_BALL: ball_add(t, a, b); ball_seeded[t] = 1; break;
    case EV_BALL_CLEAR: for (unsigned i = 0; i < VSC_BALLS; ++i) balls[t][i].on = 0; ball_next[t] = 0; ball_seeded[t] = 0; pending_n[t] = 0; break;
    case EV_LOOP_CLEAR: tr[t].loop_state = 0; tr[t].loop_len = 0; tr[t].running = 0; break;
    case EV_PANIC: handle_panic(); break;
    case EV_ACTION:
        switch (a) {
        case ACT_SPACE_FREEZE: space_frozen ^= 1; vsc_meter.space_frozen = space_frozen; break;
        case ACT_SPACE_CLEAR:
            vsc_memset(delay_line, 0, sizeof delay_line); vsc_memset(rv_buf, 0, sizeof rv_buf);
            space_frozen = 0; vsc_meter.space_frozen = 0; break;
        case ACT_MOD_TRIG: { struct modst *ms = &tr[t].ms[b & 3];
            if (b & 0x80) { if (ms->stage && ms->stage < 4) { ms->stage = 4; ms->sp = 0.f; ms->from = ms->env; } }
            else { ms->stage = 1; ms->sp = 0.f; ms->from = ms->env; }
            break; }
        case ACT_TAPE_REWIND: tr[t].ipos = 0; tr[t].fpos = 0.f; break;
        default: break;
        }
        break;
    default: break;
    }
}
static VSC_HOT void drain(struct queue *q)
{
    while (q->r != q->w) {
        uint32_t e = q->d[q->r & (QN - 1)];
        __asm__ volatile("" ::: "memory");
        q->r = q->r + 1;
        handle(e);
    }
}

/* -------------------------------------------------------------- render */
void vsc_init(void)
{
    ulaw_build();
    vsc_device_defaults();
    vsc_learn_release();
    for (unsigned p = 0; p < VSC_NP; ++p) is_enum[p] = (vsc_tparam_info(0, p)->flags & VSC_PF_ENUM) != 0;
    for (unsigned ty = 0; ty < 4; ++ty)
        for (unsigned kk = 0; kk < VSC_MOD_PARAMS; ++kk) mod_enum[ty][kk] = (vsc_mod_info(ty, kk)->flags & VSC_PF_ENUM) != 0;
    for (unsigned p = 0; p < VSC_NP; ++p) vsc_tparam_max[p] = vsc_tparam_info(0, p)->max;
    for (unsigned g = 0; g < VSC_NG; ++g) vsc_gparam_max[g] = vsc_gparam_info(g)->max;
    vsc_memset(tr, 0, sizeof tr);
    vsc_memset(vsc_lock, 255, sizeof vsc_lock);
    vsc_memset(vsc_meter.overview_slot, 255, sizeof vsc_meter.overview_slot);
    vsc_memset(loop_pool, 0xff, sizeof loop_pool);
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        vsc_memset(grain_ring[t], 0xff, VSC_GRAIN_FRAMES);
        tr[t].rd.slot = -1; tr[t].rd.chunk = -1;
        tr[t].dir = 1;
    }
    vsc_memset(delay_line, 0, sizeof delay_line);
    vsc_memset(rv_buf, 0, sizeof rv_buf);
}
static VSC_HOT void input_for(unsigned t, const struct vsc_inputs *in)
{
    float *d = tr[t].in;
    unsigned sel = vsc.t[t][P_INPUT];
    if (sel >= IN_T1 && sel <= IN_T4) {
        const float *s = tr[sel - IN_T1].prev;
        for (unsigned i = 0; i < VSC_BLOCK; ++i) d[i] = s[i];
        return;
    }
    if (sel == IN_MAIN) { for (unsigned i = 0; i < VSC_BLOCK; ++i) d[i] = main_prev[i]; return; }
    const float *bt = (sel == IN_BT || sel == IN_BOTH) ? in->bt : 0;
    const float *usb = (sel == IN_USB || sel == IN_BOTH) ? in->usb : 0;
    for (unsigned i = 0; i < VSC_BLOCK; ++i) {
        float v = 0.f;
        if (bt) v += (bt[2 * i] + bt[2 * i + 1]) * 0.5f;
        if (usb) v += (usb[2 * i] + usb[2 * i + 1]) * 0.5f;
        d[i] = v;
    }
}
VSC_HOT void vsc_render(const struct vsc_inputs *in, float *out)
{
    from_midi = 0; drain(&q_ctrl);
    from_midi = 1; drain(&q_midi);
    from_midi = 0;
    /* the tempo: measured from MIDI clock while following it */
    if (follow_clock() && ext_interval > 0.f) tempo_now = clampf((float)VSC_SR * 60.f / (ext_interval * 24.f), 20.f, 400.f);
    else tempo_now = (float)vsc.tempo;
    vsc_meter.tempo = tempo_now;
    frame_counter += VSC_BLOCK;
    if (ext_last < 0x7fffffffu) ext_last += VSC_BLOCK;
    if (ext_last > VSC_SR) vsc_meter.ext_clock = 0;
    vsc_meter.bt_in = in->bt != 0;
    vsc_meter.usb_in = in->usb != 0;
    for (unsigned s = 0; s < 2; ++s) {
        const float *src = s ? in->usb : in->bt;
        for (unsigned c = 0; c < 2; ++c) {
            float pk = 0.f;
            if (src) for (unsigned i = 0; i < VSC_BLOCK; ++i) { float a = src[2 * i + c]; if (a < 0.f) a = -a; if (a > pk) pk = a; }
            float *m = &vsc_meter.in_peak[s][c];
            *m = pk > *m ? pk : *m * 0.97f;
        }
    }
    for (unsigned t = 0; t < VSC_TRACKS; ++t) input_for(t, in);
    for (unsigned i = 0; i < VSC_BLOCK; ++i) { space_l[i] = space_r[i] = 0.f; out[2 * i] = out[2 * i + 1] = 0.f; }
    {   /* DISSECT's listener, once for everyone (timed with the rest: the "+" part) */
        int want = ana_wanted();
        ana_block(in, want);
    }

    /* which tracks another track listens to (its INPUT): those never sleep for being muted */
    uint8_t heard[VSC_TRACKS] = {0};
    for (unsigned u = 0; u < VSC_TRACKS; ++u) {
        unsigned sel = vsc.t[u][P_INPUT];
        if (sel >= IN_T1 && sel <= IN_T4) heard[sel - IN_T1] = 1;
    }
    uint32_t part[VSC_PARTS] = {0};
    for (unsigned off = 0; off < VSC_BLOCK; off += VSC_SUB) {
        compute_globals();
        unsigned sub = off / VSC_SUB;
        float hit = ana.hit[sub];
        if (hit > 0.f && vsc.g[G_AHITS] == AHITS_BALLS) {
            /* HITS BALLS: each hit throws a ball into every CAROM track, at the pitch heard */
            float nh = ana_note_now();
            unsigned note = nh > 0.f ? (unsigned)clampf((float)floori(nh + 0.5f), 24.f, 96.f) : 60u;
            for (unsigned t = 0; t < VSC_TRACKS; ++t)
                if (vsc.t[t][P_SEQM] == SEQ_CAROM && !vsc.t[t][P_MUTE]) { ball_add(t, note, (unsigned)(40.f + hit * 87.f)); ball_seeded[t] = 1; }
        }
        sequencer(VSC_SUB, sub);
        for (unsigned t = 0; t < VSC_TRACKS; ++t) {
            struct track *k = &tr[t];
            uint32_t t0 = vsc_clock_now ? vsc_clock_now() : 0;
            /* asleep, the parameters only once a block (for the material) */
            if (k->awake || off == 0) compute_params(t);
            material(t, off, VSC_SUB);
            { float pk = 0.f; for (unsigned i = 0; i < VSC_SUB; ++i) { float a = absf(k->out[off + i]); if (a > pk) pk = a; } k->mat_pk = pk; }
            /* awake: the material sounds, frozen grains play on, or COLOR's noise (a source of its own) is up.
             * A track nobody can hear (muted, or no level and no send, and no track listening
             * to it) doesn't wake, and sleeps after its hold. */
            int quiet = (vsc.t[t][P_MUTE] || (k->e[P_LEVEL] <= 0.f && k->e[P_SEND] <= 0.f)) && !heard[t];
            /* DISSECT wakes a track whose devices play from the listener: REBUILD while a pitch
             * is heard, RESON's EXCITE and GRAIN's HITS on a hit */
            int lis = 0;
            if (vsc_meter.ana_on) {
                unsigned ft = vsc.t[t][P_FTYPE];
                lis = (ft == FT_REBUILD && ana.voiced) ||
                      (hit > 0.f && ((ft == FT_RING && vsc.t[t][P_KEXCITE]) || vsc.t[t][P_GTRIG] == GTRIG_HITS));
            }
            if (!quiet && (k->mat_pk > VSC_WAKE_LEVEL || vsc.t[t][P_GFREEZE] || vsc.t[t][P_NOISE] || lis)) k->awake = VSC_WAKE_HOLD;
#ifdef VSC_NO_SLEEP
            k->awake = VSC_WAKE_HOLD;             /* measuring: every track always awake */
#endif
            run_mods(t, off, VSC_SUB);
            if (!k->awake) {                      /* asleep: silent, nothing more to do */
                for (unsigned i = 0; i < VSC_SUB; ++i) { k->out[off + i] = 0.f; k->side[i] = 0.f; }
                if (vsc_clock_now) part[t] += vsc_clock_since(t0);
                continue;
            }
            grains(t, off, VSC_SUB);
            filter(t, off, VSC_SUB);
            color(t, off, VSC_SUB);
            djfilter(t, off, VSC_SUB);
            { float pk = 0.f;
              for (unsigned i = 0; i < VSC_SUB; ++i) { float a = absf(k->out[off + i]) + absf(k->side[i]); if (a > pk) pk = a; }
              if (pk > VSC_WAKE_LEVEL && !quiet) k->awake = VSC_WAKE_HOLD; else --k->awake; }
            float lev = k->e[P_LEVEL]; lev = lev * lev * 1.6f;
            if (vsc.t[t][P_MUTE]) lev = 0.f;
            float pan = k->e[P_PAN];
            float pl = sin01(0.25f + pan * 0.25f), pr = sin01(pan * 0.25f);
            float send = k->e[P_SEND] * k->e[P_SEND];
            for (unsigned i = 0; i < VSC_SUB; ++i) {
                float v = k->out[off + i] * lev, d = k->side[i] * lev;
                float l = v * pl + d, r = v * pr - d;
                out[2 * (off + i)] += l;
                out[2 * (off + i) + 1] += r;
                space_l[off + i] += l * send;
                space_r[off + i] += r * send;
            }
            if (vsc_clock_now) part[t] += vsc_clock_since(t0);
        }
    }
    uint32_t ts = vsc_clock_now ? vsc_clock_now() : 0;
    {
        static float l[VSC_BLOCK], r[VSC_BLOCK];   /* static (RAM): 1 KB is too much for the interrupt's stack */
        /* SPACE sleeps too: it runs while something is sent to it, frozen, or until its
         * echoes and tail have died away (about 4 s below -80 dB) */
        static uint16_t space_awake;
        float in_pk = 0.f;
        for (unsigned i = 0; i < VSC_BLOCK; ++i) { float a = absf(space_l[i]) + absf(space_r[i]); if (a > in_pk) in_pk = a; }
        if (in_pk > VSC_WAKE_LEVEL || space_frozen) space_awake = 1400u;   /* blocks: ~4 s */
#ifdef VSC_NO_SLEEP
        space_awake = 1400u;
#endif
        if (space_awake) {
            for (unsigned i = 0; i < VSC_BLOCK; ++i) { l[i] = 0.f; r[i] = 0.f; }
            space(l, r, VSC_BLOCK);
            float pk = 0.f;
            for (unsigned i = 0; i < VSC_BLOCK; ++i) {
                out[2 * i] += l[i]; out[2 * i + 1] += r[i];
                float a = absf(l[i]) + absf(r[i]); if (a > pk) pk = a;
            }
            if (pk > VSC_WAKE_LEVEL) space_awake = 1400u; else --space_awake;
        }
    }
    if (vsc_clock_now) {
        part[VSC_TRACKS] = vsc_clock_since(ts);
        for (unsigned i = 0; i < VSC_PARTS; ++i) vsc_meter.part_ticks[i] = part[i];
    }
    float target = gv[G_MAIN] * (1.f / 127.f);
    target *= target;
    float mp = 0.f;
    /* master COMP: one knob, fast attack, slow release, then the soft limiter */
    float mc = gv[G_MCOMP] * (1.f / 127.f);
    for (unsigned i = 0; i < VSC_BLOCK; ++i) {
        main_gain_s += (target - main_gain_s) * 0.01f;
        float gl = main_gain_s * 1.2f;
        if (mc > 0.002f) {
            float a = absf(out[2 * i]) > absf(out[2 * i + 1]) ? absf(out[2 * i]) : absf(out[2 * i + 1]);
            main_comp_env += (a - main_comp_env) * (a > main_comp_env ? 0.01f : 0.0003f);
            gl *= (1.f + mc * 2.f) / (1.f + mc * 8.f * main_comp_env);
        }
        float click = 0.f;
        if (metro_env > 0.0005f) {
            metro_ph += metro_inc; if (metro_ph >= 1.f) metro_ph -= 1.f;
            click = (metro_ph < 0.5f ? 0.3f : -0.3f) * metro_env; metro_env *= 0.9965f;   /* ~15 ms */
        }
        /* held to full scale: tanha can round a hair past 1 at its knee (the FM-1 clips there anyway) */
        float l = clampf(tanha(out[2 * i] * gl) + click, -1.f, 1.f), r = clampf(tanha(out[2 * i + 1] * gl) + click, -1.f, 1.f);
        out[2 * i] = l; out[2 * i + 1] = r;
        main_prev[i] = (l + r) * 0.5f;
        float a = absf(l) > absf(r) ? absf(l) : absf(r);
        vsc_scope[i] = (uint8_t)(a * 255.f);
        if (a > mp) mp = a;
    }
    vsc_meter.main_peak = mp > vsc_meter.main_peak ? mp : vsc_meter.main_peak * 0.97f;
    /* one column of one track's sample overview per block: the bank is only read here,
     * in the audio context, so the screen never touches the flash cache */
    {
        static unsigned ov_col, ov_track;
        unsigned t = ov_track, slot = vsc.t[t][P_SLOT];
        unsigned mode = vsc.t[t][P_MODE];
        if ((mode == MODE_PLAY || mode == MODE_KEYS) && slot < VSC_BANK_SLOTS && vsc_bank_frames(slot)) {
            if (vsc_meter.overview_slot[t] != slot) { vsc_meter.overview_slot[t] = (uint8_t)slot; ov_col = 0; }
            uint32_t frames = vsc_bank_frames(slot);
            uint32_t chunk = frames * ov_col / 240u / VSC_CHUNK;   /* 32-bit, constant divisors only */
            const int16_t *c = vsc_bank_chunk(slot, chunk);
            int pk = 0;
            if (c) {
                uint32_t n = frames - chunk * VSC_CHUNK; if (n > VSC_CHUNK) n = VSC_CHUNK;
                for (uint32_t i = 0; i < n; i += 4) { int v = c[i] < 0 ? -c[i] : c[i]; if (v > pk) pk = v; }
            }
            vsc_meter.overview[t][ov_col] = (uint8_t)(pk * 60 / 32768);
            if (++ov_col >= 240u) { ov_col = 0; ov_track = (ov_track + 1) % VSC_TRACKS; }
        } else { ov_col = 0; ov_track = (ov_track + 1) % VSC_TRACKS; }
    }
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        float pk = 0.f;
        for (unsigned i = 0; i < VSC_BLOCK; ++i) {
            float a = absf(tr[t].out[i]); if (a > pk) pk = a;
            tr[t].prev[i] = tr[t].out[i];
        }
        vsc_meter.peak[t] = pk > vsc_meter.peak[t] ? pk : vsc_meter.peak[t] * 0.97f;
        vsc_meter.loop_state[t] = tr[t].loop_state;
        unsigned mode = vsc.t[t][P_MODE];
        vsc_meter.pos_len[t] = mode == MODE_LOOP && tr[t].loop_state == 1 ? loop_capacity(t) : src_len(t);
        vsc_meter.pos_play[t] = mode == MODE_LOOP && tr[t].loop_state == 1 ? tr[t].rec_pos
                              : mode == MODE_KEYS ? tr[t].v[0].ipos : tr[t].ipos;
        vsc_meter.grain_write[t] = tr[t].gw;
        for (unsigned g = 0; g < VSC_GRAINS; ++g) {
            const struct grain *gr = &tr[t].g[g];
            vsc_meter.grain_pos[t][g] = gr->active ? (uint16_t)(gr->pos * (65535.f / (float)VSC_GRAIN_FRAMES)) : 0xffff;
            vsc_meter.grain_amp[t][g] = gr->active ? (uint8_t)(255.f * (float)gr->t / (float)(gr->len ? gr->len : 1)) : 0;
        }
    }
}

const uint8_t *vsc_loop_memory(unsigned t, uint32_t *frames, uint32_t *capacity)
{
    if (t >= VSC_TRACKS) t = 0;
    if (frames) *frames = tr[t].loop_state == 1 ? tr[t].rec_pos : tr[t].loop_len;
    if (capacity) *capacity = loop_capacity(t);
    return loop_pool;
}
const uint8_t *vsc_grain_ring(unsigned t) { return grain_ring[t < VSC_TRACKS ? t : 0]; }
uint32_t vsc_loop_base(unsigned t) { return loop_base(t); }
