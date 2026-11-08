/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: the audio engine. Everything here runs in the audio interrupt on
 * the FM-1, so it is either VSC_HOT (placed in RAM) or always inlined, uses
 * no C library and only single-precision float. */
#include "vsc.h"

struct vsc_project vsc;
struct vsc_meter vsc_meter;
uint8_t vsc_lock[VSC_TRACKS][VSC_NP];
float vsc_scope[2][128];

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
VSC_INL float tanpre(float x) { float x2 = x * x; return x * (15.f - x2) / (15.f - 6.f * x2); }

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
    /* color */
    float comp_env, hold, hold_phase, tone_lp, dust;
    /* mods */
    float lfo_phase, rnd_val, rnd_target, rnd_phase, follow, env, sh;
    uint8_t env_stage, gates;
    /* step gate */
    int32_t gate_left; uint8_t gate_note;
    /* computed parameters, 0..1 for continuous ones */
    float e[VSC_NP];
    float out[VSC_BLOCK];       /* this block's mono output (post color) */
    float prev[VSC_BLOCK];      /* last block's, for resampling */
    float in[VSC_BLOCK];
    float pan_l, pan_r, send;
};
static struct track tr[VSC_TRACKS];
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
    struct track *k = &tr[t];
    unsigned mode = vsc.t[t][P_MODE];
    k->gates++;
    k->env_stage = 1;
    if (vsc.t[t][P_M0D]) k->lfo_phase = 0.f;
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
        k->ipos = a + (uint32_t)((uint64_t)(b - a) * slice / 16u);
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
    if (!k->gates) k->env_stage = 3;
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
static VSC_HOT void compute_params(unsigned t)
{
    struct track *k = &tr[t];
    const uint8_t *base = vsc.t[t];
    const struct vsc_scene *sc = &vsc.scene[vsc.g[G_TARGET] % VSC_SCENES];
    float morph = sc->used ? (float)vsc.g[G_MORPH] * (1.f / 127.f) : 0.f;
    float mods[VSC_MODS];
    for (unsigned m = 0; m < VSC_MODS; ++m) mods[m] = vsc_meter.mod_value[t][m];
    for (unsigned p = 0; p < VSC_NP; ++p) {
        unsigned v = vsc_lock[t][p] != 255 ? vsc_lock[t][p] : base[p];
        if (is_enum[p]) { k->e[p] = (float)v; continue; }
        float x = (float)v * (1.f / 127.f);
        if (morph > 0.f) x += ((float)sc->t[t][p] * (1.f / 127.f) - x) * morph;
        k->e[p] = x;
    }
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
        const struct vsc_route *ro = &vsc.route[t][r];
        if (ro->param >= VSC_NP || !ro->depth) continue;
        k->e[ro->param] += (float)ro->depth * (1.f / 127.f) * mods[ro->src & 3];
    }
    for (unsigned m = 0; m < VSC_MACROS; ++m) {
        float mv = (float)vsc.g[G_M0 + m] * (1.f / 127.f);
        if (mv <= 0.f) continue;
        for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r) {
            const struct vsc_mroute *ro = &vsc.macro[m][r];
            if (ro->track == t && ro->param < VSC_NP && ro->depth)
                k->e[ro->param] += (float)ro->depth * (1.f / 127.f) * mv;
        }
    }
    for (unsigned p = 0; p < VSC_NP; ++p)
        if (!is_enum[p]) k->e[p] = clampf(k->e[p], 0.f, 1.f);
}

/* --------------------------------------------------------------- mods */
static VSC_HOTDATA const float sync_beats[8] = {0.f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f};
static VSC_HOT void run_mods(unsigned t, unsigned frames)
{
    struct track *k = &tr[t];
    const uint8_t *b = vsc.t[t];
    float dt = (float)frames * (1.f / (float)VSC_SR);
    /* LFO */
    unsigned sync = b[P_M0C] & 7u;
    float hz = sync ? (float)vsc.tempo * (1.f / 60.f) / sync_beats[sync]
                    : 0.02f * exp2a((float)b[P_M0B] * (1.f / 127.f) * 10.f);
    float prev = k->lfo_phase;
    k->lfo_phase += hz * dt;
    if (k->lfo_phase >= 1.f) { k->lfo_phase -= (float)floori(k->lfo_phase); k->sh = rndb(); }
    (void)prev;
    float p = k->lfo_phase, l;
    switch (b[P_M0A]) {
    case 0: l = sin01(p); break;
    case 1: l = p < 0.5f ? p * 4.f - 1.f : 3.f - p * 4.f; break;
    case 2: l = 1.f - 2.f * p; break;
    case 3: l = 2.f * p - 1.f; break;
    case 4: l = p < 0.5f ? 1.f : -1.f; break;
    default: l = k->sh; break;
    }
    vsc_meter.mod_value[t][0] = l;
    /* RANDOM */
    float rhz = 0.1f * exp2a((float)b[P_M1A] * (1.f / 127.f) * 8.f);
    k->rnd_phase += rhz * dt;
    if (k->rnd_phase >= 1.f) { k->rnd_phase -= 1.f; k->rnd_target = rndb(); }
    float sm = (float)b[P_M1B] * (1.f / 127.f);
    float coef = 1.f - sm * 0.97f;
    k->rnd_val += (k->rnd_target - k->rnd_val) * (sm < 0.01f ? 1.f : coef * coef * 0.5f);
    vsc_meter.mod_value[t][1] = k->rnd_val;
    /* FOLLOW: peak of this block's material, smoothed */
    float pk = 0.f;
    for (unsigned i = 0; i < frames; ++i) { float a = absf(k->out[i]); if (a > pk) pk = a; }
    pk *= 0.5f + 3.f * (float)b[P_M2C] * (1.f / 127.f);
    float att = 0.002f + 0.5f * (float)b[P_M2A] * (1.f / 127.f);
    float rel = 0.01f + 2.f * (float)b[P_M2B] * (1.f / 127.f);
    float c = pk > k->follow ? dt / (att + dt) : dt / (rel + dt);
    k->follow += (pk - k->follow) * c;
    vsc_meter.mod_value[t][2] = clampf(k->follow, 0.f, 1.f);
    /* ENV */
    float ea = 0.001f + 2.f * (float)b[P_M3A] * (float)b[P_M3A] * (1.f / 16129.f);
    float ed = 0.005f + 3.f * (float)b[P_M3B] * (float)b[P_M3B] * (1.f / 16129.f);
    float es = (float)b[P_M3C] * (1.f / 127.f);
    float er = 0.005f + 4.f * (float)b[P_M3D] * (float)b[P_M3D] * (1.f / 16129.f);
    switch (k->env_stage) {
    case 1: k->env += dt / ea; if (k->env >= 1.f) { k->env = 1.f; k->env_stage = 2; } break;
    case 2: k->env += (es - k->env) * (dt / ed); if (!k->gates && es > 0.f) k->env_stage = 3;
            if (absf(k->env - es) < 0.001f && es == 0.f) k->env_stage = 0; break;
    case 3: k->env -= k->env * (dt / er) + 0.0001f; if (k->env <= 0.f) { k->env = 0.f; k->env_stage = 0; } break;
    default: break;
    }
    vsc_meter.mod_value[t][3] = k->env;
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
    int64_t np = (int64_t)*ipos + whole;
    if (np >= (int64_t)b || np < (int64_t)a) {
        if (loopm == 0) return 0;
        uint32_t span = b - a;
        if (!span) return 0;
        if (loopm == 1) {
            int64_t off = (np - (int64_t)a) % (int64_t)span;
            if (off < 0) off += span;
            np = (int64_t)a + off;
        } else {
            *dir = (int8_t)-*dir;
            np = np >= (int64_t)b ? (int64_t)b - 1 : (int64_t)a;
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
static VSC_HOT void grains(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    float mix = k->e[P_GMIX];
    uint8_t *ring = grain_ring[t];
    int freeze = vsc.t[t][P_GFREEZE];
    float fb = k->e[P_GFDBK] * 0.9f;
    if (mix <= 0.001f && fb <= 0.f) {
        /* Keep the ring filled so the grains have material when MIX rises. */
        if (!freeze)
            for (unsigned i = 0; i < n; ++i) { ring[k->gw] = ulaw_enc(x[i]); if (++k->gw >= VSC_GRAIN_FRAMES) k->gw = 0; }
        for (unsigned g = 0; g < VSC_GRAINS; ++g) k->g[g].active = 0;
        return;
    }
    float size = k->e[P_GSIZE];
    uint32_t glen = (uint32_t)((0.008f + size * size * 0.35f) * (float)VSC_SR);
    if (glen > VSC_GRAIN_FRAMES / 3) glen = VSC_GRAIN_FRAMES / 3;
    float dens = k->e[P_GDENS];
    float per_sec = 0.5f + dens * dens * 80.f;
    float spawn = per_sec / (float)VSC_SR;
    float shape = k->e[P_GSHAPE];
    for (unsigned i = 0; i < n; ++i) {
        k->gphase += spawn;
        if (k->gphase >= 1.f) {
            k->gphase -= 1.f;
            for (unsigned g = 0; g < VSC_GRAINS; ++g) {
                struct grain *gr = &k->g[g];
                if (gr->active) continue;
                float semis = k->e[P_GPITCH] * 48.f - 24.f + rndb() * k->e[P_GDETUNE] * 12.f;
                int q = quant_steps[vsc.t[t][P_GQUANT] & 3];
                if (q) { float s = semis / (float)q; semis = (float)floori(s + 0.5f) * (float)q; }
                float ratio = exp2a(semis * (1.f / 12.f));
                int rev = rndu() < k->e[P_GREV];
                float need = (float)glen * (ratio > 1.f ? ratio : 1.f) + 64.f;
                float room = (float)VSC_GRAIN_FRAMES - need - 64.f;
                float back = need + k->e[P_GTIME] * room * 0.9f + rndu() * k->e[P_GSPRAY] * room * 0.5f;
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
        x[i] = x[i] + (acc - x[i]) * mix;
    }
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
static VSC_HOT void filter(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    unsigned type = vsc.t[t][P_FTYPE];
    if (type == FT_OFF) return;
    float *x = k->out + off;
    float mix = k->e[P_FMIX];
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
            x[i] = v0 + (y - v0) * mix;
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
        x[i] = v0 + (acc - v0) * mix;
    }
}

/* --------------------------------------------------------------- color */
static VSC_HOT void color(unsigned t, unsigned off, unsigned n)
{
    struct track *k = &tr[t];
    float *x = k->out + off;
    float drive = k->e[P_DRIVE], comp = k->e[P_COMP], bits = k->e[P_BITS];
    float rate = k->e[P_RATE], noise = k->e[P_NOISE], tone = k->e[P_TONE] * 2.f - 1.f;
    float mix = k->e[P_CMIX];
    if (drive < 0.002f && comp < 0.002f && bits < 0.002f && rate < 0.002f && noise < 0.002f &&
        tone > -0.02f && tone < 0.02f)
        return;
    float pre = 1.f + drive * drive * 24.f, post = 1.f / (1.f + drive * 2.5f);
    float levels = exp2a(1.f + (1.f - bits) * 14.f);
    float hold_inc = 1.f / (1.f + rate * rate * 48.f);
    int dust = vsc.t[t][P_NTYPE];
    float n2 = noise * noise;
    for (unsigned i = 0; i < n; ++i) {
        float v0 = x[i], v = v0;
        if (drive > 0.002f) v = tanha(v * pre) * (1.f + drive) * post * 1.4f;
        if (comp > 0.002f) {
            float a = absf(v);
            k->comp_env += (a - k->comp_env) * (a > k->comp_env ? 0.02f : 0.0005f);
            v *= (1.f + comp * 2.5f) / (1.f + comp * 10.f * k->comp_env);
        }
        if (rate > 0.002f) {
            k->hold_phase += hold_inc;
            if (k->hold_phase >= 1.f) { k->hold_phase -= 1.f; k->hold = v; }
            v = k->hold;
        }
        if (bits > 0.002f) v = (float)floori(v * levels + 0.5f) / levels;
        if (noise > 0.002f) {
            if (!dust) v += rndb() * n2 * 0.25f * (0.15f + 0.85f * clampf(k->comp_env * 4.f + k->follow, 0.f, 1.f));
            else {
                if (rndu() < n2 * 0.004f) k->dust = rndb() * (0.3f + 0.7f * rndu());
                v += k->dust; k->dust *= 0.6f;
            }
        }
        k->tone_lp += (v - k->tone_lp) * 0.11f;
        float hi = v - k->tone_lp;
        v = k->tone_lp * (1.f - tone * 0.9f) + hi * (1.f + tone * 1.5f);
        x[i] = v0 + (v - v0) * mix;
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
    float frames = beats * 60.f / (float)vsc.tempo * (float)VSC_SR;
    float cap = (float)(VSC_DELAY_FRAMES - 4) * (long_mode ? 2.f : 1.f);
    while (frames > cap) frames *= 0.5f;
    float stored = long_mode ? frames * 0.5f : frames;
    float fb = (float)g[G_DFEED] * (1.f / 127.f) * 0.95f;
    float dtone = ((float)g[G_DTONE] - 64.f) * (1.f / 64.f);
    float dlev = (float)g[G_DLEVEL] * (1.f / 127.f);
    float rfb = 0.70f + 0.28f * (float)g[G_RSIZE] * (1.f / 127.f);
    float damp = 0.05f + 0.6f * (float)g[G_RDAMP] * (1.f / 127.f);
    float rlev = (float)g[G_RLEVEL] * (1.f / 127.f);
    for (unsigned i = 0; i < n; ++i) {
        float in = (space_l[i] + space_r[i]) * 0.5f;
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
        float step_frames = 15.f / (float)vsc.tempo * (float)VSC_SR;
        tr[t].gate_left = (int32_t)(step_frames * 0.5f);
    }
}
static VSC_HOT void sequencer(unsigned n)
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
    if (vsc.g[G_CLOCK]) { /* MIDI clock: steps fire on every sixth pulse */
        while (seq_ext_pending) { --seq_ext_pending; fire_step(); ++seq_step; }
        return;
    }
    float step_frames = 15.f / (float)vsc.tempo * (float)VSC_SR;
    float swing = (float)vsc.g[G_SWING] * (1.f / 127.f) * 0.5f * step_frames;
    float this_len = (seq_step & 1) ? step_frames - swing : step_frames + swing;
    seq_phase += (float)n;
    if (seq_phase >= this_len) {
        seq_phase -= this_len;
        ++seq_step;
        fire_step();
    }
}
static VSC_HOT void seq_start(int from_zero)
{
    if (from_zero) { seq_step = 0; seq_phase = 0.f; seq_clock_count = 0; }
    seq_running = 1;
    vsc_meter.playing = 1;
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
static VSC_HOT void handle(uint32_t e)
{
    unsigned type = e & 15u, t = (e >> 4) & 15u, a = (e >> 8) & 255u, b = (e >> 16) & 255u;
    if (t >= VSC_TRACKS) t = 0;
    switch (type) {
    case EV_NOTE_ON: if (b) note_on(t, a, b); else note_off(t, a); break;
    case EV_NOTE_OFF: note_off(t, a); break;
    case EV_PLAY: if (seq_running) seq_stop(); else seq_start(1); break;
    case EV_STOP: seq_stop(); break;
    case EV_START: if (vsc.g[G_CLOCK]) seq_start(1); break;
    case EV_CONT: if (vsc.g[G_CLOCK]) seq_start(0); break;
    case EV_CLOCK:
        ext_last = 0; vsc_meter.ext_clock = 1;
        if (vsc.g[G_CLOCK] && seq_running && ++seq_clock_count >= 6) { seq_clock_count = 0; if (seq_ext_pending < 4) ++seq_ext_pending; }
        break;
    case EV_REC: loop_rec(t); break;
    case EV_BALL: ball_add(t, a, b); ball_seeded[t] = 1; break;
    case EV_BALL_CLEAR: for (unsigned i = 0; i < VSC_BALLS; ++i) balls[t][i].on = 0; ball_next[t] = 0; ball_seeded[t] = 0; pending_n[t] = 0; break;
    case EV_LOOP_CLEAR: tr[t].loop_state = 0; tr[t].loop_len = 0; tr[t].running = 0; break;
    case EV_PANIC:
        for (unsigned i = 0; i < VSC_TRACKS; ++i) {
            for (unsigned v = 0; v < VSC_KEYS_VOICES; ++v) tr[i].v[v].on = 0;
            tr[i].gates = 0; tr[i].env_stage = 3;
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
    for (unsigned p = 0; p < VSC_NP; ++p) is_enum[p] = (vsc_tparam_info(0, p)->flags & VSC_PF_ENUM) != 0;
    vsc_memset(tr, 0, sizeof tr);
    vsc_memset(vsc_lock, 255, sizeof vsc_lock);
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
    drain(&q_ctrl);
    drain(&q_midi);
    frame_counter += VSC_BLOCK;
    if (ext_last < 0x7fffffffu) ext_last += VSC_BLOCK;
    if (ext_last > VSC_SR) vsc_meter.ext_clock = 0;
    vsc_meter.bt_in = in->bt != 0;
    vsc_meter.usb_in = in->usb != 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) input_for(t, in);
    for (unsigned i = 0; i < VSC_BLOCK; ++i) { space_l[i] = space_r[i] = 0.f; out[2 * i] = out[2 * i + 1] = 0.f; }

    for (unsigned off = 0; off < VSC_BLOCK; off += VSC_SUB) {
        sequencer(VSC_SUB);
        for (unsigned t = 0; t < VSC_TRACKS; ++t) {
            struct track *k = &tr[t];
            compute_params(t);
            material(t, off, VSC_SUB);
            grains(t, off, VSC_SUB);
            filter(t, off, VSC_SUB);
            color(t, off, VSC_SUB);
            run_mods(t, VSC_SUB);
            float lev = k->e[P_LEVEL]; lev = lev * lev * 1.6f;
            if (vsc.t[t][P_MUTE]) lev = 0.f;
            float pan = k->e[P_PAN];
            float pl = sin01(0.25f + pan * 0.25f), pr = sin01(pan * 0.25f);
            float send = k->e[P_SEND] * k->e[P_SEND];
            for (unsigned i = 0; i < VSC_SUB; ++i) {
                float v = k->out[off + i] * lev;
                out[2 * (off + i)] += v * pl;
                out[2 * (off + i) + 1] += v * pr;
                space_l[off + i] += v * pl * send;
                space_r[off + i] += v * pr * send;
            }
        }
    }
    {
        float l[VSC_BLOCK], r[VSC_BLOCK];
        for (unsigned i = 0; i < VSC_BLOCK; ++i) { l[i] = 0.f; r[i] = 0.f; }
        space(l, r, VSC_BLOCK);
        for (unsigned i = 0; i < VSC_BLOCK; ++i) { out[2 * i] += l[i]; out[2 * i + 1] += r[i]; }
    }
    float target = (float)vsc.g[G_MAIN] * (1.f / 127.f);
    target *= target;
    float mp = 0.f;
    for (unsigned i = 0; i < VSC_BLOCK; ++i) {
        main_gain_s += (target - main_gain_s) * 0.01f;
        float l = tanha(out[2 * i] * main_gain_s * 1.2f), r = tanha(out[2 * i + 1] * main_gain_s * 1.2f);
        out[2 * i] = l; out[2 * i + 1] = r;
        main_prev[i] = (l + r) * 0.5f;
        vsc_scope[0][i] = l; vsc_scope[1][i] = r;
        float a = absf(l) > absf(r) ? absf(l) : absf(r);
        if (a > mp) mp = a;
    }
    vsc_meter.main_peak = mp > vsc_meter.main_peak ? mp : vsc_meter.main_peak * 0.97f;
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
