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
static uint8_t capture[VSC_TRACKS][VSC_CAP_FRAMES];   /* each track's putty, 8-bit mu-law */
static uint8_t delay_line[VSC_DELAY_FRAMES];   /* mu-law, as the captures (RAM is short) */
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
/* a grain: HANDS' two overlapping ones, or a shard */
struct grain { float pos, inc, amp, pl, pr, k; uint32_t len, t; uint8_t active, shape; };
struct track {
    /* the capture: state, its length, where recording or overdub writes, the decimator */
    uint8_t cap_state; uint32_t cap_len, rec_pos, ring_w; float rec_phase;
    uint32_t bounce_len;        /* PING: the length the partner takes next (0: none) */
    struct reader rd;
    /* HANDS: the playhead (frames into the window), two grains, when the next starts */
    float ph; struct grain h[2]; int32_t hcount; uint8_t hnext;
    /* notes: KEYS' pitch from C4, the gate's level and whether a note holds it */
    float note_semis, genv; uint8_t gate, note_hit, clock_hit; int8_t jump;   /* jump: a slice to go to (-1: none) */
    /* SHARDS */
    struct grain g[VSC_GRAINS];
    uint8_t burst; int32_t bwait; float sphase;
    /* SPECTRA: its vocoder (-1: none) and blocks until its next frame */
    int8_t voc; uint8_t vblk;
    float side[VSC_SUB];        /* this sub-block's stereo difference (shards and HANDS spread) */
    /* color */
    float comp_env, hold, hold_phase, tone_lp, dust;
    /* mods: four slots of any type */
    struct modst {
        float ph, val, env, sp, from, held, hold_ph;
        uint8_t steps[16], dl[24];   /* RANDOM's steps, FOLLOW's delay line: 0..255 */
        uint8_t idx, stage, dlw, pad;
    } ms[VSC_MODS];
    float follow;               /* last FOLLOW-style level, for COLOR's noise */
    float mat_pk;               /* this sub-block's source peak, for FOLLOW */
    float fol_lp;               /* FOLLOW's input filter */
    uint8_t env_stage, gates;
    /* MIX's filter, COLOR's noise and redux */
    float djs1, djs2, nz_env, nz_p0, nz_p1, nz_p2, nz_s1, nz_s2;
    /* step gate */
    int32_t gate_left; uint8_t gate_note;
    /* computed parameters, 0..1 for continuous ones */
    float e[VSC_NP];
    float out[VSC_BLOCK];       /* this block's mono output (post color) */
    float prev[VSC_BLOCK];      /* last block's, for another track's INPUT and BOUNCE */
    float in[VSC_BLOCK];
    float pan_l, pan_r, send;
    uint16_t awake;             /* sub-blocks left before a silent track sleeps (see vsc_render) */
};
static struct track tr[VSC_TRACKS];
/* The platform's clock for the CPU readout (unset: no readout), and the guard's relief. */
uint32_t (*vsc_clock_now)(void);
uint32_t (*vsc_clock_since)(uint32_t since);
volatile uint8_t vsc_relief;
/* A track with nothing to play sleeps: only its source (and modulators) run, so a
 * capture, the reel, the input or a note wakes it at once. Awake, it stays so until its
 * output has been silent for about 1.5 s, so tails (shards, COLOR) finish. */
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
static VSC_HOT_SMALL void metro_tick(void);
volatile uint8_t vsc_learn_armed, vsc_learn_track, vsc_learn_param, vsc_learn_seen;
static uint8_t space_frozen;
static uint32_t frame_counter;

/* -------------------------------------------------------------- sources
 * What a track's putty reads: its capture (CAPTURE; THRU writes it as a ring the input
 * runs through), a splice of the reel, or a sample-bank slot. Positions are in the
 * source's own frames, as floats (exact to 2^24, far past any source here). */
struct srcv { const uint8_t *u8; float len, rate; int slot; uint32_t w; uint8_t ring; };
VSC_INL float rate_hz(unsigned r) { return r == RATE_HI ? 44100.f : r == RATE_MID ? 22050.f : 11025.f; }
static VSC_HOT int src_view(unsigned t, struct srcv *s)
{
    struct track *k = &tr[t];
    unsigned src = vsc.t[t][P_SRC];
    s->slot = -1; s->ring = 0; s->w = 0; s->u8 = 0; s->len = 0.f; s->rate = 44100.f;
    if (src == SRC_REEL) {
        unsigned r = vsc.t[t][P_REEL];
        if (r) {   /* a sample-bank slot (32 kHz, read through the decoded-chunk cache) */
            uint32_t f = vsc_bank_frames(r - 1u);
            if (!f) return 0;
            s->slot = (int)(r - 1u); s->len = (float)f; s->rate = (float)VSC_BANK_RATE;
            return 1;
        }
        const struct vsc_reel *ri = vsc_reel_index();
        const uint8_t *d = vsc_reel_data();
        if (!ri || !d || !ri->count) return 0;
        unsigned n = ri->count > VSC_SPLICES ? VSC_SPLICES : ri->count, sp = vsc.t[t][P_SPLICE];
        if (sp > n) sp = n;
        /* ALL: the reel from its first splice to its last, at the first one's rate */
        const struct vsc_splice *a = &ri->s[sp ? sp - 1u : 0], *b = &ri->s[sp ? sp - 1u : n - 1u];
        s->u8 = d + a->at; s->len = (float)(b->at + b->frames - a->at); s->rate = rate_hz(a->rate);
        return s->len > 0.f;
    }
    s->u8 = capture[t]; s->rate = rate_hz(vsc.t[t][P_RATE]);
    if (src == SRC_THRU) { s->ring = 1; s->len = (float)(VSC_CAP_FRAMES - 2048u); s->w = k->ring_w; return 1; }
    if (k->cap_state < 2) return 0;
    s->len = (float)k->cap_len;
    return 1;
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
/* the source at a position (wrapped into it), interpolated */
VSC_INL float src_at(struct track *k, const struct srcv *s, float pos)
{
    if (pos >= s->len || pos < 0.f) pos -= s->len * (float)floori(pos / s->len);
    uint32_t i = (uint32_t)pos;
    float f = pos - (float)i;
    uint32_t j = i + 1u;
    if ((float)j >= s->len) j = 0;
    float a, b;
    if (s->slot >= 0) { a = bank_at(&k->rd, s->slot, i); b = bank_at(&k->rd, s->slot, j); }
    else {
        if (s->ring) {   /* THRU: 0 is the oldest frame, behind the write head */
            i += s->w + 2048u; if (i >= VSC_CAP_FRAMES) i -= VSC_CAP_FRAMES; if (i >= VSC_CAP_FRAMES) i -= VSC_CAP_FRAMES;
            j += s->w + 2048u; if (j >= VSC_CAP_FRAMES) j -= VSC_CAP_FRAMES; if (j >= VSC_CAP_FRAMES) j -= VSC_CAP_FRAMES;
        }
        a = ulaw_table[s->u8[i]]; b = ulaw_table[s->u8[j]];
    }
    return a + (b - a) * f;
}

/* ------------------------------------------------------------- triggers */
static void note_on(unsigned t, unsigned note, unsigned vel);
static void note_off(unsigned t, unsigned note);
static VSC_HOT_SMALL void note_on(unsigned t, unsigned note, unsigned vel)
{
    (void)vel;
    if (t >= VSC_TRACKS) return;
    struct track *k = &tr[t];
    k->awake = VSC_WAKE_HOLD;
    k->gates++;
    k->env_stage = 1;
    for (unsigned m = 0; m < VSC_MODS; ++m)
        if ((vsc.t[t][P_MOD(m, 0)] & 3) == MT_ADSR) { k->ms[m].stage = 1; k->ms[m].sp = 0.f; k->ms[m].from = k->ms[m].env; }
    k->note_hit = 1;
    unsigned keys = vsc.t[t][P_KEYS];
    if (keys == KEYS_PITCH || keys == KEYS_PLAY) k->note_semis = (float)note - 60.f;
    if (keys == KEYS_PLAY) { k->ph = 0.f; k->hcount = 0; }
    if (keys == KEYS_SLICE) k->jump = (int8_t)((note + 128u - 53u) & 15u);   /* a key a sixteenth of the window, from F3 */
    if (keys >= KEYS_PLAY) k->gate = 1;
}
static VSC_HOT_SMALL void note_off(unsigned t, unsigned note)
{
    (void)note;
    struct track *k = &tr[t];
    if (k->gates) k->gates--;
    if (!k->gates) {
        k->env_stage = 3; k->gate = 0;
        for (unsigned m = 0; m < VSC_MODS; ++m)
            if ((vsc.t[t][P_MOD(m, 0)] & 3) == MT_ADSR && k->ms[m].stage && k->ms[m].stage < 4) {
                k->ms[m].stage = 4; k->ms[m].sp = 0.f; k->ms[m].from = k->ms[m].env;
            }
    }
}
/* REC: tap to start a capture, tap again to end it (the gesture sets its length; a full
 * buffer ends it); hold to overdub over what plays. A capture of the partner (BOUNCE)
 * can pass itself back: PING. */
static VSC_HOT_SMALL void cap_start(unsigned t, uint32_t fixed)
{
    struct track *k = &tr[t];
    k->cap_state = 1; k->rec_pos = 0; k->rec_phase = 0.f; k->bounce_len = fixed;
}
static VSC_HOT_SMALL void cap_end(unsigned t)
{
    struct track *k = &tr[t];
    k->cap_len = k->rec_pos < 256u ? 256u : k->rec_pos;
    k->cap_state = 2; k->ph = 0.f; k->hcount = 0; k->rec_pos = 0;
    /* PING: the partner takes this one back, as long */
    unsigned u = VSC_PARTNER(t);
    if (vsc.t[t][P_BOUNCE] == BOUNCE_PING && vsc.t[u][P_SRC] == SRC_CAPTURE && vsc.t[u][P_BOUNCE] != BOUNCE_OFF) {
        float r = rate_hz(vsc.t[u][P_RATE]) / rate_hz(vsc.t[t][P_RATE]);
        uint32_t n = (uint32_t)((float)k->cap_len * r);
        cap_start(u, n > VSC_CAP_FRAMES ? VSC_CAP_FRAMES : n);
    }
}
static VSC_HOT_SMALL void cap_rec(unsigned t, unsigned how)
{
    struct track *k = &tr[t];
    if (vsc.t[t][P_SRC] != SRC_CAPTURE) return;
    if (how == REC_DUB_ON) { if (k->cap_state == 2) { k->cap_state = 3; } return; }
    if (how == REC_DUB_OFF) { if (k->cap_state == 3) k->cap_state = 2; return; }
    if (k->cap_state == 1) cap_end(t);
    else if (k->cap_state == 3) k->cap_state = 2;
    else cap_start(t, 0);
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
volatile float vsc_robot_off[VSC_TRACKS][VSC_ROBOTS];
volatile uint8_t vsc_robot_param[VSC_TRACKS][VSC_ROBOTS] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
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
    for (unsigned r = 0; r < VSC_ROBOTS; ++r) {   /* robots: the UI's looped gestures */
        unsigned p = vsc_robot_param[t][r];
        if (p < P_MOD0 && !en[p]) { k->e[p] = clampf(k->e[p] + vsc_robot_off[t][r], 0.f, 1.f); }
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
        if (b[P_ENGINE] == ENG_SHARDS && b[P_TRIG] == TRIG_HIT) return 1;
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
static VSC_HOT_SMALL void mods_restart(void)
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

/* ------------------------------------------------------------- the putty
 * Every track plays its source through one of three engines, all reading the same
 * WINDOW (POS and SIZE of the source) with the same PLAYHEAD, which moves at the
 * source's own speed slowed or sped by STRETCH (the top: frozen) and runs backwards on
 * REVERSE. Pitch never follows the playhead's speed:
 *  - HANDS: two overlapping grains (GRAIN long, Hann, half a grain apart) start at the
 *    playhead and read at PITCH: time and pitch apart, Nebulae- and Morphagene-style.
 *  - SHARDS: fragments thrown around the playhead (SPRAY) with a window from smooth
 *    through reverse- and expodec to SHATTER (short, sharp, octaves), when the TRIG says.
 *  - SPECTRA: a 512-point phase vocoder. Its frames read the source at the playhead
 *    (resampled for PITCH and SHIFT), and keep each bin's phase moving by what it moved
 *    between two frames one hop apart, so a still playhead holds a sound as it is.
 *    BLUR smooths magnitudes over time, HOLD keeps their peaks, GATE keeps the loud
 *    bins only, TILT leans the spectrum, PHASE scatters phases, CROSS takes the
 *    partner's magnitudes (its playhead, its source) for this one's phases. */
VSC_INL float hann01(float ph) { return 0.5f - 0.5f * cos2pi(ph <= 0.5f ? ph : 1.f - ph); }
VSC_INL float playhead_speed(struct track *k, unsigned t, float rr)
{
    float v = k->e[P_STRETCH] * 127.f;
    if (v >= 126.5f || vsc.t[t][P_FREEZE]) return 0.f;
    float sp = rr * exp2a((50.f - v) * 0.04f);     /* 50: 1.00X; 0: 0.25X; 125: 8X */
    return vsc.t[t][P_REV] ? -sp : sp;
}
VSC_INL float wrapw(float x, float w)
{
    if (x >= w || x < 0.f) x -= w * (float)floori(x / w);
    return x;
}

/* ---- the source stage: records, overdubs and feeds the THRU ring; out gets what
 * should be heard while recording (the input) */
static VSC_HOT int source(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    const float *in = k->in + off;
    unsigned src = vsc.t[t][P_SRC];
    float gain = k->e[P_GAIN] * k->e[P_GAIN] * 2.f;
    unsigned step = vsc.t[t][P_RATE] == RATE_HI ? 1u : vsc.t[t][P_RATE] == RATE_MID ? 2u : 4u;
    float inv = step == 1u ? 1.f : step == 2u ? 0.5f : 0.25f;
    if (src == SRC_THRU) {
        if (vsc.t[t][P_FREEZE]) return 0;         /* frozen: the ring keeps what it has */
        for (unsigned i = 0; i < n; ++i) {
            k->rec_phase += in[i] * gain;
            if (++k->rec_pos >= step) {
                capture[t][k->ring_w] = ulaw_enc(k->rec_phase * inv);
                if (++k->ring_w >= VSC_CAP_FRAMES) k->ring_w = 0;
                k->rec_pos = 0; k->rec_phase = 0.f;
            }
        }
        return 0;
    }
    if (src != SRC_CAPTURE) return 0;
    if (k->cap_state == 1) {   /* recording: what comes in is heard as it goes down */
        uint32_t cap = k->bounce_len ? k->bounce_len : VSC_CAP_FRAMES;
        for (unsigned i = 0; i < n; ++i) {
            float v = in[i] * gain;
            x[i] = v;
            k->rec_phase += v;
            if (++k->ring_w >= step) {   /* ring_w counts the decimator's frames here */
                k->ring_w = 0;
                if (k->rec_pos < cap) capture[t][k->rec_pos++] = ulaw_enc(k->rec_phase * inv);
                k->rec_phase = 0.f;
            }
        }
        if (k->rec_pos >= cap) cap_end(t);
        return 1;
    }
    if (k->cap_state == 3 && k->cap_len) {   /* overdub: a head of its own, at real time */
        float sos = k->e[P_SOS];
        for (unsigned i = 0; i < n; ++i) {
            k->rec_phase += in[i] * gain;
            if (++k->ring_w >= step) {
                k->ring_w = 0;
                if (k->rec_pos >= k->cap_len) k->rec_pos = 0;
                uint8_t *c = &capture[t][k->rec_pos++];
                *c = ulaw_enc(ulaw_table[*c] * sos + k->rec_phase * inv);
                k->rec_phase = 0.f;
            }
        }
    }
    return 0;
}

/* ---- HANDS */
static VSC_HOT void hands(unsigned t, const struct srcv *s, float a, float wl, float *x, unsigned n)
{
    struct track *k = &tr[t];
    float rr = s->rate * (1.f / (float)VSC_SR);
    float speed = playhead_speed(k, t, rr);
    float semis = k->e[P_PITCH] * 127.f - 24.f + k->note_semis;
    float gi = exp2a(semis * (1.f / 12.f)) * rr * (vsc.t[t][P_REV] ? -1.f : 1.f);
    float gr = k->e[P_GRAIN];
    int32_t g_len = (int32_t)((0.01f + gr * gr * 0.25f) * (float)VSC_SR);
    if (g_len < 64) g_len = 64;
    float jit = k->e[P_JITTER] * (float)g_len * rr;
    float inv_len = 1.f / (float)g_len;
    for (unsigned i = 0; i < n; ++i) {
        if (--k->hcount <= 0) {
            struct grain *g = &k->h[k->hnext];
            k->hnext ^= 1u;
            g->pos = a + k->ph + rndb() * jit;
            g->inc = gi; g->t = 0; g->len = (uint32_t)g_len; g->active = 1;
            k->hcount = g_len / 2;
        }
        float acc = 0.f;
        for (unsigned v = 0; v < 2; ++v) {
            struct grain *g = &k->h[v];
            if (!g->active) continue;
            float w = hann01((float)g->t * inv_len);
            acc += src_at(k, s, g->pos) * w;
            g->pos += g->inc;
            if (++g->t >= g->len) g->active = 0;
        }
        x[i] = acc;
        k->ph = wrapw(k->ph + speed, wl);
    }
}

/* ---- SHARDS */
VSC_INL float expo(float ph, float sharp) { return ph < 0.01f ? ph * 100.f : exp2a(-(ph - 0.01f) * sharp); }
VSC_INL float shard_env(float ph, float shape)
{
    if (shape < 0.3333f) { float u = shape * 3.f; return hann01(ph) * (1.f - u) + expo(1.f - ph, 6.f) * u; }   /* smooth -> swell */
    if (shape < 0.6667f) { float u = (shape - 0.3333f) * 3.f; return expo(1.f - ph, 6.f) * (1.f - u) + expo(ph, 6.f) * u; }   /* swell -> expodec */
    return expo(ph, 6.f + (shape - 0.6667f) * 72.f);                                                                  /* expodec -> glass */
}
static VSC_HOT void shards(unsigned t, const struct srcv *s, float a, float wl, float *x, float *sd, unsigned n, unsigned sub)
{
    struct track *k = &tr[t];
    float rr = s->rate * (1.f / (float)VSC_SR);
    float speed = playhead_speed(k, t, rr);
    float shape = k->e[P_SHAPE], shatter = shape > 0.75f ? (shape - 0.75f) * 4.f : 0.f;
    float sz = k->e[P_SSIZE];
    uint32_t len = (uint32_t)((0.005f + sz * sz * 0.3f) * (float)VSC_SR * (1.f - 0.8f * shatter));
    if (len < 32u) len = 32u;
    float dens = k->e[P_DENS];
    float p_free = (0.5f + dens * dens * 60.f) * (1.f + 2.f * shatter) * (1.f / (float)VSC_SR);
    float semis0 = k->e[P_PITCH] * 127.f - 24.f + k->note_semis, spread = k->e[P_SPREAD] * 12.f;
    float spray = k->e[P_SPRAY] * wl * 0.5f, srev = k->e[P_SREV], ster = k->e[P_STEREO] * 0.5f;
    unsigned trig = vsc.t[t][P_TRIG];
    unsigned burst = 1u + (unsigned)(dens * 5.99f);
    if ((trig == TRIG_HIT && ana.hit[sub < ANA_SUBS ? sub : 0] > 0.f) || (trig == TRIG_NOTE && k->note_hit)) { k->burst = (uint8_t)burst; k->bwait = 0; }
    if (trig == TRIG_CLOCK && k->clock_hit && rndu() < 0.2f + dens * 0.8f) { k->burst = 1; k->bwait = 0; }
    k->note_hit = 0; k->clock_hit = 0;
    unsigned slots = vsc_relief >= 2 ? VSC_GRAINS / 2 : VSC_GRAINS;
    float inv_len = 64.f / (float)len;
    /* the window, as a table for this block (cheaper than its curve per sample) */
    float env[65];
    for (unsigned i = 0; i <= 64; ++i) env[i] = shard_env((float)i * (1.f / 64.f), shape);
    /* make-up: sparse, sharp shards come up to about the level of the source */
    float sharp = shape < 0.6667f ? 6.f : 6.f + (shape - 0.6667f) * 72.f;
    float epow = shape < 0.3333f ? 0.375f : 0.7213f / sharp;   /* mean of env^2 */
    float over = trig == TRIG_FREE ? p_free * (float)len : 0.5f + dens;
    if (over > (float)slots) over = (float)slots;
    float mk = over * epow; mk = mk > 0.04f ? 1.4f / sqrta(mk) : 5.f;
    if (mk > 4.f) mk = 4.f;
    if (mk < 1.f) mk = 1.f;
    for (unsigned i = 0; i < n; ++i) {
        int fire = 0;
        if (trig == TRIG_FREE) fire = rndu() < p_free;
        else if (k->burst && --k->bwait <= 0) { fire = 1; --k->burst; k->bwait = (int32_t)(len / 3u + 16u); }
        if (fire)
            for (unsigned gi = 0; gi < slots; ++gi) {
                struct grain *g = &k->g[gi];
                if (g->active) continue;
                float semis = semis0 + rndb() * spread;
                if (shatter > 0.f && rndu() < shatter * 0.5f) semis += rndu() < 0.5f ? 12.f : 24.f;
                float inc = exp2a(semis * (1.f / 12.f)) * rr;
                g->pos = a + k->ph + rndb() * spray;
                g->inc = rndu() < srev ? -inc : inc;
                g->len = len; g->t = 0; g->active = 1;
                float pan = 0.5f + rndb() * ster;
                g->pl = 1.f - pan; g->pr = pan;
                g->amp = 0.75f + 0.25f * rndu();
                break;
            }
        float l = 0.f, r = 0.f;
        for (unsigned gi = 0; gi < VSC_GRAINS; ++gi) {
            struct grain *g = &k->g[gi];
            if (!g->active) continue;
            float u = (float)g->t * inv_len; unsigned ui = (unsigned)u; if (ui > 63u) ui = 63u;
            float w = env[ui] + (env[ui + 1] - env[ui]) * (u - (float)ui);
            float v = src_at(k, s, g->pos) * w * g->amp;
            l += v * g->pl; r += v * g->pr;
            g->pos += g->inc;
            if (++g->t >= g->len) g->active = 0;
        }
        x[i] = (l + r) * 0.5f * mk;
        sd[i] = (l - r) * 0.5f * mk;
        k->ph = wrapw(k->ph + speed, wl);
    }
}

/* ---- SPECTRA: a real FFT of 512 through a complex one of 256 */
#define FN VSC_FFT
#define FH (VSC_FFT / 2)
static float cosq[FN / 4 + 1];                  /* cos(2 pi i / 512), i = 0..128 */
static uint8_t band_of_bin[FH];                 /* the screen's 48 bands */
static float rnd_unit[64];                      /* 32 unit vectors, for PHASE */
VSC_INL float c512(unsigned i)
{
    i &= FN - 1u;
    return i <= FN / 4 ? cosq[i] : i <= FN / 2 ? -cosq[FN / 2 - i] : i <= 3 * FN / 4 ? -cosq[i - FN / 2] : cosq[FN - i];
}
VSC_INL float s512(unsigned i) { return c512(i + 3u * FN / 4u); }
/* the sine window, sin(pi i / 512), from the same quarter table */
VSC_INL float swin(unsigned i)
{
    unsigned u = i < FN / 2 ? FN / 2 - i : i - FN / 2;   /* sin(pi i/512) = cos(pi u/512) */
    return (u & 1u) ? 0.5f * (cosq[u >> 1] + cosq[(u >> 1) + 1u]) : cosq[u >> 1];
}
static uint32_t voc_block;                      /* blocks rendered: which vocoder frames this one */
static VSC_HOT void cfft(float *z, int inv)   /* 256 complex, in place; inverse unscaled */
{
    for (unsigned i = 1, j = 0; i < FH; ++i) {
        unsigned bit = FH >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float a = z[2 * i], b = z[2 * i + 1]; z[2 * i] = z[2 * j]; z[2 * i + 1] = z[2 * j + 1]; z[2 * j] = a; z[2 * j + 1] = b; }
    }
    unsigned step = FN / 2;   /* twiddle stride in 512ths */
    for (unsigned len = 2; len <= FH; len <<= 1, step >>= 1) {
        unsigned half = len >> 1;
        for (unsigned k = 0; k < half; ++k) {
            float wr = c512(k * step), wi = inv ? s512(k * step) : -s512(k * step);
            for (unsigned i = k; i < FH; i += len) {
                unsigned j = i + half;
                float tr_ = wr * z[2 * j] - wi * z[2 * j + 1], ti = wr * z[2 * j + 1] + wi * z[2 * j];
                z[2 * j] = z[2 * i] - tr_; z[2 * j + 1] = z[2 * i + 1] - ti;
                z[2 * i] += tr_; z[2 * i + 1] += ti;
            }
        }
    }
}
/* 512 real samples in b -> bins 0..256 in place (b[0]: DC, b[1]: Nyquist; bin k at 2k, 2k+1) */
static VSC_HOT void rfft(float *b)
{
    cfft(b, 0);
    float z0r = b[0], z0i = b[1];
    b[0] = z0r + z0i; b[1] = z0r - z0i;
    for (unsigned k = 1; k < FH / 2; ++k) {
        unsigned m = FH - k;
        float ar = b[2 * k], ai = b[2 * k + 1], br = b[2 * m], bi = -b[2 * m + 1];   /* Z[k], conj Z[256-k] */
        float fer = (ar + br) * 0.5f, fei = (ai + bi) * 0.5f;
        float for_ = (ai - bi) * 0.5f, foi = -(ar - br) * 0.5f;                     /* (Z[k] - conj Z[m]) / 2i */
        float wr = c512(k), wi = -s512(k);
        float tr_ = wr * for_ - wi * foi, ti = wr * foi + wi * for_;
        b[2 * k] = fer + tr_; b[2 * k + 1] = fei + ti;
        /* X[256-k] = conj(Fe) - conj(W^k) conj(Fo) */
        b[2 * m] = fer - tr_; b[2 * m + 1] = -fei + ti;
    }
    b[FH + 1] = -b[FH + 1];   /* X[128] = conj Z[128] */
}
static VSC_HOT void irfft(float *b)
{
    float x0 = b[0], xn = b[1];
    b[0] = (x0 + xn) * 0.5f; b[1] = (x0 - xn) * 0.5f;
    for (unsigned k = 1; k < FH / 2; ++k) {
        unsigned m = FH - k;
        float ar = b[2 * k], ai = b[2 * k + 1], br = b[2 * m], bi = -b[2 * m + 1];   /* X[k], conj X[256-k] */
        float fer = (ar + br) * 0.5f, fei = (ai + bi) * 0.5f;
        float dr = (ar - br) * 0.5f, di = (ai - bi) * 0.5f;
        float wr = c512(k), wi = s512(k);                                             /* conj(W^k) */
        float for_ = dr * wr - di * wi, foi = dr * wi + di * wr;
        /* Z[k] = Fe + i Fo;  Z[256-k] = conj Fe + i conj Fo */
        b[2 * k] = fer - foi; b[2 * k + 1] = fei + for_;
        b[2 * m] = fer + foi; b[2 * m + 1] = -fei + for_;
    }
    b[FH + 1] = -b[FH + 1];
    cfft(b, 1);
    for (unsigned i = 0; i < FN; ++i) b[i] *= 1.f / (float)FH;
}
VSC_INL float rsqrta(float x)
{
    union { float f; uint32_t u; } v; v.f = x;
    v.u = 0x5f3759dfu - (v.u >> 1);
    float y = v.f;
    y = y * (1.5f - 0.5f * x * y * y);
    return y * (1.5f - 0.5f * x * y * y);
}
/* S: each bin's running phase as a unit vector, and ola the overlap-add, both kept in
 * 16 bits (RAM is short): S in 1/16384ths, ola in 1/8192ths (it never nears 4) */
struct vocoder { int8_t track; int16_t S[2 * FH]; float M[FH]; int16_t ola[FN]; };
#define S_ONE 16384.f
#define OLA_ONE 8192.f
VSC_INL int16_t s16(float v) { v = v > 32767.f ? 32767.f : v < -32767.f ? -32767.f : v; return (int16_t)(v + (v >= 0.f ? 0.5f : -0.5f)); }
static struct vocoder voc[VSC_VOCODERS];
static float fa[FN], fc[FN];                     /* this frame, the partner's */
static float fm[FH];                            /* this frame's magnitudes */
static VSC_HOT_SMALL int voc_get(unsigned t)
{
    struct track *k = &tr[t];
    if (k->voc >= 0 && voc[k->voc].track == (int8_t)t) return 1;
    for (unsigned v = 0; v < VSC_VOCODERS; ++v)
        if (voc[v].track < 0 || vsc.t[voc[v].track][P_ENGINE] != ENG_SPECTRA) {
            if (voc[v].track >= 0) tr[voc[v].track].voc = -1;
            voc[v].track = (int8_t)t; k->voc = (int8_t)v;
            for (unsigned i = 0; i < FH; ++i) { voc[v].S[2 * i] = (int16_t)S_ONE; voc[v].S[2 * i + 1] = 0; voc[v].M[i] = 0.f; }
            for (unsigned i = 0; i < FN; ++i) voc[v].ola[i] = 0;
            return 1;
        }
    return 0;
}
/* one frame of the source into b: windowed, read from pos (its middle at the playhead)
 * every r source frames */
static VSC_HOT void voc_frame(struct track *k, const struct srcv *s, float pos, float r, float *b)
{
    pos -= r * (float)(FN / 2);
    for (unsigned i = 0; i < FN; ++i) {
        b[i] = src_at(k, s, pos) * swin(i);
        pos += r;
    }
    rfft(b);
}
static VSC_HOT void spectra(unsigned t, const struct srcv *s, float a, float wl, float *x, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    struct vocoder *vo = &voc[k->voc];
    float rr = s->rate * (1.f / (float)VSC_SR);
    float speed = playhead_speed(k, t, rr);
    /* a frame every other block (hop 256, sine windows: they overlap-add to one), the two
     * vocoders on alternate blocks, so no block carries both */
    const unsigned hop = 2u * VSC_BLOCK;
    if (off == 0 && ((voc_block ^ (unsigned)k->voc) & 1u)) k->vblk = 1;
    else if (off == 0) {
        k->vblk = 0;
        float semis = k->e[P_PITCH] * 127.f - 24.f + k->e[P_SHIFT] * 127.f - 24.f + k->note_semis;
        float r = exp2a(semis * (1.f / 12.f)) * rr;
        float pos = a + k->ph;
        voc_frame(k, s, pos, r, fa);
        float cross = k->e[P_CROSS];
        unsigned u = VSC_PARTNER(t);
        struct srcv ps;
        int have_p = cross > 0.001f && vsc_relief < 2 && src_view(u, &ps);   /* eased: no CROSS */
        if (have_p) {
            float pa = vsc.t[u][P_POS] * (1.f / 127.f) * ps.len;
            voc_frame(&tr[u], &ps, pa + tr[u].ph, ps.rate * (1.f / (float)VSC_SR), fc);
        }
        float blur = k->e[P_BLUR], hold = k->e[P_HOLD], gate = k->e[P_GATE], ph_amt = k->e[P_SPHASE];
        float tilt = k->e[P_STILT] * 2.f - 1.f;
        float keep = 1.f - (1.f - blur) * (1.f - blur) * 0.98f - 0.02f;   /* blur 0: follows at once */
        if (blur <= 0.f) keep = 0.f;
        keep *= keep;                       /* (per 256 frames: the times of the old 128 hop) */
        float hdec = 0.9f + hold * 0.0999f; hdec *= hdec;
        float gthr = gate * gate * 0.25f;
        float fmax = 1e-6f;
        uint8_t spec[48];
        for (unsigned i = 0; i < 48; ++i) spec[i] = 0;
        fa[0] = fa[1] = 0.f;
        /* identity phase locking (Laroche and Dolson): each spectral peak keeps moving by
         * what it moved over the hop; the bins around it keep their place relative to it,
         * as the analysis had them, so a partial's bins stay together (no phasiness) */
        for (unsigned kb = 1; kb < FH; ++kb) { float m1s = fa[2 * kb] * fa[2 * kb] + fa[2 * kb + 1] * fa[2 * kb + 1]; fm[kb] = m1s * rsqrta(m1s + 1e-20f); }
        fm[0] = 0.f;
        uint8_t peaks[FH / 2]; unsigned np = 0;
        for (unsigned kb = 2; kb + 1 < FH && np < FH / 2; ++kb)
            if (fm[kb] > fm[kb - 1] && fm[kb] >= fm[kb + 1] && fm[kb] > 1e-7f) peaks[np++] = (uint8_t)kb;
        if (!np) peaks[np++] = 1;
        for (unsigned pi = 0; pi < np; ++pi) {
            unsigned p = peaks[pi];
            unsigned lo = pi ? (peaks[pi - 1] + p) / 2u + 1u : 1u, hi = pi + 1 < np ? (p + peaks[pi + 1]) / 2u : FH - 1u;
            float xr = fa[2 * p], xi = fa[2 * p + 1], i1 = fm[p] > 1e-12f ? 1.f / fm[p] : 0.f;
            /* the peak's true frequency, between bins, from its log magnitudes (a parabola);
             * its phase moves on by that over the hop: no second frame needed */
            float la = log2a(fm[p - 1] + 1e-12f), lb = log2a(fm[p] + 1e-12f), lc = log2a(fm[p + 1] + 1e-12f);
            float den = la - 2.f * lb + lc, d = den < -1e-6f ? 0.5f * (la - lc) / den : 0.f;
            d = clampf(d, -0.5f, 0.5f);
            float turn = ((float)p + d) * (float)hop * (1.f / (float)FN);   /* cycles over the hop */
            turn = (turn - (float)floori(turn)) * (float)FN;
            unsigned ti = (unsigned)turn; float tf = turn - (float)ti;
            float c0 = c512(ti), s0_ = s512(ti);
            float zr = c0 + (c512(ti + 1u) - c0) * tf, zi = s0_ + (s512(ti + 1u) - s0_) * tf;
            int16_t *S = &vo->S[2 * p];
            float s0 = (float)S[0] * (1.f / S_ONE), s1 = (float)S[1] * (1.f / S_ONE);
            float sr = s0 * zr - s1 * zi, si = s0 * zi + s1 * zr;
            if (ph_amt > 0.f && rndu() < ph_amt * 0.5f) { unsigned q = (rnd() & 31u) * 2u; sr = rnd_unit[q]; si = rnd_unit[q + 1]; }
            float nm = 1.5f - 0.5f * (sr * sr + si * si);
            sr *= nm; si *= nm; S[0] = s16(sr * S_ONE); S[1] = s16(si * S_ONE);
            float ur = xr * i1, ui = -xi * i1;   /* conj of the peak's unit phase */
            for (unsigned kb = lo; kb <= hi; ++kb) {
                if (kb == p) continue;
                float ar = fa[2 * kb], ai = fa[2 * kb + 1], ik = fm[kb] > 1e-12f ? 1.f / fm[kb] : 0.f;
                float rr2 = (ar * ur - ai * ui) * ik, ri = (ar * ui + ai * ur) * ik;   /* X[k] conj(X[p]) / |..| */
                if (ik == 0.f) { rr2 = 1.f; ri = 0.f; }
                vo->S[2 * kb] = s16((sr * rr2 - si * ri) * S_ONE); vo->S[2 * kb + 1] = s16((sr * ri + si * rr2) * S_ONE);
            }
        }
        for (unsigned kb = 1; kb < FH; ++kb) {
            const int16_t *S = &vo->S[2 * kb];
            float mag = fm[kb];
            if (have_p) {   /* CROSS: toward the partner's magnitude, through their geometric mean */
                float pr_ = fc[2 * kb], pi_ = fc[2 * kb + 1], ps2 = pr_ * pr_ + pi_ * pi_;
                float mp = ps2 * rsqrta(ps2 + 1e-20f);
                float gm = mag * mp; gm = gm * rsqrta(gm + 1e-20f);
                mag = cross < 0.5f ? mag + (gm - mag) * cross * 2.f : gm + (mp - gm) * (cross * 2.f - 1.f);
            }
            if (mag > fmax) fmax = mag;
            if (gate > 0.f && mag < gthr * vo->M[0]) mag = 0.f;   /* M[0]: the last frame's loudest bin */
            if (tilt != 0.f) { float g = 1.f + tilt * ((float)kb * (2.f / (float)FH) - 1.f); mag *= g > 0.f ? g : 0.f; }
            float m = vo->M[kb];
            float target = hold > 0.f && m * hdec > mag ? m * hdec : mag;
            m = target + (m - target) * keep;
            vo->M[kb] = m;
            fa[2 * kb] = (float)S[0] * (1.f / S_ONE) * m; fa[2 * kb + 1] = (float)S[1] * (1.f / S_ONE) * m;
            unsigned bnd = band_of_bin[kb];
            float lv = m * 8.f; uint8_t q = (uint8_t)(lv > 255.f ? 255.f : lv);
            if (q > spec[bnd]) spec[bnd] = q;
        }
        vo->M[0] = fmax;
        for (unsigned i = 0; i < 48; ++i) vsc_meter.spec[t][i] = spec[i];
        irfft(fa);
        float norm = 1.f;
        for (unsigned i = 0; i < FN - hop; ++i) vo->ola[i] = vo->ola[i + hop];
        for (unsigned i = FN - hop; i < FN; ++i) vo->ola[i] = 0;
        norm *= OLA_ONE;
        for (unsigned i = 0; i < FN; ++i) vo->ola[i] = s16((float)vo->ola[i] + fa[i] * swin(i) * norm);
        k->ph = wrapw(k->ph + speed * (float)hop, wl);
    }
    unsigned at = (unsigned)k->vblk * VSC_BLOCK + off;   /* this block's part of the hop */
    if (at + n > FN) at = 0;
    for (unsigned i = 0; i < n; ++i) x[i] = (float)vo->ola[at + i] * (1.f / OLA_ONE);
}

VSC_INL float svf_g(float hz)
{
    hz = clampf(hz, 16.f, 0.45f * (float)VSC_SR);
    return tanpre(3.14159265f * hz / (float)VSC_SR);
}
/* ---- the putty, per sub-block */
static VSC_HOT void putty(unsigned t, unsigned off, unsigned n, unsigned sub)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    for (unsigned i = 0; i < n; ++i) k->side[i] = 0.f;
    if (source(t, off, n)) { k->mat_pk = 1.f; return; }   /* recording: the input is heard */
    struct srcv s;
    if (!src_view(t, &s)) { for (unsigned i = 0; i < n; ++i) x[i] = 0.f; vsc_meter.src_len[t] = 0; return; }
    float a = k->e[P_POS] * s.len, wl = k->e[P_SIZE] * s.len;
    if (wl < 64.f) wl = 64.f;
    if (wl > s.len) wl = s.len;
    if (k->jump >= 0) { k->ph = (float)k->jump * (1.f / 16.f) * wl; k->jump = -1; k->hcount = 0; }
    k->ph = wrapw(k->ph, wl);
    unsigned eng = vsc.t[t][P_ENGINE];
    vsc_meter.spectra_on[t] = 0;
    if (eng == ENG_SPECTRA) {
        if (voc_get(t)) vsc_meter.spectra_on[t] = 1;
        else eng = ENG_HANDS;
    }
    if (eng == ENG_SHARDS) shards(t, &s, a, wl, x, k->side, n, sub);
    else if (eng == ENG_SPECTRA) spectra(t, &s, a, wl, x, off, n);
    else hands(t, &s, a, wl, x, n);
    k->note_hit = 0; k->clock_hit = 0;
    /* KEYS PLAY and SLICE: a note opens it (5 ms), letting go closes it (80 ms) */
    unsigned keys = vsc.t[t][P_KEYS];
    if (keys >= KEYS_PLAY) {
        float tg = k->gate ? 1.f : 0.f, rate = k->gate ? 0.0045f : 0.00028f;
        for (unsigned i = 0; i < n; ++i) { k->genv += (tg - k->genv) * rate * 10.f; x[i] *= k->genv; k->side[i] *= k->genv; }
    }
    float pk = 0.f;
    for (unsigned i = 0; i < n; ++i) { float v = absf(x[i]); if (v > pk) pk = v; }
    k->mat_pk = pk;
    /* the screen's view of it */
    float inv = 65535.f / s.len;
    vsc_meter.src_len[t] = (uint32_t)s.len;
    vsc_meter.src_rate[t] = s.rate;
    vsc_meter.win_a[t] = (uint16_t)clampf(a * inv, 0.f, 65535.f);
    vsc_meter.win_b[t] = (uint16_t)clampf(wrapw(a + wl, s.len + 1.f) * inv, 0.f, 65535.f);
    vsc_meter.head[t] = (uint16_t)clampf(wrapw(a + k->ph, s.len) * inv, 0.f, 65535.f);
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
        float da = ulaw_table[delay_line[a]], tap = da + (ulaw_table[delay_line[b]] - da) * fr;
        float rd2 = (float)dl_w - stored * 0.75f;
        while (rd2 < 0.f) rd2 += (float)VSC_DELAY_FRAMES;
        float tap2 = ulaw_table[delay_line[(uint32_t)floori(rd2) % VSC_DELAY_FRAMES]];
        dl_lp += (tap - dl_lp) * (0.15f + 0.35f * (dtone + 1.f));
        float fbs = dtone < 0.f ? dl_lp : tap + (tap - dl_lp) * dtone * 0.5f;
        float w = tanha(in + fbs * fb);
        if (!long_mode || (dl_half ^= 1)) {
            delay_line[dl_w] = ulaw_enc(w);
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

static VSC_HOT_SMALL void fire_note(unsigned t, unsigned note, unsigned vel)
{
    if (vsc.t[t][P_MUTE]) return;
    note_on(t, note, vel);
    float g = tr[t].e[P_RGATE];
    int32_t frames = (int32_t)((0.02f + g * g * 0.6f) * (float)VSC_SR);
    for (unsigned i = 0; i < 12; ++i)
        if (!rel[t][i].on) { rel[t][i].on = 1; rel[t][i].note = (uint8_t)note; rel[t][i].left = frames; return; }
    note_off(t, note);   /* no room to track it: make it a click */
}
static VSC_HOT_SMALL void ball_launch(unsigned t, struct ball *b, unsigned k, float x, float y)
{
    /* the golden angle spaces successive launches; SPREAD fans them out */
    float spread = tr[t].e[P_RSPREAD];
    float ph = (float)k * 0.381966f * spread + 0.125f + rndu() * spread * 0.08f;
    ph -= (float)floori(ph);
    float c = sin01(ph + 0.25f), sn = sin01(ph);
    b->x = clampf(x, 0.02f, 0.98f); b->y = clampf(y, 0.02f, 0.98f);
    b->vx = c; b->vy = sn;
}
static VSC_HOT_SMALL void ball_add(unsigned t, unsigned note, unsigned vel)
{
    unsigned max = balls_allowed(t);
    unsigned k = ball_next[t] % max;
    struct ball *b = &balls[t][k];
    b->note = (uint8_t)note; b->vel = (uint8_t)(vel ? vel : 100); b->on = 1;
    ball_launch(t, b, k + note, 0.5f, 0.5f);
    ball_next[t] = (uint8_t)((k + 1) % max);
}
static VSC_HOT_SMALL void ball_seed(unsigned t)
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
static VSC_HOT_SMALL void ball_hit(unsigned t, struct ball *b, int shift)
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
static VSC_HOT_SMALL void fire_step(void)
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
        tr[t].clock_hit = 1;   /* SHARDS TRIG CLOCK: every sixteenth */
        for (unsigned p = 0; p < VSC_NP; ++p) vsc_lock[t][p] = 255;
        if (!st->on) continue;
        /* PROB: the step may not play (its locks still hold) */
        if (st->prob < 100 && (rnd() & 127u) * 100u >= (unsigned)st->prob * 128u) { for (unsigned l = 0; l < 2; ++l) if (st->lp[l] < VSC_NP) vsc_lock[t][st->lp[l]] = st->lv[l]; continue; }
        for (unsigned l = 0; l < 2; ++l) if (st->lp[l] < VSC_NP) vsc_lock[t][st->lp[l]] = st->lv[l];
        if (vsc.t[t][P_MUTE]) continue;
        if (tr[t].gate_left > 0) note_off(t, tr[t].gate_note);
        note_on(t, st->note, st->vel);
        /* SLICE: the playhead jumps to that sixteenth of the window (the 856's fragments) */
        if (st->slice) tr[t].jump = (int8_t)(st->slice - 1u);
        tr[t].gate_note = st->note;
        float step_frames = 15.f / tempo_now * (float)VSC_SR;
        tr[t].gate_left = (int32_t)(step_frames * (float)(st->len ? st->len : 1u) * 0.125f);   /* LEN: eighths of a step */
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
static VSC_HOT_SMALL void metro_tick(void)
{
    if (!vsc_dev.metro || (seq_step & 3u)) return;
    metro_env = 1.f; metro_ph = 0.f;
    metro_inc = ((seq_step & 15u) == 0 ? 2400.f : 1600.f) / (float)VSC_SR;
}
static VSC_HOT_SMALL void seq_start(int from_zero)
{
    if (from_zero) { seq_step = 0; seq_phase = 0.f; seq_clock_count = 0; }
    seq_running = 1;
    vsc_meter.playing = 1;
    if (from_zero) mods_restart();
    fire_step();
}
static VSC_HOT_SMALL void seq_stop(void)
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
    [14] = P_POS + 1, [15] = P_SIZE + 1, [16] = P_STRETCH + 1, [17] = P_PITCH + 1, [18] = P_GAIN + 1, [19] = P_SOS + 1,
    [20] = P_SHAPE + 1, [21] = P_DENS + 1, [22] = P_SPRAY + 1, [23] = P_SSIZE + 1, [24] = P_SPREAD + 1,
    [25] = P_SREV + 1, [26] = P_STEREO + 1, [27] = P_GRAIN + 1, [28] = P_JITTER + 1,
    [70] = P_BLUR + 1, [71] = P_SHIFT + 1, [72] = P_CROSS + 1, [73] = P_GATE + 1, [74] = P_STILT + 1,
    [75] = P_HOLD + 1, [76] = P_SPHASE + 1,
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
static VSC_HOT_SMALL int cc_value_now(unsigned track, unsigned code)
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
static VSC_HOT_SMALL void set_from_cc(unsigned track, unsigned code, unsigned value)
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
static VSC_HOT_SMALL void midi_cc(unsigned ch, unsigned cc, unsigned value)
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
static VSC_HOT_SMALL void handle_panic(void)
{
    for (unsigned i = 0; i < VSC_TRACKS; ++i) {
        tr[i].gates = 0; tr[i].env_stage = 3; tr[i].gate = 0;
        for (unsigned m = 0; m < VSC_MODS; ++m) if (tr[i].ms[m].stage) tr[i].ms[m].stage = 4;
    }
}
static VSC_HOT_SMALL void handle(uint32_t e)
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
    case EV_REC: cap_rec(t, a); break;
    case EV_BALL: ball_add(t, a, b); ball_seeded[t] = 1; break;
    case EV_BALL_CLEAR: for (unsigned i = 0; i < VSC_BALLS; ++i) balls[t][i].on = 0; ball_next[t] = 0; ball_seeded[t] = 0; pending_n[t] = 0; break;
    case EV_LOOP_CLEAR: tr[t].cap_state = 0; tr[t].cap_len = 0; tr[t].rec_pos = 0; tr[t].ph = 0.f; break;
    case EV_PANIC: handle_panic(); break;
    case EV_ACTION:
        switch (a) {
        case ACT_SPACE_FREEZE: space_frozen ^= 1; vsc_meter.space_frozen = space_frozen; break;
        case ACT_SPACE_CLEAR:
            vsc_memset(delay_line, 0xff, sizeof delay_line); vsc_memset(rv_buf, 0, sizeof rv_buf);
            space_frozen = 0; vsc_meter.space_frozen = 0; break;
        case ACT_MOD_TRIG: { struct modst *ms = &tr[t].ms[b & 3];
            if (b & 0x80) { if (ms->stage && ms->stage < 4) { ms->stage = 4; ms->sp = 0.f; ms->from = ms->env; } }
            else { ms->stage = 1; ms->sp = 0.f; ms->from = ms->env; }
            break; }
        case ACT_TAPE_REWIND: tr[t].ph = 0.f; tr[t].hcount = 0; break;
        default: break;
        }
        break;
    default: break;
    }
}
static VSC_HOT_SMALL void drain(struct queue *q)
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
    vsc_memset(capture, 0xff, sizeof capture);   /* mu-law silence */
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        tr[t].rd.slot = -1; tr[t].rd.chunk = -1;
        tr[t].voc = -1; tr[t].jump = -1;
    }
    for (unsigned v = 0; v < VSC_VOCODERS; ++v) voc[v].track = -1;
    for (unsigned i = 0; i <= FN / 4; ++i) {   /* cos 0 .. pi/2, by the cosine series (cos2pi is accurate to 1e-6 here) */
        cosq[i] = cos2pi((float)i * (1.f / (float)FN));
    }
    for (unsigned k = 0; k < FH; ++k) {   /* 48 bands over 256 bins, finer low */
        float b = (float)k; unsigned q = 0;
        while ((float)((q + 1) * (q + 1)) * (256.f / (48.f * 48.f)) <= b && q < 47u) ++q;
        band_of_bin[k] = (uint8_t)q;
    }
    for (unsigned i = 0; i < 32; ++i) { rnd_unit[2 * i] = c512(i * 16u + 5u); rnd_unit[2 * i + 1] = s512(i * 16u + 5u); }
    vsc_memset(delay_line, 0xff, sizeof delay_line);
    vsc_memset(rv_buf, 0, sizeof rv_buf);
#ifdef VSC_BENCH
    for (unsigned t = 0; t < VSC_TRACKS; ++t)   /* (the benchmark's material, before the audio starts) */
        for (unsigned i = 0; i < VSC_CAP_FRAMES; ++i) capture[t][i] = ulaw_enc(0.4f * (float)((int)(rnd() >> 24) - 128) * (1.f / 128.f) * (float)((i >> 6) & 7) * 0.14f);
#endif
}
static VSC_HOT void input_for(unsigned t, const struct vsc_inputs *in)
{
    float *d = tr[t].in;
    unsigned sel = vsc.t[t][P_INPUT];
    if (vsc.t[t][P_BOUNCE] != BOUNCE_OFF) sel = IN_T1 + VSC_PARTNER(t);   /* BOUNCE: the partner */
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
#ifdef VSC_BENCH
/* a CPU benchmark build only: every track awake and busy (two SPECTRA crossing, dense
 * SHARDS, HANDS), SPACE on */
void vsc_bench_setup(void)
{
#if VSC_BENCH == 2
    static const uint8_t eng[VSC_TRACKS] = {ENG_HANDS, ENG_HANDS, ENG_SHARDS, ENG_HANDS};
#elif VSC_BENCH == 3
    static const uint8_t eng[VSC_TRACKS] = {ENG_SPECTRA, ENG_HANDS, ENG_SHARDS, ENG_HANDS};
#else
    static const uint8_t eng[VSC_TRACKS] = {ENG_SPECTRA, ENG_SPECTRA, ENG_SHARDS, ENG_HANDS};
#endif
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        tr[t].cap_len = VSC_CAP_FRAMES; tr[t].cap_state = 2;
        vsc.t[t][P_SRC] = SRC_CAPTURE; vsc.t[t][P_BOUNCE] = BOUNCE_OFF; vsc.t[t][P_MUTE] = 0;
        vsc.t[t][P_ENGINE] = eng[t]; vsc.t[t][P_SEND] = 64; vsc.t[t][P_DRIVE] = 40;
    }
    vsc.t[1][P_CROSS] = 64; vsc.t[0][P_BLUR] = 40; vsc.t[2][P_DENS] = 127; vsc.t[2][P_SSIZE] = 127;
    vsc.g[G_RLEVEL] = 100; vsc.g[G_DLEVEL] = 100;
}
#endif
VSC_HOT void vsc_render(const struct vsc_inputs *in, float *out)
{
    ++voc_block;

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
        if (vsc.t[u][P_BOUNCE] != BOUNCE_OFF) sel = IN_T1 + VSC_PARTNER(u);
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
            /* asleep, the parameters only once a block (for the source) */
            if (k->awake || off == 0) compute_params(t);
            /* awake while it has something to play (a capture, being recorded, the reel, the
             * input through THRU), or COLOR's noise (a source of its own) is up. A track
             * nobody can hear (muted, or no level and no send, and no track listening to
             * it) doesn't wake, and sleeps after its hold. */
            int quiet = (vsc.t[t][P_MUTE] || (k->e[P_LEVEL] <= 0.f && k->e[P_SEND] <= 0.f)) && !heard[t];
            unsigned src = vsc.t[t][P_SRC];
            int has = src != SRC_CAPTURE || k->cap_state != 0;
            if (!quiet && (has || vsc.t[t][P_NOISE])) k->awake = VSC_WAKE_HOLD;
#ifdef VSC_NO_SLEEP
            k->awake = VSC_WAKE_HOLD;             /* measuring: every track always awake */
#endif
            run_mods(t, off, VSC_SUB);
            if (!k->awake) {                      /* asleep: what it records still goes down, silently */
                (void)source(t, off, VSC_SUB);
                for (unsigned i = 0; i < VSC_SUB; ++i) { k->out[off + i] = 0.f; k->side[i] = 0.f; }
                k->mat_pk = 0.f;
                if (vsc_clock_now) part[t] += vsc_clock_since(t0);
                continue;
            }
            putty(t, off, VSC_SUB, sub);
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
    /* one column of one track's source overview per block: sources are only read here,
     * in the audio context, so the screen never touches the flash cache */
    {
        static unsigned ov_col, ov_track;
        unsigned t = ov_track;
        struct srcv sv;
        if (src_view(t, &sv)) {
            float span = sv.len * (1.f / 240.f), p0 = span * (float)ov_col, st = span * (1.f / 16.f);
            float pk = 0.f;
            for (unsigned i = 0; i < 16; ++i) { float v = absf(src_at(&tr[t], &sv, p0 + st * (float)i)); if (v > pk) pk = v; }
            vsc_meter.overview[t][ov_col] = (uint8_t)(pk > 1.f ? 60.f : pk * 60.f);
        } else vsc_meter.overview[t][ov_col] = 0;
        if (++ov_col >= 240u) { ov_col = 0; ov_track = (ov_track + 1) % VSC_TRACKS; }
    }
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        float pk = 0.f;
        for (unsigned i = 0; i < VSC_BLOCK; ++i) {
            float a = absf(tr[t].out[i]); if (a > pk) pk = a;
            tr[t].prev[i] = tr[t].out[i];
        }
        vsc_meter.peak[t] = pk > vsc_meter.peak[t] ? pk : vsc_meter.peak[t] * 0.97f;
        struct track *k = &tr[t];
        vsc_meter.cap_state[t] = vsc.t[t][P_SRC] == SRC_CAPTURE ? k->cap_state : vsc.t[t][P_SRC] == SRC_THRU ? 2 : 0;
        uint32_t cap = k->bounce_len ? k->bounce_len : VSC_CAP_FRAMES;
        vsc_meter.src_cap[t] = cap;
        vsc_meter.rec_head[t] = k->cap_state == 1 ? (uint16_t)((float)k->rec_pos * 65535.f / (float)cap)
                              : k->cap_state == 3 && k->cap_len ? (uint16_t)((float)k->rec_pos * 65535.f / (float)k->cap_len) : 0;
        if (vsc.t[t][P_SRC] == SRC_CAPTURE && k->cap_state == 1) vsc_meter.src_len[t] = k->rec_pos;
        for (unsigned g = 0; g < VSC_GRAINS; ++g) {
            const struct grain *gr = &k->g[g];
            float sl = vsc_meter.src_len[t] ? (float)vsc_meter.src_len[t] : 1.f;
            float ps = gr->pos >= 0.f ? gr->pos : 0.f;
            if (ps >= sl) ps -= sl * (float)floori(ps / sl);
            vsc_meter.shard_pos[t][g] = gr->active && vsc.t[t][P_ENGINE] == ENG_SHARDS ? (uint16_t)clampf(ps * 65535.f / sl, 0.f, 65534.f) : 0xffff;
            vsc_meter.shard_amp[t][g] = gr->active ? (uint8_t)(255.f * (float)gr->t / (float)(gr->len ? gr->len : 1)) : 0;
        }
    }
}

const uint8_t *vsc_capture(unsigned t) { return capture[t < VSC_TRACKS ? t : 0]; }
