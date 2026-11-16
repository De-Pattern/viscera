/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: the audio engine. A four-track tape in RAM (4-bit ADPCM, varispeed),
 * the input's preamp, four channel strips with a 424-style preamp each, a pitch
 * shifter on the tape's output, bounces through RESON to a free track, two robots and
 * MIDI learn. Runs in the audio interrupt: no allocation, no C library. */
#include "vsc.h"

struct vsc_project vsc;
struct vsc_meter vsc_meter;
uint32_t (*vsc_clock_now)(void);
uint32_t (*vsc_clock_since)(uint32_t since);
volatile uint8_t vsc_relief;
volatile uint8_t vsc_learn_armed, vsc_learn_kind, vsc_learn_param, vsc_learn_seen;

/* ---------------------------------------------------------------- maths */
VSC_INL float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
VSC_INL float absf(float x) { return x < 0.f ? -x : x; }
VSC_INL int floori(float x) { int i = (int)x; return (float)i > x ? i - 1 : i; }
/* 2^x, about 1e-4 relative error over the audio range. */
static VSC_HOT_SMALL float exp2a(float x)
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
/* sin(2*pi*p) for p in [0,1), near enough for a robot */
static VSC_HOT_SMALL float sin01(float p)
{
    float x = p < 0.5f ? p * 4.f - 1.f : 3.f - p * 4.f; /* triangle -1..1 */
    return -x * (1.5f - 0.5f * x * x);
}
/* cos(2 pi p) for p in 0..0.5, accurate enough to tune a resonator */
VSC_INL float cos2pi(float p)
{
    float x = p * 6.28318531f, flip = 1.f;
    if (x > 1.57079633f) { x = 3.14159265f - x; flip = -1.f; }
    float x2 = x * x;
    return flip * (1.f + x2 * (-0.5f + x2 * (1.f / 24.f + x2 * (-1.f / 720.f + x2 * (1.f / 40320.f)))));
}
static VSC_HOT_SMALL float cos2pi_c(float p) { return cos2pi(p); }
VSC_INL float tanpre(float x) { float x2 = x * x; return x * (15.f - x2) / (15.f - 6.f * x2); }
/* log2(x) for x > 0, about 2e-4 absolute error */
static VSC_HOT_SMALL float log2a(float x)
{
    union { float f; uint32_t u; } v; v.f = x;
    float e = (float)(int)((v.u >> 23) & 255u) - 127.f;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;   /* mantissa in [1,2) */
    float m = v.f;
    return e + ((((-0.07915816f * m + 0.62887362f) * m - 2.08121416f) * m + 4.02854794f) * m - 2.49684606f);
}
static VSC_HOT_SMALL float sqrta(float x)
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
static VSC_HOT_SMALL float rndc(void) { return rndu(); }   /* the same, called: for code that runs now and then */

/* mu-law, 8-bit, for RESON's droplets. The decode table is built at init into RAM. */
static float ulaw_table[256];
static VSC_HOT_SMALL uint8_t ulaw_enc(float x)
{
    int s = (int)(clampf(x, -1.f, 1.f) * 32767.f);
    int sign = 0;
    if (s < 0) { s = -s; sign = 0x80; }
    if (s > 32635) s = 32635;
    s += 0x84;
    union { float f; uint32_t u; } c;
    c.f = (float)s;
    int e = (int)(c.u >> 23) - 134, mant = (int)(c.u >> 19) & 15;
    return (uint8_t)~(sign | (e << 4) | mant);
}
static void ulaw_build(void)
{
    for (int i = 0; i < 256; ++i) {
        int u = ~i & 0xff;
        int sign = u & 0x80, e = (u >> 4) & 7, mant = u & 15;
        int s = (((mant << 3) + 0x84) << e) - 0x84;
        ulaw_table[i] = (float)(sign ? -s : s) * (1.f / 32768.f);
    }
}

/* --------------------------------------------------------------- events */
#define QN 128
struct queue { volatile uint32_t w, r; uint32_t d[QN]; };
static struct queue q_ctrl, q_midi;
static void push(struct queue *q, unsigned type, unsigned a, unsigned b)
{
    uint32_t w = q->w;
    if (w - q->r >= QN) return;
    q->d[w & (QN - 1)] = (type & 255u) | ((a & 255u) << 8) | ((b & 0xffffu) << 16);
    __asm__ volatile("" ::: "memory");
    q->w = w + 1;
}
void vsc_event(unsigned type, unsigned a, unsigned b) { push(&q_ctrl, type, a, b); }
void vsc_midi_event(unsigned type, unsigned a, unsigned b) { push(&q_midi, type, a, b); }

/* ----------------------------------------------------------------- tape
 * Each track: VSC_TAPE_BLOCKS blocks of 128 IMA ADPCM samples, and per block the
 * decoder's state at its start (so a block can be read from its start). Reading runs
 * one sample ahead of the playhead (s0, s1, interpolated); writing finalises the
 * sample the playhead has just left, re-encoding what was there mixed with the input
 * (DUB) or the input alone (REPLACE). A pass that stops mid-block runs on to the
 * block's end re-encoding what is there, so the next block's start state still fits. */
#define TBYTES (VSC_TAPE_BLOCKS * (VSC_TAPE_BLK / 2))
#define TCAP ((uint32_t)VSC_TAPE_BLOCKS * VSC_TAPE_BLK)
static uint8_t tape[VSC_TRACKS][TBYTES];
static int16_t hpred[VSC_TRACKS][VSC_TAPE_BLOCKS];
static uint8_t hidx[VSC_TRACKS][VSC_TAPE_BLOCKS];
static VSC_HOTDATA const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static VSC_HOTDATA const int8_t ima_adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct rd { int32_t pred; int32_t idx; int32_t ap, ai, bp, bi; uint32_t n0, m; float s0, s1; uint8_t on; int32_t lsp, lsi; };   /* lsp, lsi: the state just before the loop's START */
struct wr { int32_t pred, idx; float acc; uint16_t cnt; uint8_t on, ending; };
struct trk {
    struct rd r;
    struct wr w;
    float tp, inc;            /* its own place on the tape (its SPEED), and its step a frame */
    float dg;                 /* DUB: how much of what is there a pass keeps */
    uint8_t cr[VSC_SUB];      /* this sub-block: where it moved on a sample */
    int16_t sh[VSC_SHIFT_RING];   /* its pitch shifter's memory */
    uint32_t shw;
    float shph, shmix;
    float lp;                 /* the head's gentle low-pass */
    float dc;                 /* the preamp's DC blocker */
    float vu, pk, buf[VSC_SUB];
    float old[VSC_SUB];       /* what was on tape where a pass writes (for the writer, per sample) */
    uint32_t oldn[VSC_SUB];
};
static struct trk tk[VSC_TRACKS];
static float tp;              /* the master playhead (SPEED only): the loop's time, the first take, bounces */
static uint32_t loop_len;     /* the loop's END (exclusive); 0: no loop yet */
static uint32_t loop_start;   /* the loop's START */
static uint32_t take_len;     /* the furthest a take has gone: what START and END can reach (FULL) */
static uint8_t sl_src = 255, sl_dst = 255, sl_rec;   /* SLICE: the track sliced, where a take goes, recording */
static uint8_t transport, bounce_on, bounce_dst, bounce_src, bounce_clear;
static uint32_t bounce_done;  /* samples of the loop printed */

VSC_INL uint32_t loop_end(void) { return loop_len ? loop_len : TCAP; }
VSC_INL float tape_rate(unsigned s) { return s == TS_HI ? 22050.f : s == TS_MID ? 16000.f : 11025.f; }
VSC_INL float dec(const uint8_t *tb, uint32_t i, int32_t *pred, int32_t *idx)
{
    unsigned byte = tb[i >> 1], n = (i & 1u) ? byte >> 4 : byte & 15u;
    int32_t step = ima_step[*idx], diff = step >> 3;
    if (n & 4u) diff += step;
    if (n & 2u) diff += step >> 1;
    if (n & 1u) diff += step >> 2;
    int32_t p = (n & 8u) ? *pred - diff : *pred + diff;
    p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    *pred = p;
    int32_t x = *idx + ima_adj[n & 7u];
    *idx = x < 0 ? 0 : x > 88 ? 88 : x;
    return (float)p * (1.f / 32768.f);
}
VSC_INL void enc(uint8_t *tb, uint32_t i, float v, int32_t *pred, int32_t *idx)
{
    int32_t s = (int32_t)(clampf(v, -1.f, 1.f) * 32767.f);
    int32_t step = ima_step[*idx], diff = s - *pred, vp = step >> 3;
    unsigned n = 0;
    if (diff < 0) { n = 8u; diff = -diff; }
    if (diff >= step) { n |= 4u; diff -= step; vp += step; }
    step >>= 1;
    if (diff >= step) { n |= 2u; diff -= step; vp += step; }
    step >>= 1;
    if (diff >= step) { n |= 1u; vp += step; }
    int32_t p = (n & 8u) ? *pred - vp : *pred + vp;
    *pred = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    int32_t x = *idx + ima_adj[n & 7u];
    *idx = x < 0 ? 0 : x > 88 ? 88 : x;
    uint8_t *b = &tb[i >> 1];
    *b = (i & 1u) ? (uint8_t)((*b & 0x0fu) | (n << 4)) : (uint8_t)((*b & 0xf0u) | n);
}
/* the reader moves on a sample: s0 <- s1, the next into s1 (round the loop) */
static VSC_HOT void rd_fetch(unsigned t)
{
    struct rd *r = &tk[t].r;
    uint32_t i = r->m + 1u;
    if (i >= loop_end()) {
        i = loop_start;
        if (i & (VSC_TAPE_BLK - 1u)) {   /* a START inside a block: the state there, from the block's start */
            int32_t p = hpred[t][i / VSC_TAPE_BLK], x = hidx[t][i / VSC_TAPE_BLK];
            for (uint32_t j = i & ~(VSC_TAPE_BLK - 1u); j < i; ++j) (void)dec(tape[t], j, &p, &x);
            r->pred = r->lsp = p; r->idx = r->lsi = x;
        }
    }
    if (!(i & (VSC_TAPE_BLK - 1u))) { r->pred = hpred[t][i / VSC_TAPE_BLK]; r->idx = hidx[t][i / VSC_TAPE_BLK]; }
    r->ap = r->bp; r->ai = r->bi; r->bp = r->pred; r->bi = r->idx;
    r->s0 = r->s1; r->n0 = r->m;
    r->s1 = dec(tape[t], i, &r->pred, &r->idx);
    r->m = i;
}
/* the reader to sample n: s0 is n, s1 the one after */
static VSC_HOT_SMALL void rd_seek(unsigned t, uint32_t n)
{
    struct rd *r = &tk[t].r;
    uint32_t b = n / VSC_TAPE_BLK;
    r->pred = hpred[t][b]; r->idx = hidx[t][b];
    for (uint32_t i = b * VSC_TAPE_BLK; i < n; ++i) (void)dec(tape[t], i, &r->pred, &r->idx);
    r->bp = r->pred; r->bi = r->idx;
    if (n == loop_start) { r->lsp = r->pred; r->lsi = r->idx; }
    r->s1 = dec(tape[t], n, &r->pred, &r->idx);
    r->m = n;
    rd_fetch(t);
}
/* the writer finalises sample n: what was there (old) and the input it gathered */
VSC_INL int wr_last(uint32_t n) { return !((n + 1u) & (VSC_TAPE_BLK - 1u)) || n + 1u >= loop_end(); }
static VSC_HOT_SMALL float tape_soft(float v)   /* the tape takes a hot DUB softly */
{
    float a = absf(v);
    if (a <= 0.7f) return v;
    float u = (a - 0.7f) * (1.f / 0.3f);
    a = 0.7f + 0.3f * u / (1.f + u);
    return v < 0.f ? -a : a;
}
static VSC_HOT void wr_put(unsigned t, uint32_t n, float old, float in, int replace)
{
    struct wr *w = &tk[t].w;
    if (!(n & (VSC_TAPE_BLK - 1u))) { hpred[t][n / VSC_TAPE_BLK] = (int16_t)w->pred; hidx[t][n / VSC_TAPE_BLK] = (uint8_t)w->idx; }
    else if (n == loop_start) { w->pred = tk[t].r.lsp; w->idx = tk[t].r.lsi; }   /* round from END to a START inside a block: as the block has it there */
    float v = w->ending ? old : replace ? in : old * tk[t].dg + in;
    enc(tape[t], n, tape_soft(v), &w->pred, &w->idx);
    if (w->ending && wr_last(n)) w->on = w->ending = 0;
}
static VSC_HOT_SMALL void wr_start(unsigned t)
{
    struct trk *k = &tk[t];
    if (!k->r.on) { k->r.on = 1; rd_seek(t, (uint32_t)k->tp < loop_end() ? (uint32_t)k->tp : 0); }
    if (k->w.on && !k->w.ending) return;
    if (!k->w.on) { k->w.pred = k->r.ap; k->w.idx = k->r.ai; }
    k->w.on = 1; k->w.ending = 0; k->w.acc = 0.f; k->w.cnt = 0;
    vsc_meter.has[t] = 1;
}
/* punch out: the writer runs on to its block's end, re-encoding what is there */
static VSC_HOT_SMALL void wr_stop(unsigned t) { if (tk[t].w.on) tk[t].w.ending = 1; }
/* stop now: finish the block at once (the transport stops) */
static VSC_HOT_SMALL void wr_finish(unsigned t)
{
    struct trk *k = &tk[t];
    if (!k->w.on) return;
    k->w.ending = 1;
    for (unsigned guard = 0; k->w.on && guard <= VSC_TAPE_BLK; ++guard) {
        wr_put(t, k->r.n0, k->r.s0, 0.f, 0);
        rd_fetch(t);
    }
    k->w.on = k->w.ending = 0;
}
/* every track to n (its own place when n is ~0u) */
static VSC_HOT_SMALL void tape_seek_all(uint32_t n)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        struct trk *k = &tk[t];
        if (n != ~0u) k->tp = (float)n;
        if (k->tp >= (float)loop_end() || k->tp < (float)loop_start) k->tp = (float)loop_start;
        if (k->r.on) rd_seek(t, (uint32_t)k->tp);
    }
}
/* the loop's length moved: the readers read round the new one from where they are */
static VSC_HOT_SMALL void loop_set(uint32_t len)
{
    loop_len = len;
    if (len > take_len) take_len = len;
    if (tp >= (float)len || tp < (float)loop_start) tp = (float)loop_start;
    tape_seek_all(~0u);
}
static VSC_HOT_SMALL void track_clear(unsigned t)
{
    tk[t].w.on = tk[t].w.ending = 0;
    vsc_memset(tape[t], 0, TBYTES);
    vsc_memset(hpred[t], 0, sizeof hpred[t]); vsc_memset(hidx[t], 0, sizeof hidx[t]);
    tk[t].r.on = 0; tk[t].r.s0 = tk[t].r.s1 = 0.f;
    vsc_meter.has[t] = 0;
    vsc_memset(vsc_meter.overview[t], 0, sizeof vsc_meter.overview[t]);
}

/* -------------------------------------------------------------- the desk */
/* the parameters in use this block: the knobs, moved by the robots */
static float pf[VSC_NP];
static float rob_ph[VSC_ROBOTS], rob_out[VSC_ROBOTS], rob_hold[VSC_ROBOTS][2];
static uint8_t is_enum[VSC_NP], pmax[VSC_NP];
static VSC_HOTDATA const uint8_t rtarget[VSC_RTARGETS] = {
    P_SPEED, P_PITCH, P_TLEVEL, P_IDRIVE,
    P_STRIP + 0 * VSC_TP + S_LEVEL, P_STRIP + 1 * VSC_TP + S_LEVEL, P_STRIP + 2 * VSC_TP + S_LEVEL, P_STRIP + 3 * VSC_TP + S_LEVEL,
    P_STRIP + 0 * VSC_TP + S_PAN, P_STRIP + 1 * VSC_TP + S_PAN, P_STRIP + 2 * VSC_TP + S_PAN, P_STRIP + 3 * VSC_TP + S_PAN,
    P_RMIX, P_RDECAY, P_RWIDTH, P_RSHIFT,
    P_STRIP + 0 * VSC_TP + S_SPEED, P_STRIP + 1 * VSC_TP + S_SPEED, P_STRIP + 2 * VSC_TP + S_SPEED, P_STRIP + 3 * VSC_TP + S_SPEED,
    P_STRIP + 0 * VSC_TP + S_PITCH, P_STRIP + 1 * VSC_TP + S_PITCH, P_STRIP + 2 * VSC_TP + S_PITCH, P_STRIP + 3 * VSC_TP + S_PITCH,
};
volatile uint8_t vsc_motion_hold;
/* a lane's value where the loop is (the master's place), between its points */
static VSC_HOT_SMALL float motion_at(unsigned m)
{
    const struct vsc_motion *mo = &vsc.motion[m];
    float len = loop_len > loop_start ? (float)(loop_len - loop_start) : 1.f;
    float x = (tp - (float)loop_start) / len * (float)VSC_MOTION_PTS;
    if (x < 0.f) x = 0.f;
    unsigned i = (unsigned)x % VSC_MOTION_PTS, j = (i + 1u) % VSC_MOTION_PTS;
    float f = x - (float)(unsigned)x;
    return (float)mo->v[i] + ((float)mo->v[j] - (float)mo->v[i]) * f;
}
float vsc_motion_value(unsigned m) { return m < VSC_MOTIONS ? motion_at(m) : 0.f; }
static VSC_HOT_SMALL void robots(void)
{
    for (unsigned p = 0; p < VSC_NP; ++p) pf[p] = (float)vsc.p[p];
    /* MOTION: each lane in place of its knob (not while it's being written, nor with no loop) */
    if (loop_len) for (unsigned m = 0; m < VSC_MOTIONS; ++m) {
        const struct vsc_motion *mo = &vsc.motion[m];
        if (!mo->on || mo->param >= VSC_NP || (vsc_motion_hold >> m & 1u)) continue;
        pf[mo->param] = motion_at(m);
    }
    for (unsigned r = 0; r < VSC_ROBOTS; ++r) {
        const uint8_t *q = &vsc.p[P_ROB(r, 0)];
        unsigned shape = q[R_SHAPE];
        float inc, o;
        if (shape == RS_GESTURE) inc = 1000.f / (float)(vsc.gest_ms[r] ? vsc.gest_ms[r] : 1000u) * ((float)VSC_BLOCK / (float)VSC_SR);
        else inc = 0.02f * exp2a((float)q[R_RATE] * (9.f / 127.f)) * ((float)VSC_BLOCK / (float)VSC_SR);
        float ph = rob_ph[r] + inc;
        int wrapped = ph >= 1.f;
        if (wrapped) { ph -= (float)(int)ph; rob_hold[r][0] = rob_hold[r][1]; rob_hold[r][1] = rndc() * 2.f - 1.f; }
        rob_ph[r] = ph;
        float sk = clampf((float)q[R_SKEW] * (1.f / 127.f), 0.03f, 0.97f);
        float w = ph < sk ? 0.5f * ph / sk : 0.5f + 0.5f * (ph - sk) / (1.f - sk);   /* skew: the cycle's first half takes sk */
        switch (shape) {
        case RS_SINE: o = sin01(w); break;
        case RS_TRI: o = w < 0.5f ? 4.f * w - 1.f : 3.f - 4.f * w; break;
        case RS_SAW: o = 2.f * w - 1.f; break;
        case RS_SQUARE: o = ph < sk ? 1.f : -1.f; break;
        case RS_SH: o = rob_hold[r][1]; break;
        case RS_DRIFT: { float s = ph * ph * (3.f - 2.f * ph); o = rob_hold[r][0] + (rob_hold[r][1] - rob_hold[r][0]) * s; break; }
        default: {
            float x = ph * (float)(VSC_GEST - 1); unsigned i = (unsigned)x; float f = x - (float)i;
            unsigned j = i + 1u < VSC_GEST ? i + 1u : i;
            o = ((float)vsc.gest[r][i] + ((float)vsc.gest[r][j] - (float)vsc.gest[r][i]) * f) * (1.f / 127.f);
            break;
        }
        }
        rob_out[r] = o;
        vsc_meter.robot[r] = o;
        unsigned tg = q[R_TARGET] < VSC_RTARGETS ? rtarget[q[R_TARGET]] : VSC_NP;
        if (tg >= VSC_NP || !q[R_DEPTH]) continue;
        float span = shape == RS_GESTURE ? (float)pmax[tg] : 0.5f * (float)pmax[tg];
        pf[tg] = clampf(pf[tg] + o * span * (float)q[R_DEPTH] * (1.f / 127.f), 0.f, (float)pmax[tg]);
    }
}
VSC_INL float trim_gain(float v) { return v < 0.5f ? 0.f : exp2a((v - 80.f) * (0.3f / 6.0206f)); }   /* 0.3 dB a step, 80: 0 dB */
VSC_INL float level_gain(float v) { float g = v * (1.f / 100.f); return g * g; }
/* The preamp: clean to its knee (about 0 VU), then rounding off, a little lopsided (the
 * second harmonic a pushed transistor stage adds), toward 1. */
#define KNEE 0.5f
VSC_INL float pre(float u, float even, float *dc, uint8_t *hit)
{
    u += even * u * u;
    float a = absf(u);
    if (a > KNEE) {
        float x = (a - KNEE) * (1.f / (1.f - KNEE));
        a = KNEE + (1.f - KNEE) * x / (1.f + x);
        u = u < 0.f ? -a : a;
        *hit = 255;
    }
    *dc += (u - *dc) * 0.0008f;   /* (the lopsided part's DC, about 5 Hz) */
    return u - *dc;
}

/* ---------------------------------------------------------------- RESON
 * One resonant filterbank of eight bands, on a track's playback, the input, or a
 * bounce. Each band is a bandpass whose damping comes from its decay, so the same bank
 * filters (FILTER), carries a vocoder's band envelopes (VOCODE), rings from short bursts
 * of excitation (PING) or does all at once (HYBRID). The profile sets what the knobs
 * don't. Off, it costs nothing. */
enum { BM_OFF, BM_FILTER, BM_VOCODE, BM_PING, BM_HYBRID };
enum { BV_SELF, BV_INPUT, BV_OSC, BV_NOISE, BV_OUTPUT };
enum { BT_OFF, BT_HIGH, BT_FULL, BT_LOW };
/* a profile: mode, modulator, trigger, liquid, feedback, spectral, attack, release,
 * depth, sensitivity, hardness, cohesion, droplets, migration, the oscillator */
enum { H_MODE, H_VMOD, H_TRIG, H_LIQ, H_FDBK, H_SPEC, H_ATK, H_REL, H_DEPTH, H_SENS, H_HARD, H_COHES, H_DROP, H_MIGR, H_OSC, H_N };
static VSC_HOTDATA const uint8_t rprof_h[RP_COUNT][H_N] = {
    {BM_HYBRID, BV_SELF, BT_HIGH, 44, 10, 64, 18, 54, 72, 90, 89, 80, 0, 20, 40},     /* LIQUID MEMORY */
    {BM_HYBRID, BV_SELF, BT_HIGH, 30, 10, 64, 18, 54, 72, 90, 120, 90, 0, 20, 40},    /* GLASS */
    {BM_VOCODE, BV_OUTPUT, BT_OFF, 20, 10, 64, 30, 70, 100, 90, 89, 80, 0, 20, 40},   /* THROAT */
    {BM_PING, BV_SELF, BT_FULL, 44, 10, 64, 18, 54, 72, 90, 89, 40, 40, 50, 40},      /* DUST */
    {BM_FILTER, BV_SELF, BT_OFF, 0, 10, 64, 18, 54, 72, 90, 89, 80, 0, 20, 40},       /* RADIO */
    {BM_HYBRID, BV_SELF, BT_HIGH, 44, 12, 64, 18, 54, 72, 90, 89, 100, 50, 20, 40},   /* DROPS */
};
/* and what it puts on the knobs: mix, decay, width, dust, the eight bands */
static const uint8_t rprof_k[RP_COUNT][12] = {
    {83, 66, 76, 0, 38, 110, 38, 30, 30, 34, 103, 30},
    {90, 41, 90, 30, 25, 30, 25, 25, 40, 99, 105, 97},
    {85, 50, 50, 0, 100, 108, 104, 96, 30, 34, 40, 30},
    {100, 100, 76, 70, 85, 95, 90, 88, 92, 86, 95, 84},
    {75, 30, 20, 0, 40, 60, 105, 105, 105, 105, 50, 40},
    {83, 66, 76, 15, 38, 110, 38, 30, 30, 34, 103, 30},
};
void vsc_reson_profile(unsigned n)
{
    if (n >= RP_COUNT) return;
    const uint8_t *k = rprof_k[n];
    vsc.p[P_RPROF] = (uint8_t)n;
    vsc.p[P_RMIX] = k[0]; vsc.p[P_RDECAY] = k[1]; vsc.p[P_RWIDTH] = k[2]; vsc.p[P_RDUST] = k[3];
    for (unsigned b = 0; b < VSC_RBANDS; ++b) vsc.p[P_RG0 + b] = k[4 + b];
}
static VSC_HOTDATA const float rb_hz[VSC_RBANDS] = {200.f, 400.f, 800.f, 1200.f, 1600.f, 2400.f, 3200.f, 4800.f};
static VSC_HOTDATA const float rb_dec[VSC_RBANDS] = {78.f, 64.f, 50.f, 41.f, 35.f, 26.f, 20.f, 11.f};   /* each band's decay, around the knob's */
static VSC_HOTDATA const float rb_w1[VSC_RBANDS] = {0.5f, 0.45f, 0.6f, 0.7f, 0.8f, 0.9f, 1.f, 1.1f};
static VSC_HOTDATA const float rb_w2[VSC_RBANDS] = {0.3f, -0.6f, 0.5f, -0.4f, 0.7f, -0.5f, 0.9f, -0.8f};
#define RS_RING 1024u                   /* the droplets' memory: 23 ms of the wet sound, mu-law */
#define RS_DROPS 3
struct rdrop { float pos, inc, amp, du; uint16_t len, t; uint8_t dest, on; };
static struct {
    float cy1[VSC_RBANDS], cy2[VSC_RBANDS];
    float ry1[VSC_RBANDS], ry2[VSC_RBANDS], rx1[VSC_RBANDS], rx2[VSC_RBANDS];
    float my1[VSC_RBANDS], my2[VSC_RBANDS];
    float cd1[VSC_RBANDS], cd2[VSC_RBANDS], cb0[VSC_RBANDS], ab0[VSC_RBANDS];
    float rd1[VSC_RBANDS], rd2[VSC_RBANDS], rb0[VSC_RBANDS];
    float cx1, cx2, mx1, mx2;
    float pn[VSC_RBANDS];
    float gain[VSC_RBANDS], gstep[VSC_RBANDS], gg[VSC_RBANDS];
    float env[VSC_RBANDS], oenv[VSC_RBANDS];
    float pw[VSC_RBANDS], inj[VSC_RBANDS];
    float exc, exc_dec;
    float lq[2], lqt[2], lqs[2];
    float hp, lp, ef, es, act;
    float osc, fb, dc, yh;
    int qidx;
    uint16_t refr, rw;
    uint8_t armed, where;
    struct rdrop d[RS_DROPS];
    uint8_t ring[RS_RING];
} rs;
static float rs_req, rs_note = 0.f;   /* a ping asked for by a note (its velocity); the oscillator's note */
static VSC_HOT_SMALL float rsqrt_(float x) { return sqrta(x); }
#define RS_CONT(mode) ((mode) == BM_PING ? 0.f : (mode) == BM_HYBRID ? 0.5f : 1.f)
static VSC_HOT_SMALL float svf_g(float hz)
{
    hz = clampf(hz, 16.f, 0.45f * (float)VSC_SR);
    return tanpre(3.14159265f * hz / (float)VSC_SR);
}
static VSC_HOT_SMALL void reson_control(unsigned n, const uint8_t *h)
{
    {   /* no denormals: every float of the state under about 1e-26 becomes 0 */
        uint32_t *f = (uint32_t *)(void *)&rs;
        for (unsigned i = 0; i < offsetof(__typeof__(rs), qidx) / 4u; ++i)
            if (((f[i] >> 23) & 0xffu) < 40u) f[i] = 0u;
    }
    const float cont = RS_CONT(h[H_MODE]);
    const float liq = (float)h[H_LIQ] * (1.f / 127.f);
    for (unsigned w = 0; w < 2; ++w) {
        if (absf(rs.lq[w] - rs.lqt[w]) < absf(rs.lqs[w]) * 1.5f || rs.lqs[w] == 0.f) {
            rs.lqt[w] = rndc() * 2.f - 1.f;
            rs.lqs[w] = (rs.lqt[w] - rs.lq[w]) * (float)n / ((0.35f + rndc() * (w ? 1.6f : 0.9f)) * (float)VSC_SR);
        }
        rs.lq[w] += rs.lqs[w];
    }
    const float shift = pf[P_RSHIFT] - 64.f, t0 = pf[P_RDECAY] * (8.f / 127.f);
    const float kw = exp2a(-pf[P_RWIDTH] * (5.f / 127.f));
    for (unsigned b = 0; b < VSC_RBANDS; ++b) {
        float mv = liq * (rb_w1[b] * rs.lq[0] + rb_w2[b] * rs.lq[1]);
        float f = rb_hz[b] * exp2a(shift * (1.f / 64.f)) * (1.f + 0.03f * mv);
        float T = 0.01f * exp2a(t0 + (rb_dec[b] - 64.f) * (1.f / 32.f)) * (1.f + 0.3f * liq * rb_w2[b] * rs.lq[1]);
        float g = svf_g(2.f * f), g2 = g * g, kk = 2.2f / (f * T);   /* (the bank runs at half the rate) */
        kk = clampf(kk, 0.004f, 1.6f);
        float ia = 1.f / (1.f + g * kk + g2);
        rs.rd1[b] = 2.f * (g2 - 1.f) * ia; rs.rd2[b] = (1.f - g * kk + g2) * ia; rs.rb0[b] = g * kk * ia;
        rs.pn[b] = 1.25f * (float)VSC_SR / (6.2831853f * f * kk);
        float kc = kk > kw ? kk : kw, ic = 1.f / (1.f + g * kc + g2);
        rs.cd1[b] = 2.f * (g2 - 1.f) * ic; rs.cd2[b] = (1.f - g * kc + g2) * ic; rs.ab0[b] = g * kc * ic;
        rs.cb0[b] = rs.ab0[b] * cont * 8.f * rsqrt_(0.125f / kc);
        float v = pf[P_RG0 + b];
        float lin = v < 0.5f ? 0.f : exp2a((-60.f + v * (66.f / 127.f) + 3.f * liq * rb_w1[b] * rs.lq[0]) * 0.1660964f);
        rs.gstep[b] = (lin - rs.gain[b]) * (1.f / (float)n);
    }
}
static VSC_HOT_SMALL void reson_ping(float vel, const uint8_t *h)
{
    float spec = (float)h[H_SPEC] * (1.f / 127.f), mx = 1e-6f;
    for (unsigned b = 0; b < VSC_RBANDS; ++b) if (rs.env[b] > mx) mx = rs.env[b];
    float ex = 1.f + 3.f * spec;
    for (unsigned b = 0; b < VSC_RBANDS; ++b) {
        float w = exp2a(ex * log2a(rs.env[b] / mx + 1e-4f));
        rs.pw[b] = (1.f - spec + spec * w) * rs.pn[b];
    }
    float hard = (float)h[H_HARD] * (1.f / 127.f), tau = (0.004f - 0.0037f * hard) * (float)VSC_SR;
    rs.exc_dec = exp2a(-1.4427f / tau);
    rs.exc += vel * 0.7f / tau;
    ++vsc_meter.rb_pings;
}
/* x: the sound it works on, in place; in: the input (the modulator for INPUT) */
static VSC_HOT void reson(float *x, const float *inp, unsigned n, unsigned where)
{
    const uint8_t *h = rprof_h[vsc.p[P_RPROF] < RP_COUNT ? vsc.p[P_RPROF] : 0];
    const unsigned mode = h[H_MODE], vm = h[H_VMOD], trig = h[H_TRIG];
    if (rs.where != where) {   /* moved: start clean */
        vsc_memset(&rs, 0, offsetof(__typeof__(rs), ring)); vsc_memset(rs.ring, 0xff, RS_RING);
        rs.where = (uint8_t)where; rs.armed = 1;
    }
    reson_control(n, h);
    const float cont = RS_CONT(mode);
    const int voc = mode == BM_VOCODE || mode == BM_HYBRID;
    const float depth = (float)h[H_DEPTH] * (1.f / 127.f), wet = pf[P_RMIX] * (1.f / 127.f);
    const float sa_ = (float)h[H_ATK] * (1.f / 127.f), sr_ = (float)h[H_REL] * (1.f / 127.f);
#define RS_EV 4u
    const float att = 1.f - exp2a(-1.4427f * (float)RS_EV / ((0.0005f + 0.05f * sa_ * sa_) * (float)VSC_SR));
    const float rel = 1.f - exp2a(-1.4427f * (float)RS_EV / ((0.005f + 0.6f * sr_ * sr_) * (float)VSC_SR));
    const float fdbk = (float)h[H_FDBK] * (0.85f / 127.f);
    const float *mod = x;
    float mbuf[VSC_SUB];
    if (vm == BV_INPUT) mod = inp;
    else if (vm == BV_OSC || vm == BV_NOISE) {
        float dust = pf[P_RDUST] * (1.f / 127.f), shape = (float)h[H_OSC] * (4.f / 127.f);
        float inc = 110.f * exp2a(rs_note * (1.f / 12.f)) * (1.f / (float)VSC_SR);
        for (unsigned i = 0; i < n; ++i) {
            float v;
            if (vm == BV_NOISE) {
                float r = rndu() * 2.f - 1.f;
                v = dust > 0.7f ? r : (rndu() < dust * dust * 0.3f ? r * 3.f : 0.f);
            } else {
                rs.osc += inc; if (rs.osc >= 1.f) rs.osc -= 1.f;
                float ph = rs.osc, q = ph < 0.25f ? ph + 0.75f : ph - 0.25f;
                float sn = cos2pi_c(q <= 0.5f ? q : 1.f - q), saw = 2.f * ph - 1.f, pul = ph < 0.3f ? 1.f : -1.f;
                v = (shape < 2.f ? sn + (saw - sn) * shape * 0.5f : saw + (pul - saw) * (shape - 2.f) * 0.5f) * 0.5f;
            }
            mbuf[i] = v;
        }
        mod = mbuf;
    }
    const int other = !vsc_relief && (vm == BV_INPUT || vm == BV_OSC || vm == BV_NOISE);
    const float hpk = 0.5775f, lpk = 0.0591f;
    const float sens = (float)h[H_SENS] * (1.f / 127.f), th = 0.25f * exp2a(-sens * 9.f);
    const float fa = 2.f / (0.001f * (float)VSC_SR), fr = 2.f / (0.015f * (float)VSC_SR), sa = 2.f / (0.06f * (float)VSC_SR), sr = 2.f / (0.3f * (float)VSC_SR);
    const float dust = pf[P_RDUST] * (1.f / 127.f), cohes = (float)h[H_COHES] * (1.f / 127.f);
    const float drop = vsc_relief >= 2 ? 0.f : (float)h[H_DROP] * (1.f / 127.f), migr = (float)h[H_MIGR] * (1.f / 127.f);
    const float dust_p = vm == BV_NOISE ? 0.f : dust * dust * (600.f / (float)VSC_SR);
    unsigned live = 0;
    float opk[VSC_RBANDS];
    unsigned on = 0;
    float gmin = 1e-6f;
    const float gk = vsc_relief >= 2 ? 0.5f : 0.25f;
    if (vsc_relief) for (unsigned b = 0; b < VSC_RBANDS; ++b) if (rs.gain[b] * gk > gmin) gmin = rs.gain[b] * gk;
    for (unsigned b = 0; b < VSC_RBANDS; ++b) {
        opk[b] = 0.f;
        if (rs.gain[b] > gmin || rs.gstep[b] > 0.f) on |= 1u << b;
        else { rs.gain[b] += rs.gstep[b] * (float)n; rs.oenv[b] *= 0.9f; }
    }
    const float vd8 = voc ? depth * 8.f : 0.f, vd3 = voc ? depth * 3.f : 0.f, vbase = voc ? 1.f - depth : 1.f;
    const unsigned self = !other && vm != BV_OUTPUT;
    for (unsigned i = 0; i < n; ++i) {
        float in = x[i], m = mod[i];
        rs.exc *= rs.exc_dec;
        float y = rs.yh;
        if (!(i & 1u)) {
        const unsigned i1 = i + 1u < n ? i + 1u : i;
        const float inh = 0.5f * (in + x[i1]), mh = 0.5f * (m + mod[i1]);
        rs.lp += (mh - rs.lp) * lpk; rs.hp += (mh - rs.hp) * hpk;
        float d = trig == BT_HIGH ? mh - rs.hp : trig == BT_LOW ? rs.lp : mh;
        float a = absf(d);
        rs.ef += (a - rs.ef) * (a > rs.ef ? fa : fr);
        rs.es += (a - rs.es) * (a > rs.es ? sa : sr);
        float nov = rs.ef - 1.8f * rs.es, tha = th + 0.4f * rs.es;
        rs.act += ((nov > th ? 1.f : 0.f) - rs.act) * 0.001f;
        if (rs.refr) --rs.refr;
        if (trig != BT_OFF) {
            if (nov > tha && rs.armed && !rs.refr) {
                reson_ping(clampf(nov / th * 0.2f, 0.15f, 1.f), h);
                rs.armed = 0; rs.refr = (uint16_t)(0.02f * VSC_SR);
            } else if (nov < tha * 0.5f) rs.armed = 1;
        }
        if (dust_p > 0.f && rndu() < dust_p * (1.f - cohes + cohes * 6.f * rs.act)) {
            unsigned b = rnd() & 7u;
            rs.inj[b] += (rndc() * 2.f - 1.f) * rs.pn[b] * (0.15f + 2.f * rs.ef);
        }
        const float e = rs.exc * (1.f + rs.exc_dec), fb = rs.fb * fdbk;
        const float dc_ = inh - rs.cx2, dm = mh - rs.mx2;
        rs.cx2 = rs.cx1; rs.cx1 = inh; rs.mx2 = rs.mx1; rs.mx1 = mh;
        float yc = 0.f;
        const int upd = (i & (RS_EV - 1u)) == 0;
        for (unsigned b = 0; b < VSC_RBANDS; ++b) {
            if (!(on >> b & 1u)) continue;
            float vc = 0.f;
            if (cont > 0.f) { vc = rs.cb0[b] * dc_ - rs.cd1[b] * rs.cy1[b] - rs.cd2[b] * rs.cy2[b]; rs.cy2[b] = rs.cy1[b]; rs.cy1[b] = vc; }
            float u = e * rs.pw[b] + rs.inj[b] + fb;
            float vr = rs.rb0[b] * (u - rs.rx2[b]) - rs.rd1[b] * rs.ry1[b] - rs.rd2[b] * rs.ry2[b];
            rs.rx2[b] = rs.rx1[b]; rs.rx1[b] = u; rs.ry2[b] = rs.ry1[b]; rs.ry1[b] = vr;
            rs.inj[b] = 0.f;
            float sb = vc + vr, em = 0.f;
            if (other) {
                float vm_ = rs.ab0[b] * dm - rs.cd1[b] * rs.my1[b] - rs.cd2[b] * rs.my2[b];
                rs.my2[b] = rs.my1[b]; rs.my1[b] = vm_; em = absf(vm_);
            }
            float gg;
            if (upd) {
                float en = other ? em : self ? absf(sb) : rs.oenv[b];
                float env = rs.env[b];
                env += (en - env) * (en > env ? att : rel); rs.env[b] = env;
                float g = rs.gain[b], ve = env * vd8; rs.gain[b] = g + rs.gstep[b] * (float)RS_EV;
                gg = g * (vbase + (ve < vd3 ? ve : vd3));
                rs.gg[b] = gg;
                float ao = absf(sb * gg); if (ao > opk[b]) opk[b] = ao;
            } else gg = rs.gg[b];
            yc += sb * gg;
        }
        y = 0.5f * (rs.yh + yc); rs.yh = yc;
        }
        if (fdbk > 0.f) {
            rs.dc += (y - rs.dc) * 0.001f;
            float fy = y - rs.dc;
            rs.fb = fy / (1.f + absf(fy));
        } else rs.fb = 0.f;
        float dsum = 0.f;
        if (drop > 0.f) {
            rs.ring[rs.rw] = ulaw_enc(clampf(y, -1.f, 1.f)); rs.rw = (uint16_t)((rs.rw + 1u) & (RS_RING - 1u));
            const float ps = drop * (0.4f + cohes * 6.f * rs.act) * (60.f / (float)VSC_SR);
            float r = rndu();
            for (unsigned j = 0; j < RS_DROPS; ++j) {
                struct rdrop *q = &rs.d[j];
                if (!q->on) {
                    if (r < ps) {
                        r = 2.f;
                        q->on = 1; q->t = 0; q->len = (uint16_t)((0.004f + rndc() * 0.014f) * VSC_SR);
                        q->du = 1.f / (float)q->len;
                        q->pos = (float)((rs.rw + RS_RING - q->len - 64u - (rnd() & 511u)) & (RS_RING - 1u));
                        q->inc = 1.f + (rndc() - 0.5f) * 0.02f; q->amp = 0.7f + rndc() * 0.6f;
                        q->dest = (uint8_t)(rnd() & 7u); if (rs.gain[q->dest] < 1e-3f) q->dest = (uint8_t)((q->dest + 5u) & 7u);
                    } else r -= ps;
                    continue;
                }
                ++live;
                unsigned ip = (unsigned)q->pos & (RS_RING - 1u);
                float uu = (float)q->t * q->du, w = 4.f * uu * (1.f - uu);
                float v = ulaw_table[rs.ring[ip]] * w * q->amp;
                dsum += v;
                rs.inj[q->dest] += v * migr * 1.5f;
                q->pos += q->inc; if (q->pos >= (float)RS_RING) q->pos -= (float)RS_RING;
                if (++q->t >= q->len) q->on = 0;
            }
        }
        float o = in * (1.f - wet) + (y + dsum * drop * (1.f - migr)) * wet;
        if (!(o == o)) { vsc_memset(&rs, 0, offsetof(__typeof__(rs), ring)); rs.where = (uint8_t)where; o = 0.f; }
        x[i] = o;
    }
    for (unsigned b = 0; b < VSC_RBANDS; ++b) if (on >> b & 1u) rs.oenv[b] += (opk[b] - rs.oenv[b]) * (opk[b] > rs.oenv[b] ? 0.27f : 0.016f);
    if (rs_req > 0.f) { reson_ping(rs_req, h); rs_req = 0.f; }
    for (unsigned b = 0; b < VSC_RBANDS; ++b) { float l = rs.oenv[b] * 600.f; vsc_meter.rb_level[b] = (uint8_t)(l > 255.f ? 255.f : l); }
    vsc_meter.rb_drops = (uint8_t)live;
}

/* ------------------------------------------------------------ pitch shift
 * Each track's: two read heads sweep a short memory of its sound, crossfaded: the pitch
 * moves, the time doesn't (SPEED moves both, as tape does). Out of the way at 0. */
VSC_INL float sh_tap(const int16_t *rg, uint32_t w, float d)
{
    float pos = (float)w - d;
    if (pos < 0.f) pos += (float)VSC_SHIFT_RING;
    unsigned i = (unsigned)pos; float f = pos - (float)i;
    float a = rg[i & (VSC_SHIFT_RING - 1u)], b = rg[(i + 1u) & (VSC_SHIFT_RING - 1u)];
    return (a + (b - a) * f) * (1.f / 32768.f);
}
static VSC_HOT void shifter(struct trk *k, float *x, unsigned n, float semis)
{
    const float W = (float)(VSC_SHIFT_RING - 4);
    const int want = semis > 0.05f || semis < -0.05f;
    if (!want && k->shmix <= 0.f) return;
    const float ratio = exp2a(semis * (1.f / 12.f)), dinc = (1.f - ratio) / W, mstep = want ? 1.f / 512.f : -1.f / 512.f;
    for (unsigned i = 0; i < n; ++i) {
        const float d = x[i];
        k->sh[k->shw & (VSC_SHIFT_RING - 1u)] = (int16_t)(clampf(d, -1.f, 1.f) * 32767.f);
        float ph = k->shph + dinc; if (ph >= 1.f) ph -= 1.f; else if (ph < 0.f) ph += 1.f;
        k->shph = ph;
        float p2 = ph + 0.5f; if (p2 >= 1.f) p2 -= 1.f;
        float w1 = ph < 0.5f ? 2.f * ph : 2.f - 2.f * ph;
        float y = sh_tap(k->sh, k->shw, 2.f + ph * W) * w1 + sh_tap(k->sh, k->shw, 2.f + p2 * W) * (1.f - w1);
        k->shmix = clampf(k->shmix + mstep, 0.f, 1.f);
        x[i] = d + (y - d) * k->shmix;
        ++k->shw;
    }
}

/* ------------------------------------------------------------ the master
 * A four-band EQ (biquads, after the RBJ cookbook; the first and last bands can shelve
 * or cut), then the bus compressor after Ravel's COMP (after the SSL G bus compressor:
 * stereo-linked, feed-forward, a hard knee, stepped ratios, attacks and releases, AUTO
 * releasing in two stages; the level is the peak of each 16 frames, the gain worked out
 * in dB once per 16 and swept across them). */
float vsc_eq_coef[4][5];          /* b0 b1 b2 a1 a2 (a0 = 1), for the picture */
uint8_t vsc_eq_live;              /* bands that do something */
static uint8_t eq_seen[16] = {255};
static float eq_z[4][2][2];       /* each band's two states, per side */
VSC_INL float sin2pi(float p) { float q = p - 0.25f; return cos2pi(q < 0.f ? -q : q); }   /* p in 0..0.5 */
static VSC_HOT_SMALL void eq_design(void)
{
    vsc_eq_live = 0;
    for (unsigned b = 0; b < 4; ++b) {
        const uint8_t *q = &vsc.p[P_EQ(b, 0)];
        float f = 20.f * exp2a((float)q[E_FREQ] * (9.965784f / 127.f));
        float pw = clampf(f / (float)VSC_SR, 0.0002f, 0.45f);
        float cw = cos2pi(pw), sw = sin2pi(pw);
        float Q = 0.3f * exp2a((float)q[E_Q] * (5.f / 127.f));
        float gdb = ((float)q[E_GAIN] - 64.f) * (15.f / 64.f);
        float A = exp2a(gdb * (0.16609640f * 0.5f));   /* 10^(dB/40) */
        float al = sw / (2.f * Q), sa = 2.f * sqrta(A) * al;
        unsigned ty = q[E_TYPE];
        float b0, b1, b2, a0, a1, a2;
        int on = 1;
        if (ty == EQ_CUT) {
            if (b == 0) { b0 = (1.f + cw) * 0.5f; b1 = -(1.f + cw); }
            else { b0 = (1.f - cw) * 0.5f; b1 = 1.f - cw; }
            b2 = b0; a0 = 1.f + al; a1 = -2.f * cw; a2 = 1.f - al;
        } else if (ty == EQ_SHELF && b == 0) {
            b0 = A * ((A + 1.f) - (A - 1.f) * cw + sa); b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cw); b2 = A * ((A + 1.f) - (A - 1.f) * cw - sa);
            a0 = (A + 1.f) + (A - 1.f) * cw + sa; a1 = -2.f * ((A - 1.f) + (A + 1.f) * cw); a2 = (A + 1.f) + (A - 1.f) * cw - sa;
            on = q[E_GAIN] != 64;
        } else if (ty == EQ_SHELF) {
            b0 = A * ((A + 1.f) + (A - 1.f) * cw + sa); b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cw); b2 = A * ((A + 1.f) + (A - 1.f) * cw - sa);
            a0 = (A + 1.f) - (A - 1.f) * cw + sa; a1 = 2.f * ((A - 1.f) - (A + 1.f) * cw); a2 = (A + 1.f) - (A - 1.f) * cw - sa;
            on = q[E_GAIN] != 64;
        } else {
            b0 = 1.f + al * A; b1 = -2.f * cw; b2 = 1.f - al * A; a0 = 1.f + al / A; a1 = -2.f * cw; a2 = 1.f - al / A;
            on = q[E_GAIN] != 64;
        }
        float ia = 1.f / a0;
        float *c = vsc_eq_coef[b];
        c[0] = b0 * ia; c[1] = b1 * ia; c[2] = b2 * ia; c[3] = a1 * ia; c[4] = a2 * ia;
        if (on) vsc_eq_live |= (uint8_t)(1u << b);
        else { eq_z[b][0][0] = eq_z[b][0][1] = eq_z[b][1][0] = eq_z[b][1][1] = 0.f; }
    }
}
static VSC_HOT void eq_run(float *l, float *r, unsigned n)
{
    int changed = 0;
    for (unsigned i = 0; i < 16; ++i) if (eq_seen[i] != vsc.p[P_EQ0 + i]) { eq_seen[i] = vsc.p[P_EQ0 + i]; changed = 1; }
    if (changed) eq_design();
    for (unsigned b = 0; b < 4; ++b) {
        if (!(vsc_eq_live >> b & 1u)) continue;
        const float *c = vsc_eq_coef[b];
        for (unsigned ch = 0; ch < 2; ++ch) {
            float *x = ch ? r : l, z1 = eq_z[b][ch][0], z2 = eq_z[b][ch][1];
            for (unsigned i = 0; i < n; ++i) {   /* transposed direct form II */
                float in = x[i], y = c[0] * in + z1;
                z1 = c[1] * in - c[3] * y + z2;
                z2 = c[2] * in - c[4] * y;
                x[i] = y;
            }
            if (absf(z1) < 1e-20f) z1 = 0.f;
            if (absf(z2) < 1e-20f) z2 = 0.f;
            if (!(z1 == z1) || !(z2 == z2)) z1 = z2 = 0.f;
            eq_z[b][ch][0] = z1; eq_z[b][ch][1] = z2;
        }
    }
}
#define COMP_SUB 16
static float comp_gr, comp_grs, comp_g = 1.f, comp_db = -120.f;
static VSC_HOTDATA const float ratio_slope[3] = {0.5f, 0.75f, 0.9f};
static VSC_HOTDATA const float atk_s[6] = {0.0001f, 0.0003f, 0.001f, 0.003f, 0.01f, 0.03f};
static VSC_HOTDATA const float rel_s[5] = {0.1f, 0.3f, 0.6f, 1.2f, 0.12f};   /* (AUTO: its fast stage) */
VSC_INL float coef_of(float sec) { return 1.f - exp2a(-1.442695f * (float)COMP_SUB / (sec * (float)VSC_SR)); }
static VSC_HOT void comp_run(float *l, float *r, unsigned n, const float *key)
{
    if (!vsc.p[P_CON]) { comp_gr = comp_grs = 0.f; comp_g = 1.f; vsc_meter.comp_gr = 0.f; vsc_meter.comp_in = -120.f; return; }
    unsigned rel = vsc.p[P_CREL] > 4 ? 4 : vsc.p[P_CREL];
    const float thr = (float)vsc.p[P_CTHR] - 30.f, slope = ratio_slope[vsc.p[P_CRATIO] > 2 ? 2 : vsc.p[P_CRATIO]];
    const float aa = coef_of(atk_s[vsc.p[P_CATK] > 5 ? 5 : vsc.p[P_CATK]]), ar = coef_of(rel_s[rel]);
    const int autor = rel == 4;
    const float aas = coef_of(0.6f), ars = coef_of(1.5f);
    const float mk = (float)vsc.p[P_CMAKE], mix = (float)vsc.p[P_CMIX] * (1.f / 127.f);
    if (!autor) comp_grs = 0.f;
    for (unsigned i = 0; i < n; i += COMP_SUB) {
        float pk = 0.f;
        for (unsigned j = 0; j < COMP_SUB; ++j) {
            float a = key ? absf(key[i + j]) : absf(l[i + j]), b = key ? 0.f : absf(r[i + j]);
            a = a > b ? a : b; pk = a > pk ? a : pk;
        }
        float db = 6.0206f * log2a(pk + 1e-6f);
        float tgt = db > thr ? (db - thr) * slope : 0.f;
        comp_gr += (tgt - comp_gr) * (tgt > comp_gr ? aa : ar);
        float gr = comp_gr;
        if (autor) { comp_grs += (tgt - comp_grs) * (tgt > comp_grs ? aas : ars); gr = gr > comp_grs ? gr : comp_grs; }
        float g1 = exp2a((mk - gr) * 0.16609640f);
        g1 = 1.f - mix + mix * g1;
        float dg = (g1 - comp_g) * (1.f / (float)COMP_SUB);
        for (unsigned j = 0; j < COMP_SUB; ++j) { comp_g += dg; l[i + j] *= comp_g; r[i + j] *= comp_g; }
        comp_db = db;
    }
    vsc_meter.comp_gr = comp_gr > comp_grs ? comp_gr : comp_grs;
    vsc_meter.comp_in = comp_db;
}
static float key_buf[VSC_BLOCK];   /* the compressor's key, when it listens to a track */

/* ------------------------------------------------------------- transport */
static VSC_HOT_SMALL unsigned armed_mask(void)
{
    unsigned m = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) if (vsc.p[P_TRK(t, S_ARM)]) m |= 1u << t;
    return m;
}
static VSC_HOT_SMALL void punch_out_all(void) { for (unsigned t = 0; t < VSC_TRACKS; ++t) wr_stop(t); }
static VSC_HOT_SMALL void bounce_end(int done)
{
    if (!bounce_on) return;
    bounce_on = 0;
    if (done) {
        if (bounce_clear) for (unsigned t = 0; t < VSC_TRACKS; ++t) if (bounce_src >> t & 1u) track_clear(t);
        /* the print plays on its own now: its strip at unity, the sources' left as they were */
        vsc_meter.bounce_dst = bounce_dst;
    }
    vsc_meter.bounce = 0;
}
static VSC_HOT_SMALL void stop(void)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t) wr_finish(t);
    if (transport == TR_FIRST) { loop_len = (uint32_t)tp + 1u; if (loop_len >= TCAP) loop_len = TCAP; }
    bounce_end(0);
    sl_rec = 0;
    if (transport == TR_FIRST) take_len = loop_len;
    transport = TR_STOP;
    tp = (float)loop_start;
    tape_seek_all(loop_start);
}
static VSC_HOT_SMALL void play(void)
{
    if (!loop_len) return;
    transport = TR_PLAY;
    tp = (float)loop_start;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) tk[t].r.on = vsc_meter.has[t];
    tape_seek_all(loop_start);
}
static VSC_HOT_SMALL void rec(void)
{
    unsigned arm = armed_mask();
    if (bounce_on) return;
    if (transport == TR_FIRST) {   /* the first take ends: the loop is set */
        uint32_t l = (uint32_t)tp + 1u;
        transport = TR_PLAY;
        punch_out_all();
        loop_set(l < 64u ? 64u : l);
        return;
    }
    if (!loop_len) {   /* an empty tape: the first take starts */
        if (!arm) return;
        for (unsigned t = 0; t < VSC_TRACKS; ++t) if (arm >> t & 1u) track_clear(t);
        tp = 0.f; transport = TR_FIRST; loop_start = 0; take_len = 0;
        for (unsigned t = 0; t < VSC_TRACKS; ++t) tk[t].tp = 0.f;
        for (unsigned t = 0; t < VSC_TRACKS; ++t) if (arm >> t & 1u) { tk[t].r.on = 1; rd_seek(t, 0); wr_start(t); }
        return;
    }
    if (transport == TR_STOP) play();
    int punching = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) punching |= tk[t].w.on && !tk[t].w.ending;
    if (punching) { punch_out_all(); return; }
    for (unsigned t = 0; t < VSC_TRACKS; ++t) if (arm >> t & 1u) wr_start(t);
}
VSC_HOT_SMALL int vsc_bounce_plan(unsigned *src_mask, unsigned *dst)
{
    unsigned src = vsc.p[P_BSRC] & 15u, d = vsc.p[P_BDST];
    unsigned have = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) if (vsc_meter.has[t]) have |= 1u << t;
    if (!loop_len) return BE_NOLOOP;
    src &= have;
    if (!src) return BE_EMPTY;
    if (d == 0) {
        d = 5;
        for (unsigned t = 0; t < VSC_TRACKS && d == 5; ++t) if (!(have >> t & 1u) && !(src >> t & 1u)) d = t + 1u;
        if (d == 5) return BE_NOROOM;
    }
    if (src >> (d - 1u) & 1u) return BE_NOROOM;
    if (src_mask) *src_mask = src;
    if (dst) *dst = d - 1u;
    return BE_NONE;
}
static VSC_HOT_SMALL void bounce_start(void)
{
    unsigned src = 0, dst = 0;
    int e = vsc_bounce_plan(&src, &dst);
    vsc_meter.bounce_err = (uint8_t)e;
    if (e) return;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) wr_finish(t);
    track_clear(dst);
    bounce_src = (uint8_t)src; bounce_dst = (uint8_t)dst; bounce_clear = vsc.p[P_BCLEAR];
    transport = TR_PLAY; tp = (float)loop_start;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) tk[t].r.on = vsc_meter.has[t] || t == dst;
    tape_seek_all(loop_start);
    struct wr *w = &tk[dst].w;
    w->pred = 0; w->idx = 0; w->on = 1; w->ending = 0; w->acc = 0.f; w->cnt = 0;
    vsc_meter.has[dst] = 1;
    bounce_on = 1; bounce_done = 0;
    vsc_meter.bounce = 1;
}

/* ---------------------------------------------------------- the intro
 * Three seconds of music at power-on: an original lo-fi chip piece (a four-on-the-floor kick,
 * a buzzing bass, a pulse arpeggio over four chords, noise hats), crushed to 4 bits. 32
 * sixteenths at 150 BPM; it fades over the last four. */
#define IN_STEP 4410u
static uint32_t in_n, in_len;
static float in_ph[3], in_kp, in_kf;
static uint32_t in_noise = 1;
static VSC_HOTDATA const int8_t in_bass[8] = {33, 33, 45, 33, 36, 33, 43, 31};
static VSC_HOTDATA const int8_t in_chord[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
VSC_INL float midi_inc(int n) { return 440.f * exp2a(((float)n - 69.f) * (1.f / 12.f)) * (1.f / (float)VSC_SR); }
static VSC_HOT_SMALL void intro(float *l, float *r, unsigned n)
{
    for (unsigned i = 0; i < n && in_n < in_len; i += 16) {   /* (the pitches a 16th of a block at a time) */
        uint32_t step = in_n / IN_STEP, pos = in_n % IN_STEP;
        float bi = midi_inc(in_bass[step & 7u] + (step >= 16 ? 2 : 0));
        float ai = midi_inc(in_chord[(step >> 3) & 3u][(in_n / 1470u) % 3u] + 12);
        float fade = step >= 28 ? 1.f - (float)(in_n - 28u * IN_STEP) / (4.f * (float)IN_STEP) : 1.f;
        for (unsigned j = i; j < i + 16 && j < n; ++j) {
            float t = (float)pos * (1.f / (float)VSC_SR);
            /* the kick, on the beat: a pitch-dropping square */
            float k = 0.f;
            if (!(step & 3u) && pos < 3000u) { in_kf = 0.0011f + 0.006f * (1.f - (float)pos / 3000.f); in_kp += in_kf; if (in_kp >= 1.f) in_kp -= 1.f; k = (in_kp < 0.5f ? 0.9f : -0.9f) * (1.f - (float)pos / 3000.f); }
            /* the bass: a square, gated to each sixteenth's first two thirds */
            in_ph[0] += bi; if (in_ph[0] >= 1.f) in_ph[0] -= 1.f;
            float b = pos < 2900u ? (in_ph[0] < 0.5f ? 0.5f : -0.5f) : 0.f;
            /* the arpeggio: a 25 % pulse, decaying in each sixteenth */
            in_ph[1] += ai; if (in_ph[1] >= 1.f) in_ph[1] -= 1.f;
            float a = (in_ph[1] < 0.25f ? 0.35f : -0.12f) * (1.f - t * 7.f > 0.f ? 1.f - t * 7.f : 0.f);
            /* hats on the offbeats */
            in_noise ^= in_noise << 13; in_noise ^= in_noise >> 17; in_noise ^= in_noise << 5;
            float h = (step & 1u) && pos < 900u ? ((float)(in_noise >> 24) * (1.f / 128.f) - 1.f) * 0.25f * (1.f - (float)pos / 900.f) : 0.f;
            float v = (k + b + a + h) * 0.32f * fade;
            v = (float)(int)(v * 8.f + (v >= 0.f ? 0.5f : -0.5f)) * (1.f / 8.f);   /* 4 bits */
            l[j] += v; r[j] += v;
            ++in_n; ++pos;
            if (pos >= IN_STEP) { pos = 0; step = in_n / IN_STEP; in_kp = 0.f; }
        }
    }
}

/* ---------------------------------------------------------- SLICE
 * A track's loop cut into slices the keys play. While the page is open the track is muted and
 * the slices play through its strip; REC records them into a free track (REPLACE). Two voices,
 * so a new slice crossfades over the last; each reads the tape on its own (from its block's
 * start, then on), at the source track's speed. */
uint32_t vsc_slice_at[17];
volatile uint8_t vsc_slice_n;
struct svoice { uint32_t n, end; float fr, s0, s1, env; int32_t pred, idx; uint8_t on, fade; };
static struct svoice sv[2];
static uint8_t sv_next;
static float sl_buf[VSC_SUB];
static VSC_HOT_SMALL void slice_play(unsigned k, float vel)
{
    unsigned nsl = vsc_slice_n;
    if (sl_src >= VSC_TRACKS || !vsc_meter.has[sl_src] || !nsl || k >= nsl) return;
    uint32_t a = vsc_slice_at[k], e = vsc_slice_at[k + 1];
    if (e <= a + 2u || e > TCAP) return;
    for (unsigned v = 0; v < 2; ++v) if (sv[v].on) sv[v].fade = 1;   /* the last one lets go */
    struct svoice *s = &sv[sv_next]; sv_next ^= 1u;
    uint32_t b = a / VSC_TAPE_BLK;
    s->pred = hpred[sl_src][b]; s->idx = hidx[sl_src][b];
    for (uint32_t j = b * VSC_TAPE_BLK; j < a; ++j) (void)dec(tape[sl_src], j, &s->pred, &s->idx);
    s->s0 = dec(tape[sl_src], a, &s->pred, &s->idx);
    s->s1 = dec(tape[sl_src], a + 1u, &s->pred, &s->idx);
    s->n = a; s->end = e; s->fr = 0.f; s->env = 0.f; s->on = 1; s->fade = 0;
    (void)vel;
}
/* the voices, a sub-block: into sl_buf */
static VSC_HOT void slice_voices(unsigned n, float inc)
{
    for (unsigned i = 0; i < n; ++i) sl_buf[i] = 0.f;
    unsigned live = 0;
    for (unsigned v = 0; v < 2; ++v) {
        struct svoice *s = &sv[v];
        if (!s->on) continue;
        ++live;
        for (unsigned i = 0; i < n; ++i) {
            s->fr += inc;
            while (s->fr >= 1.f) {
                s->fr -= 1.f; ++s->n;
                s->s0 = s->s1;
                s->s1 = s->n + 1u < s->end ? dec(tape[sl_src], s->n + 1u, &s->pred, &s->idx) : 0.f;
                if (s->n + 64u >= s->end) s->fade = 1;   /* its end: a short fade, then off */
            }
            if (s->fade) { s->env -= 1.f / 96.f; if (s->env <= 0.f) { s->env = 0.f; s->on = 0; break; } }
            else if (s->env < 1.f) s->env += 1.f / 48.f;
            sl_buf[i] += (s->s0 + (s->s1 - s->s0) * s->fr) * s->env;
        }
    }
    vsc_meter.sl_on = (uint8_t)live;
}
/* where a take goes: P_SLDST (AUTO: the first empty track that isn't the source) */
static VSC_HOT_SMALL int slice_dst(void)
{
    unsigned d = vsc.p[P_SLDST];
    if (d) return d - 1u == sl_src ? -1 : (int)(d - 1u);
    for (unsigned t = 0; t < VSC_TRACKS; ++t) if (t != sl_src && !vsc_meter.has[t]) return (int)t;
    return -1;
}
static VSC_HOT_SMALL void slice_rec(int on);
/* (control task) the tape's level in frames of 256 samples, from its block's start */
unsigned vsc_tape_env(unsigned t, uint32_t from, uint32_t to, uint8_t *env, unsigned max)
{
    if (t >= VSC_TRACKS || to > TCAP || from >= to) return 0;
    uint32_t b = from / VSC_TAPE_BLK;
    int32_t p = hpred[t][b], x = hidx[t][b];
    for (uint32_t j = b * VSC_TAPE_BLK; j < from; ++j) (void)dec(tape[t], j, &p, &x);
    unsigned nf = 0; float acc = 0.f; unsigned c = 0;
    for (uint32_t j = from; j < to && nf < max; ++j) {
        if (!(j & (VSC_TAPE_BLK - 1u))) { p = hpred[t][j / VSC_TAPE_BLK]; x = hidx[t][j / VSC_TAPE_BLK]; }
        acc += absf(dec(tape[t], j, &p, &x));
        if (++c == 256u) { float v = acc * (1.f / 256.f) * 600.f; env[nf++] = (uint8_t)(v > 255.f ? 255.f : v); acc = 0.f; c = 0; }
    }
    return nf;
}

static VSC_HOT_SMALL void slice_rec(int on)
{
    if (!on) { if (sl_rec && sl_dst < VSC_TRACKS) wr_stop(sl_dst); sl_rec = 0; return; }
    if (sl_rec || sl_src >= VSC_TRACKS || bounce_on) return;
    if (!loop_len) { vsc_meter.sl_err = BE_NOLOOP; return; }
    int d = slice_dst();
    if (d < 0) { vsc_meter.sl_err = BE_NOROOM; return; }
    vsc_meter.sl_err = BE_NONE;
    sl_dst = (uint8_t)d;
    if (transport == TR_STOP) play();
    wr_start(sl_dst);
    sl_rec = 1;
}

/* ---------------------------------------------------------- MIDI learn */
static uint8_t learn_held[VSC_LEARN];
static int16_t learn_last[VSC_LEARN];
void vsc_learn_release(void) { for (unsigned l = 0; l < VSC_LEARN; ++l) { learn_held[l] = 0; learn_last[l] = -1; } }
static VSC_HOT_SMALL void action(unsigned a)
{
    if (a == ACT_REC) rec();
    else if (a == ACT_PLAY) { if (transport != TR_STOP) stop(); else play(); }
    else if (a >= ACT_ARM1 && a <= ACT_ARM4) {   /* one track armed at a time: arming one disarms the rest */
        unsigned k = a - ACT_ARM1, p = P_TRK(k, S_ARM);
        vsc.p[p] ^= 1u;
        if (!vsc.p[p]) wr_stop(k);
        else for (unsigned t = 0; t < VSC_TRACKS; ++t) if (t != k && vsc.p[P_TRK(t, S_ARM)]) { vsc.p[P_TRK(t, S_ARM)] = 0; wr_stop(t); }
    }
    else if (a == ACT_BOUNCE) bounce_start();
}
static VSC_HOT_SMALL int learn_take(unsigned ch, unsigned num, unsigned kind)
{
    if (!vsc_learn_armed || vsc_learn_kind != kind) return 0;
    unsigned slot = VSC_LEARN;
    for (unsigned l = 0; l < VSC_LEARN; ++l) {
        const struct vsc_learn *e = &vsc_dev.learn[l];
        if (e->param != 255 && e->kind == kind && e->ch == ch && e->cc == num) { slot = l; break; }
    }
    for (unsigned l = 0; l < VSC_LEARN && slot == VSC_LEARN; ++l) if (vsc_dev.learn[l].param == 255) slot = l;
    if (slot == VSC_LEARN) slot = 0;
    struct vsc_learn *e = &vsc_dev.learn[slot];
    e->ch = (uint8_t)ch; e->cc = (uint8_t)num; e->kind = (uint8_t)kind; e->param = vsc_learn_param;
    learn_held[slot] = 1; learn_last[slot] = -1;
    vsc_learn_armed = 0; ++vsc_learn_seen; ++vsc_dev_changed;
    return 1;
}
static VSC_HOT_SMALL void midi_cc(unsigned ch, unsigned cc, unsigned v)
{
    if (learn_take(ch, cc, 0)) return;
    for (unsigned l = 0; l < VSC_LEARN; ++l) {
        const struct vsc_learn *e = &vsc_dev.learn[l];
        if (e->param == 255 || e->kind != 0 || e->ch != ch || e->cc != cc || e->param >= VSC_NP) continue;
        unsigned max = pmax[e->param];
        int target = (int)((v * max + 63u) / 127u), cur = vsc.p[e->param], now127 = (int)((cur * 127u + max / 2u) / (max ? max : 1u));
        int prev = learn_last[l];
        /* picks up where the parameter is before it moves it */
        if (!learn_held[l] && ((int)v - now127 <= 2 && now127 - (int)v <= 2)) learn_held[l] = 1;
        if (!learn_held[l] && prev >= 0 && ((prev <= now127 && (int)v >= now127) || (prev >= now127 && (int)v <= now127))) learn_held[l] = 1;
        learn_last[l] = (int16_t)v;
        if (learn_held[l]) vsc.p[e->param] = (uint8_t)target;
    }
}
static VSC_HOT_SMALL void midi_note(unsigned ch, unsigned note, unsigned vel)
{
    if (learn_take(ch, note, 1)) return;
    for (unsigned l = 0; l < VSC_LEARN; ++l) {
        const struct vsc_learn *e = &vsc_dev.learn[l];
        if (e->param != 255 && e->kind == 1 && e->ch == ch && e->cc == note) { action(e->param); return; }
    }
    rs_note = (float)note - 45.f;
    rs_req = 0.3f + 0.7f * (float)vel * (1.f / 127.f);
}

/* -------------------------------------------------------------- events */
static uint8_t from_midi;
static VSC_HOT_SMALL void handle(uint32_t e)
{
    unsigned type = e & 255u, a = (e >> 8) & 255u, b = e >> 16;
    switch (type) {
    case EV_NOTE_ON:   /* from MIDI: a the channel, b the note and velocity; the keys: a the note, b the velocity */
        if (from_midi) midi_note(a, b & 127u, (b >> 8) & 127u);
        else { rs_note = (float)a - 45.f; rs_req = 0.3f + 0.7f * (float)(b & 127u) * (1.f / 127.f); }
        break;
    case EV_PLAY:   /* in the first take: it ends, and the loop plays */
        if (transport == TR_FIRST) rec();
        else if (a == 1) { if (transport == TR_STOP) play(); }
        else if (transport != TR_STOP) stop(); else play();
        break;
    case EV_STOP: if (transport != TR_STOP) stop(); break;
    case EV_REC: rec(); break;
    case EV_CLEAR:
        if (a < VSC_TRACKS) {
            if (bounce_on && (a == bounce_dst || (bounce_src >> a & 1u))) bounce_end(0);
            track_clear(a);
            unsigned any = 0;
            for (unsigned t = 0; t < VSC_TRACKS; ++t) any |= vsc_meter.has[t];
            if (!any && transport != TR_FIRST) { stop(); loop_len = loop_start = take_len = 0; }
        }
        break;
    case EV_ERASE:
        stop();
        for (unsigned t = 0; t < VSC_TRACKS; ++t) track_clear(t);
        loop_len = loop_start = take_len = 0;
        break;
    case EV_BOUNCE: bounce_start(); break;
    case EV_ARM: if (a < VSC_TRACKS) action(ACT_ARM1 + a); break;
    case EV_LOOP:   /* a: clicks (signed); b: 0 END, 1 START, 2 SLIDE (both), 3 FULL (the whole take); END
                     * moves in twentieths of a second, START and SLIDE in hundredths */
        if (loop_len && transport != TR_FIRST) {
            float rate = tape_rate(vsc.p[P_TAPESPD]);
            int32_t d = (int32_t)(int8_t)a * (int32_t)(rate * (b == 0 ? 0.05f : 0.01f));
            int32_t st = (int32_t)loop_start, en = (int32_t)loop_len, lo = (int32_t)(rate * 0.05f);
            if (b == 0) en += d;
            else if (b == 1) st += d;
            else if (b == 2) { if (st + d < 0) d = -st; if (en + d > (int32_t)TCAP) d = (int32_t)TCAP - en; st += d; en += d; }
            else { st = 0; en = (int32_t)(take_len ? take_len : loop_len); }
            if (en > (int32_t)TCAP) en = (int32_t)TCAP;
            if (st < 0) st = 0;
            if (en - st < lo) { if (b == 1) st = en - lo; else en = st + lo; }
            if (st < 0) { st = 0; en = lo; }
            loop_start = (uint32_t)st;
            loop_set((uint32_t)en);
        }
        break;
    case EV_SLICE: slice_play(a, (float)(b & 127u) * (1.f / 127.f)); break;
    case EV_SLMODE:
        if (!a) { slice_rec(0); sl_src = 255; sv[0].on = sv[1].on = 0; }
        else if (a <= VSC_TRACKS && sl_src != a - 1u) { slice_rec(0); sl_src = (uint8_t)(a - 1u); sv[0].on = sv[1].on = 0; }
        break;
    case EV_SLREC: slice_rec(a != 0); break;
    case EV_INTRO: in_n = 0; in_len = a ? 32u * IN_STEP : 0u; in_ph[0] = in_ph[1] = in_kp = 0.f; break;
    case EV_PANIC: rs_req = 0.f; vsc_memset(&rs, 0, offsetof(__typeof__(rs), ring)); rs.where = 255; break;
    default: break;
    }
}
static VSC_HOT_SMALL void drain(struct queue *q)
{
    uint32_t r = q->r;
    while (r != q->w) {
        __asm__ volatile("" ::: "memory");
        uint32_t e = q->d[r & (QN - 1)];
        if ((e & 255u) == EV_CC) midi_cc((e >> 8) & 15u, (e >> 16) & 127u, (e >> 24) & 127u);   /* a the channel, b the CC and its value */
        else handle(e);
        q->r = ++r;
    }
}

/* -------------------------------------------------------------- render */
__attribute__((minsize)) void vsc_init(void)
{
    ulaw_build();
    vsc_device_defaults();
    vsc_learn_release();
    for (unsigned p = 0; p < VSC_NP; ++p) { is_enum[p] = (vsc_param_info(p)->flags & VSC_PF_ENUM) != 0; pmax[p] = vsc_param_info(p)->max; }
    vsc_memset(tk, 0, sizeof tk);
    for (unsigned t = 0; t < VSC_TRACKS; ++t) track_clear(t);
    rs.where = 255;
    rob_hold[0][1] = 0.3f; rob_hold[1][1] = -0.4f;
    vsc_meter.cap = TCAP;
}
#ifdef VSC_BENCH
/* a CPU benchmark build only: four full tracks playing, one overdubbing, the pitch
 * shifter, and RESON on a track (6, 7: DUST) */
void vsc_bench_setup(void)
{
    loop_len = TCAP;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        for (uint32_t i = 0; i < TCAP; ++i) {
            float v = 0.4f * (float)((int)(rnd() >> 24) - 128) * (1.f / 128.f) * (float)((i >> 6) & 7) * 0.14f;
            if (!(i & (VSC_TAPE_BLK - 1u))) { hpred[t][i / VSC_TAPE_BLK] = (int16_t)tk[t].w.pred; hidx[t][i / VSC_TAPE_BLK] = (uint8_t)tk[t].w.idx; }
            enc(tape[t], i, v, &tk[t].w.pred, &tk[t].w.idx);
        }
        vsc_meter.has[t] = 1;
        vsc.p[P_TRK(t, S_TRIM)] = 110;   /* into the preamps' knee */
        vsc.p[P_TRK(t, S_ARM)] = t == 0;
    }
    vsc.p[P_PITCH] = 17; vsc.p[P_SPEED] = 80;
#if VSC_BENCH >= 6
    vsc.p[P_RTGT] = RT_T2;
    vsc_reson_profile(VSC_BENCH == 7 ? RP_DUST : RP_LIQUID);
#endif
    play();
    rec();
}
#endif
static float main_l[VSC_BLOCK], main_r[VSC_BLOCK];
static float vu_lp[2];
VSC_HOT void vsc_render(const struct vsc_inputs *in, float *out)
{
    uint32_t t_start = vsc_clock_now ? vsc_clock_now() : 0, t_reson = 0;
    from_midi = 0; drain(&q_ctrl);
    from_midi = 1; drain(&q_midi);
    from_midi = 0;
    robots();

    const unsigned isrc = vsc.p[P_ISRC];
    const float *bt = (isrc == IN_BT || isrc == IN_BOTH) ? in->bt : 0;
    const float *usb = (isrc == IN_USB || isrc == IN_BOTH) ? in->usb : 0;
    vsc_meter.bt_in = in->bt != 0; vsc_meter.usb_in = in->usb != 0;
    const float itrim = trim_gain(pf[P_ITRIM]), idr = pf[P_IDRIVE] * (1.f / 127.f);
    const float ipre = itrim * (1.f + 5.f * idr * idr), ipost = 1.f / (1.f + 2.f * idr * idr), ieven = 0.02f + 0.12f * idr;
    const unsigned arm = armed_mask();
    const unsigned mon = vsc.p[P_IMON] == MON_ON || (vsc.p[P_IMON] == MON_AUTO && arm);
    const int replace = vsc.p[P_IREC] == IREC_REPLACE;
    const unsigned rtgt = vsc.p[P_RTGT];
    const int rs_bounce = bounce_on && vsc.p[P_BRESON];
    const float bpre = 1.f + 5.f * (pf[P_BDRIVE] * pf[P_BDRIVE]) * (1.f / 16129.f), bpost = 1.f / (1.f + 2.f * pf[P_BDRIVE] * pf[P_BDRIVE] * (1.f / 16129.f));
    const float beven = 0.02f + 0.12f * pf[P_BDRIVE] * (1.f / 127.f), blev = level_gain(pf[P_BLEVEL]);
    const float tape_hz = tape_rate(vsc.p[P_TAPESPD]);
    const float speed = exp2a((pf[P_SPEED] - 64.f) * (1.f / 64.f));
    const float inc = clampf(tape_hz * speed * (1.f / (float)VSC_SR), 0.f, 1.f);
    vsc_meter.rate = tape_hz * speed;
    const float head = clampf(tape_hz * speed * (6.2831853f * 0.42f / (float)VSC_SR), 0.05f, 1.f);   /* the head's roll-off: about 0.42 of the tape's rate */
    float g_trim[VSC_TRACKS], g_l[VSC_TRACKS], g_r[VSC_TRACKS];
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        g_trim[t] = trim_gain(pf[P_TRK(t, S_TRIM)]);
        float lv = vsc.p[P_TRK(t, S_MUTE)] ? 0.f : level_gain(pf[P_TRK(t, S_LEVEL)]), pan = (pf[P_TRK(t, S_PAN)] - 64.f) * (1.f / 64.f);
        pan = clampf(pan, -1.f, 1.f);
        g_l[t] = lv * (pan > 0.f ? 1.f - pan : 1.f); g_r[t] = lv * (pan < 0.f ? 1.f + pan : 1.f);
    }
    const float tlev = level_gain(pf[P_TLEVEL]);
    const float semis = (pf[P_PITCH] - 12.f);
    float t_semis[VSC_TRACKS];
    /* each track's step: the tape's, times its own SPEED (all together in the first take; a
     * bounce's print at the tape's), at most a sample a frame */
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        struct trk *k = &tk[t];
        float ts = exp2a((pf[P_TRK(t, S_SPEED)] - 64.f) * (1.f / 64.f));
        k->inc = transport == TR_FIRST || (bounce_on && t == bounce_dst) ? inc : clampf(inc * ts, 0.f, 1.f);
        k->dg = pf[P_TRK(t, S_DUB)] * (1.f / 127.f);
        t_semis[t] = semis + pf[P_TRK(t, S_PITCH)] - 12.f;
    }
    const int keyed = vsc.p[P_CON] && vsc.p[P_CKEY] >= 1 && vsc.p[P_CKEY] <= VSC_TRACKS;
    float ibuf[VSC_SUB], bbuf[VSC_SUB];
    float iacc = 0.f, ipk = 0.f, tacc[VSC_TRACKS] = {0}, tpk[VSC_TRACKS] = {0};
    static float idc, bdc;
    uint8_t ihit = 0, thit[VSC_TRACKS] = {0}, bhit = 0;
    for (unsigned off = 0; off < VSC_BLOCK; off += VSC_SUB) {
        const unsigned n = VSC_SUB;
        /* the input: mono, trimmed, through its preamp */
        for (unsigned i = 0; i < n; ++i) {
            unsigned j = off + i;
            float v = 0.f;
            if (bt) v += (bt[2 * j] + bt[2 * j + 1]) * 0.5f;
            if (usb) v += (usb[2 * j] + usb[2 * j + 1]) * 0.5f;
            v = pre(v * ipre, ieven, &idc, &ihit) * ipost;
            ibuf[i] = v;
            float a = absf(v); iacc += a; if (a > ipk) ipk = a;
        }
        if (rtgt == RT_INPUT && !rs_bounce) {
            uint32_t r0 = vsc_clock_now ? vsc_clock_now() : 0;
            reson(ibuf, ibuf, n, RT_INPUT);
            if (vsc_clock_since) t_reson += vsc_clock_since(r0);
        }
        /* pass A: the tape moves; each track reads (a sample ahead) at its own speed */
        int moving = transport != TR_STOP, wrap_i = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (!moving) { for (unsigned t = 0; t < VSC_TRACKS; ++t) { tk[t].buf[i] = 0.f; tk[t].cr[i] = 0; } continue; }
            /* the master: the loop's time */
            float p = tp + inc;
            if ((uint32_t)p != (uint32_t)tp) {
                uint32_t e = loop_end();
                if (p >= (float)e) {
                    p -= (float)(e - loop_start);
                    if (transport == TR_FIRST) {   /* the tape's end: the first take closes */
                        loop_len = TCAP; transport = TR_PLAY; punch_out_all();
                    }
                    if (bounce_on && bounce_done) wrap_i = (int)i;
                } else if (transport == TR_FIRST && vsc.p[P_LOOP] && p >= tape_hz * 0.05f * (float)vsc.p[P_LOOP]) {
                    /* a set length: the first take closes there (the tracks are where the master is) */
                    uint32_t l = (uint32_t)p; if (l > TCAP) l = TCAP;
                    punch_out_all(); transport = TR_PLAY;
                    for (unsigned t = 0; t < VSC_TRACKS; ++t) tk[t].tp = tp;
                    tp = p; loop_set(l); p = tp;
                }
                if (bounce_on) ++bounce_done;
            }
            tp = p;
            const uint32_t e = loop_end();
            for (unsigned t = 0; t < VSC_TRACKS; ++t) {
                struct trk *k = &tk[t];
                k->cr[i] = 0;
                if (!k->r.on) { k->buf[i] = 0.f; continue; }
                float q = k->tp + k->inc;
                if ((uint32_t)q != (uint32_t)k->tp) {
                    k->cr[i] = 1;
                    k->old[i] = k->r.s0; k->oldn[i] = k->r.n0;
                    rd_fetch(t);
                    if (q >= (float)e) q -= (float)(e - loop_start);
                }
                k->tp = q;
                if (replace && k->w.on && !k->w.ending && !bounce_on) { k->buf[i] = 0.f; continue; }
                if (t == sl_src) continue;   /* (SLICE: the source's tape isn't heard; its slices are) */
                float v = k->r.s0 + (k->r.s1 - k->r.s0) * (q - (float)(uint32_t)q);
                k->lp += (v - k->lp) * head;
                k->buf[i] = k->lp;
            }
        }
        /* SLICE: the voices play through the source's strip, in place of its tape */
        const int slicing = sl_src < VSC_TRACKS;
        if (slicing) {
            slice_voices(n, tk[sl_src].inc > 0.f ? tk[sl_src].inc : inc);
            for (unsigned i = 0; i < n; ++i) tk[sl_src].buf[i] = sl_buf[i];
        }
        /* RESON on a track's playback (before its strip) */
        if (rtgt >= RT_T1 && rtgt <= RT_T4 && !rs_bounce) {
            uint32_t r0 = vsc_clock_now ? vsc_clock_now() : 0;
            reson(tk[rtgt - RT_T1].buf, ibuf, n, rtgt);
            if (vsc_clock_since) t_reson += vsc_clock_since(r0);
        }
        /* the strips: trim into the preamp; the bounce's sum */
        for (unsigned i = 0; i < n; ++i) bbuf[i] = 0.f;
        for (unsigned t = 0; t < VSC_TRACKS; ++t) {
            struct trk *k = &tk[t];
            if (!k->r.on && !(slicing && t == sl_src)) continue;
            float acc = 0.f, pk = 0.f;
            const float bg = bounce_on && (bounce_src >> t & 1u) ? level_gain(pf[P_TRK(t, S_LEVEL)]) : 0.f;
            for (unsigned i = 0; i < n; ++i) k->buf[i] = pre(k->buf[i] * g_trim[t], 0.04f, &k->dc, &thit[t]);
            shifter(k, k->buf, n, t_semis[t]);
            for (unsigned i = 0; i < n; ++i) {
                float v = k->buf[i];
                float a = absf(v); acc += a; if (a > pk) pk = a;
                bbuf[i] += v * bg;
            }
            if (keyed && t == vsc.p[P_CKEY] - 1u) for (unsigned i = 0; i < n; ++i) key_buf[off + i] = k->buf[i];
            tacc[t] += acc; if (pk > tpk[t]) tpk[t] = pk;
        }
        if (bounce_on) {
            if (rs_bounce) {
                uint32_t r0 = vsc_clock_now ? vsc_clock_now() : 0;
                reson(bbuf, ibuf, n, 6);
                if (vsc_clock_since) t_reson += vsc_clock_since(r0);
            }
            for (unsigned i = 0; i < n; ++i) bbuf[i] = pre(bbuf[i] * bpre, beven, &bdc, &bhit) * bpost * blev;
        }
        /* pass B: the writers take this sub-block's input (or the bounce) */
        for (unsigned t = 0; t < VSC_TRACKS; ++t) {
            struct trk *k = &tk[t];
            if (!k->w.on || !moving) continue;
            const int bw = bounce_on && t == bounce_dst;
            const float *src = bw ? bbuf : sl_rec && t == sl_dst && slicing ? tk[sl_src].buf : ibuf;
            for (unsigned i = 0; i < n; ++i) {
                if (k->cr[i] && k->w.on) {
                    float v = k->w.cnt ? k->w.acc / (float)k->w.cnt : 0.f;
                    wr_put(t, k->oldn[i], k->old[i], v, replace || bw || (sl_rec && t == sl_dst));
                    k->w.acc = 0.f; k->w.cnt = 0;
                    if (bw && (int)i == wrap_i) { k->w.on = 0; break; }   /* the bounce's last sample */
                }
                k->w.acc += src[i]; ++k->w.cnt;
            }
        }
        if (wrap_i >= 0) bounce_end(1);
        /* the mix */
        for (unsigned i = 0; i < n; ++i) {
            float l = 0.f, r = 0.f;
            for (unsigned t = 0; t < VSC_TRACKS; ++t) {
                if ((!tk[t].r.on && !(slicing && t == sl_src)) || (bounce_on && ((bounce_src >> t & 1u) || t == bounce_dst))) continue;
                if (sl_rec && t == sl_dst) continue;   /* (the take: heard through the source as it's made) */
                float v = tk[t].buf[i];
                l += v * g_l[t]; r += v * g_r[t];
            }
            if (bounce_on) { l += bbuf[i] * 0.7f; r += bbuf[i] * 0.7f; }
            main_l[off + i] = l * tlev; main_r[off + i] = r * tlev;
        }
        /* the overview: each track's level where the tape is */
        if (moving) {
            uint32_t e = transport == TR_FIRST ? TCAP : take_len > loop_len ? take_len : loop_len;
            for (unsigned t = 0; t < VSC_TRACKS; ++t) {
                unsigned col = (unsigned)(tk[t].tp * 60.f / (float)(e ? e : 1u));
                if (!tk[t].r.on || col >= 60u) continue;
                float m = 0.f; for (unsigned i = 0; i < n; ++i) { float a = absf(tk[t].buf[i]); if (a > m) m = a; }
                unsigned v = (unsigned)(m * 30.f); vsc_meter.overview[t][col] = (uint8_t)(v > 15u ? 15u : v);
            }
        }
        /* the monitor */
        if (mon) for (unsigned i = 0; i < n; ++i) { main_l[off + i] += ibuf[i] * 0.8f; main_r[off + i] += ibuf[i] * 0.8f; }
    }
    if (keyed && !tk[vsc.p[P_CKEY] - 1u].r.on) vsc_memset(key_buf, 0, sizeof key_buf);
    if (in_n < in_len) intro(main_l, main_r, VSC_BLOCK);
    eq_run(main_l, main_r, VSC_BLOCK);
    comp_run(main_l, main_r, VSC_BLOCK, keyed ? key_buf : 0);
    float lacc = 0.f, racc = 0.f;
    for (unsigned i = 0; i < VSC_BLOCK; ++i) {
        float l = main_l[i], r = main_r[i];
        if (!(l == l)) l = 0.f;
        if (!(r == r)) r = 0.f;
        l = clampf(l, -1.f, 1.f); r = clampf(r, -1.f, 1.f);
        out[2 * i] = l; out[2 * i + 1] = r;
        lacc += absf(l); racc += absf(r);
    }
    /* the meters: VU (300 ms), peaks, the preamps' lamps */
    const float vk = (float)VSC_BLOCK / (0.3f * (float)VSC_SR), inv = 1.f / (float)VSC_BLOCK;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        vsc_meter.vu[t] += (tacc[t] * inv - vsc_meter.vu[t]) * vk;
        vsc_meter.pk[t] = tpk[t] > vsc_meter.pk[t] ? tpk[t] : vsc_meter.pk[t] * 0.96f;
        vsc_meter.sat[t] = thit[t] ? 255 : vsc_meter.sat[t] > 4 ? (uint8_t)(vsc_meter.sat[t] - 4) : 0;
    }
    vsc_meter.vu_in += (iacc * inv - vsc_meter.vu_in) * vk;
    vsc_meter.pk_in = ipk > vsc_meter.pk_in ? ipk : vsc_meter.pk_in * 0.96f;
    vsc_meter.sat_in = ihit ? 255 : vsc_meter.sat_in > 4 ? (uint8_t)(vsc_meter.sat_in - 4) : 0;
    vu_lp[0] += (lacc * inv - vu_lp[0]) * vk; vu_lp[1] += (racc * inv - vu_lp[1]) * vk;
    vsc_meter.vu_l = vu_lp[0]; vsc_meter.vu_r = vu_lp[1];
    (void)bhit;
    unsigned recm = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) if (tk[t].w.on && !tk[t].w.ending) recm |= 1u << t;
    vsc_meter.recording = (uint8_t)recm;
    vsc_meter.sl_rec = sl_rec; vsc_meter.sl_dst = sl_dst;
    vsc_meter.transport = transport;
    vsc_meter.playing = transport != TR_STOP;
    /* (for the screen: places within the loop, its length; and where it sits in the take) */
    vsc_meter.pos = (uint32_t)tp - (tp >= (float)loop_start ? loop_start : 0u); vsc_meter.loop = loop_len ? loop_len - loop_start : 0u; vsc_meter.cap = TCAP;
    vsc_meter.lstart = loop_start; vsc_meter.lend = loop_len; vsc_meter.take = transport == TR_FIRST ? (uint32_t)tp + 1u : take_len > loop_len ? take_len : loop_len;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) vsc_meter.tpos[t] = (uint32_t)tk[t].tp - ((uint32_t)tk[t].tp >= loop_start ? loop_start : 0u);
    vsc_meter.bounce_pct = bounce_on && loop_len ? (uint8_t)((float)bounce_done * 100.f / (float)(loop_len - loop_start)) : 0;
    if (vsc_clock_since) {
        uint32_t all = vsc_clock_since(t_start);
        vsc_meter.part_ticks[1] = t_reson;
        vsc_meter.part_ticks[0] = all > t_reson ? all - t_reson : 0;
        vsc_meter.part_ticks[2] = 0;
    }
}
