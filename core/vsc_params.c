/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: parameter table, labels, defaults, scenes and routing. */
#include "vsc.h"

static const char *const n_src[] = {"CAPTUR", "REEL", "THRU"};
static const char *const n_input[] = {"BT", "USB", "BT+USB", "A", "B", "C", "D", "MAIN"};
static const char *const n_rate[] = {"HI", "MID", "LO"};
static const char *const n_off_on[] = {"OFF", "ON"};
static const char *const n_bounce[] = {"OFF", "IN", "PING"};
static const char *const n_keys[] = {"OFF", "PITCH", "PLAY", "SLICE"};
static const char *const n_trig[] = {"FREE", "HIT", "NOTE", "CLOCK"};
static const char *const n_engine[] = {"HANDS", "SHARDS", "SPECTR"};
static const char *const n_wmode[] = {"MIX", "SEND"};
static const char *const n_cmode[] = {"BITS", "REDUX", "BOTH"};
static const char *const n_xover[] = {"LOW", "MID", "HIGH"};
static const char *const n_ntype[] = {"WHITE", "PINK", "DUST"};
static const char *const n_mtype[] = {"WAVE", "RANDOM", "FOLLOW", "ADSR"};
static const char *const n_wshape[] = {"SINE", "TRI", "SQUARE", "SAW", "RAMP", "S+H"};
static const char *const n_sync[] = {"FREE", "STRGHT", "TRIPLT", "DOTTED"};
static const char *const n_fin[] = {"TRACK", "SOURCE", "A", "B", "C", "D", "MAIN", "INPUT",
                                    "LOW", "MID", "HIGH", "HITS", "PITCH", "BRIGHT"};
static const char *const n_afocus[] = {"FULL", "LOW", "MID", "HIGH"};
static const char *const n_ahits[] = {"OFF", "BALLS", "STEPS"};
static const char *const n_btrack[] = {"OFF", "A", "B", "C", "D"};
static const char *const n_bmode[] = {"OFF", "FILTER", "VOCODE", "PING", "HYBRID"};
static const char *const n_bvmod[] = {"SELF", "INPUT", "PARTNR", "OSC", "NOISE", "OUTPUT"};
static const char *const n_btrig[] = {"OFF", "HIGH", "FULL", "LOW", "HITS", "NOTES"};
static const char *const n_bquant[] = {"OFF", "1/32", "1/16", "1/8", "1/4"};
static const char *const n_dtime[] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4"};
static const char *const n_clock[] = {"AUTO", "INT", "MIDI"};
static const char *const n_seqm[] = {"STEPS", "CAROM"};
static const char *const n_rquant[] = {"FREE", "1/16", "1/8"};
static const char *const n_rwalls[] = {"OCTAVE", "SAME", "5TH"};
/* the enum labels, by number: a parameter keeps one byte for its list, not a pointer */
enum { NL_NONE, NL_n_src, NL_n_input, NL_n_rate, NL_n_off_on, NL_n_bounce, NL_n_keys, NL_n_trig, NL_n_engine, NL_n_wmode, NL_n_cmode, NL_n_xover, NL_n_ntype, NL_n_mtype, NL_n_wshape, NL_n_sync, NL_n_fin, NL_n_afocus, NL_n_ahits, NL_n_btrack, NL_n_bmode, NL_n_bvmod, NL_n_btrig, NL_n_bquant, NL_n_dtime, NL_n_clock, NL_n_seqm, NL_n_rquant, NL_n_rwalls };
static const char *const *const name_lists[] = {0, n_src, n_input, n_rate, n_off_on, n_bounce, n_keys, n_trig, n_engine, n_wmode, n_cmode, n_xover, n_ntype, n_mtype, n_wshape, n_sync, n_fin, n_afocus, n_ahits, n_btrack, n_bmode, n_bvmod, n_btrig, n_bquant, n_dtime, n_clock, n_seqm, n_rquant, n_rwalls};
#define NAMES(i) name_lists[i]

#define C(n, d) {n, 127, d, 0, 0}
#define B(n, d) {n, 127, d, VSC_PF_BIPOLAR, 0}
#define E(n, list, d) {n, sizeof list / sizeof list[0] - 1, d, VSC_PF_ENUM, NL_##list}
#define U(n, d) {n, 127, d, VSC_PF_UNIT, 0}          /* shown as 0.00 .. 1.00 */
#define X {"", 0, 0, VSC_PF_ENUM, 0}
#define MODSLOT E("TYPE", n_mtype, 0), X, X, X, X, X, X, X, X, X

static const struct vsc_pinfo tinfo[VSC_NP] = {
    /* SOURCE */
    E("SOURCE", n_src, 0), E("INPUT", n_input, IN_BOTH), E("RATE", n_rate, RATE_MID), U("SOS", 100),
    E("BOUNCE", n_bounce, 0), {"REEL", 16, 0, VSC_PF_ENUM, 0}, {"SPLICE", 16, 0, VSC_PF_ENUM, 0}, C("GAIN", 100),
    E("KEYS", n_keys, KEYS_PITCH),
    /* HANDS */
    C("STRTCH", 50), {"PITCH", 48, 24, VSC_PF_BIPOLAR, 0}, U("POS", 0), U("SIZE", 127),
    E("REVRS", n_off_on, 0), U("GRAIN", 50), U("JITTER", 10), E("FREEZE", n_off_on, 0),
    /* SHARDS */
    U("SHAPE", 40), U("DENS", 70), U("SPRAY", 40), E("TRIG", n_trig, 0),
    U("SIZE", 40), U("SPREAD", 0), U("REVRS", 0), U("STEREO", 60),
    /* SPECTRA */
    U("BLUR", 0), {"SHIFT", 48, 24, VSC_PF_BIPOLAR, 0}, U("CROSS", 0), U("GATE", 0), B("TILT", 64), U("HOLD", 0), U("PHASE", 0),
    E("MODE", n_engine, 0),
    /* COLOR */
    C("DRIVE", 0), C("COMP", 0), C("CRUSH", 0), E("C.MODE", n_cmode, 0), B("TILT", 64), E("XOVER", n_xover, 1),
    C("NOISE", 0), C("N.DEC", 40), C("N.TONE", 64), E("N.TYPE", n_ntype, 1), C("WET", 127), E("W.MODE", n_wmode, 0),
    /* MIX */
    C("LEVEL", 100), B("PAN", 64), C("SEND", 0), E("MUTE", n_off_on, 0), B("FILTER", 64),
    /* MODS: the per-type parameters come from mod_tables */
    MODSLOT, MODSLOT, MODSLOT, MODSLOT,
    /* SEQUENCER */
    E("SEQ", n_seqm, 0), C("SPEED", 50), B("GRAVTY", 64), C("LOSS", 0), C("SPREAD", 64), E("QUANT", n_rquant, 1),
    {"BALLS", VSC_BALLS - 1, VSC_BALLS - 1, VSC_PF_ENUM, 0}, E("WALLS", n_rwalls, 0), C("JITTER", 0), C("GATE", 40),
};
/* Modulators: parameters 1..9 for each type (0 is TYPE). */
static const struct vsc_pinfo mod_tables[4][9] = {
    /* WAVE */
    {C("RATE", 50), C("AMOUNT", 127), C("PHASE", 0), B("OFFSET", 64), B("SKEW", 64), C("FOLD", 0), B("CURVE", 64),
     E("SHAPE", n_wshape, 0), E("SYNC", n_sync, 0)},
    /* RANDOM */
    {C("RATE", 70), C("AMOUNT", 127), C("PHASE", 0), B("OFFSET", 64), {"LENGTH", 15, 7, VSC_PF_ENUM, 0}, C("VARIAT", 30),
     C("SMOOTH", 40), X, E("SYNC", n_sync, 0)},
    /* FOLLOW */
    {C("DELAY", 0), C("AMOUNT", 127), C("RISE", 10), C("FALL", 50), E("INPUT", n_fin, 0), B("FILTER", 64), C("HOLD", 0),
     C("GAIN", 64), X},
    /* ADSR */
    {C("ATTACK", 5), C("DECAY", 50), C("SUSTN", 0), C("RELEAS", 30), B("A.CURV", 64), B("D.CURV", 64), B("R.CURV", 64),
     C("AMOUNT", 127), X},
};
static const struct vsc_pinfo ginfo[VSC_NG] = {
    C("SWING", 0), E("TIME", n_dtime, 5), C("FDBK", 50), B("TONE", 64), E("LONG", n_off_on, 0), C("DELAY", 60),
    C("SIZE", 70), C("DAMP", 50), C("REVERB", 60), C("MAIN", 110), E("CLOCK", n_clock, 0), E("MONITR", n_off_on, 0),
    C("MORPH", 0), {"TARGET", VSC_SCENES - 1, 1, VSC_PF_ENUM, 0}, {"MACRO", VSC_MACROS - 1, 0, VSC_PF_ENUM, 0},
    C("COMP", 0),
    C("M1", 0), C("M2", 0), C("M3", 0), C("M4", 0), C("M5", 0), C("M6", 0), C("M7", 0), C("M8", 0),
    E("SOURCE", n_input, IN_BOTH), C("SENS", 80), E("FOCUS", n_afocus, 0), E("HITS", n_ahits, 0),
    /* RESON, as LIQUID MEMORY: bands 2 and 7 (400 Hz, 3.2 kHz) up, the rest far down;
     * band 2 rings about 180 ms, band 7 about 70 ms */
    E("TRACK", n_btrack, 0), E("MODE", n_bmode, BM_HYBRID), C("RESO", 66), C("MIX", 83), C("WIDTH", 76),
    C("GAIN 1", 38), C("GAIN 2", 110), C("GAIN 3", 38), C("GAIN 4", 30), C("GAIN 5", 30), C("GAIN 6", 34), C("GAIN 7", 103), C("GAIN 8", 30),
    B("FREQ 1", 64), B("FREQ 2", 64), B("FREQ 3", 64), B("FREQ 4", 64), B("FREQ 5", 64), B("FREQ 6", 64), B("FREQ 7", 64), B("FREQ 8", 64),
    B("DECAY1", 78), B("DECAY2", 64), B("DECAY3", 50), B("DECAY4", 41), B("DECAY5", 35), B("DECAY6", 26), B("DECAY7", 20), B("DECAY8", 11),
    B("SHIFT", 64), C("LIQUID", 44), C("FDBK", 10), C("SPECTR", 64),
    E("MODSRC", n_bvmod, BV_SELF), C("OSC", 40), C("ATTACK", 18), C("RELEAS", 54), C("DEPTH", 72),
    E("TRIG", n_btrig, BT_HIGH), C("SENS", 90), E("QUANT", n_bquant, 0), C("HARD", 89),
    C("DENS", 0), C("COHES", 80), C("DROPS", 0), C("MIGR", 20),
};
static const uint8_t mod_default_type[4] = {MT_WAVE, MT_RANDOM, MT_FOLLOW, MT_ADSR};

const struct vsc_pinfo *vsc_mod_info(unsigned type, unsigned k)
{
    if (k == 0) return &tinfo[P_MOD0];
    return &mod_tables[type & 3][(k - 1) % 9];
}
void vsc_mod_defaults(unsigned t, unsigned m)
{
    if (t >= VSC_TRACKS || m >= VSC_MODS) return;
    unsigned type = vsc.t[t][P_MOD(m, 0)] & 3;
    for (unsigned k = 1; k < VSC_MOD_PARAMS; ++k) vsc.t[t][P_MOD(m, k)] = mod_tables[type][k - 1].def;
}
const struct vsc_pinfo *vsc_tparam_info(unsigned track, unsigned p)
{
    if (p >= VSC_NP) return &tinfo[P_MOD0 + 1];   /* an empty entry */
    if (p > P_MOD0 && p < P_MOD0 + 4 * VSC_MOD_PARAMS && track < VSC_TRACKS) {
        unsigned rel = p - P_MOD0, k = rel % VSC_MOD_PARAMS;
        if (k) return &mod_tables[vsc.t[track][P_MOD0 + rel - k] & 3][k - 1];
    }
    return &tinfo[p];
}
const struct vsc_pinfo *vsc_gparam_info(unsigned p) { return p < VSC_NG ? &ginfo[p] : &ginfo[0]; }

static void put_int(char *o, int v)
{
    char tmp[8];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)-v : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 6);
    int i = 0;
    if (neg) o[i++] = '-';
    while (n) o[i++] = tmp[--n];
    o[i] = 0;
}
static void put_str(char *o, const char *s)
{
    int i = 0;
    while (s[i] && i < 7) { o[i] = s[i]; ++i; }
    o[i] = 0;
}
/* a number with a sign if asked, and a unit after it: "+7ST", "-12DB" */
static void put_unit(char *o, int v, int sign, const char *unit)
{
    int i = 0;
    if (sign && v >= 0) o[i++] = '+';
    put_int(o + i, v);
    i = (int)vsc_strlen(o);
    while (*unit && i < 7) o[i++] = *unit++;
    o[i] = 0;
}
/* RESON's bands as the knobs have them (the engine adds its liquid motion) */
static const float rb_base[VSC_RBANDS] = {200.f, 400.f, 800.f, 1200.f, 1600.f, 2400.f, 3200.f, 4800.f};
static float pexp2(float x)   /* 2^x, near enough for a label */
{
    float r = 1.f; while (x >= 1.f) { r *= 2.f; x -= 1.f; } while (x < 0.f) { r *= 0.5f; x += 1.f; }
    return r * (1.f + x * (0.6951786f + x * (0.2261598f + x * 0.0782371f)));
}
float vsc_reson_hz(unsigned b)
{
    b &= 7u;
    return rb_base[b] * pexp2(((float)vsc.g[G_BSHIFT] + (float)vsc.g[G_BF0 + b] - 128.f) * (1.f / 64.f));
}
float vsc_reson_sec(unsigned b)
{
    b &= 7u;
    return 0.01f * pexp2((float)vsc.g[G_BRESO] * (8.f / 127.f) + ((float)vsc.g[G_BD0 + b] - 64.f) * (1.f / 32.f));
}
void vsc_param_label(const struct vsc_pinfo *info, unsigned p, unsigned v, char out[8], int global)
{
    out[0] = 0;
    if (!info->name[0]) return;
    if (info->names) { put_str(out, NAMES(info->names)[v > info->max ? info->max : v]); return; }
    if (info->flags & VSC_PF_UNIT) {   /* 0.00 .. 1.00 */
        int c = ((int)v * 100 + 63) / 127, i = 0;
        out[i++] = (char)('0' + c / 100); out[i++] = '.';
        out[i++] = (char)('0' + (c / 10) % 10); out[i++] = (char)('0' + c % 10); out[i] = 0;
        return;
    }
    if (!global) {
        if (p == P_REEL) { if (!v) { put_str(out, "KEPT"); return; } out[0] = 'S'; put_int(out + 1, (int)v); return; }
        if (p == P_SPLICE) { if (!v) { put_str(out, "ALL"); return; } put_int(out, (int)v); return; }
        if (p == P_PITCH || p == P_SHIFT) {   /* +7ST */
            put_unit(out, (int)v - 24, 1, "ST"); return;
        }
        if (p == P_STRETCH) {   /* 0.25X .. 8.00X, the top: frozen */
            if (v >= 127) { put_str(out, "FREEZE"); return; }
            float f = 1.f;
            int e = (int)v - 50;   /* 2^(e/25) */
            while (e >= 25) { f *= 2.f; e -= 25; }
            while (e < 0) { f *= 0.5f; e += 25; }
            f *= 1.f + (float)e * (0.6931472f / 25.f) * (1.f + (float)e * (0.6931472f / 50.f));
            int c = (int)(f * 100.f + 0.5f), i = 0;
            if (c >= 1000) { put_int(out, c / 100); i = (int)vsc_strlen(out); out[i++] = '.'; out[i++] = (char)('0' + (c / 10) % 10); }
            else { out[i++] = (char)('0' + c / 100); out[i++] = '.'; out[i++] = (char)('0' + (c / 10) % 10); out[i++] = (char)('0' + c % 10); }
            out[i++] = '\\'; out[i] = 0; return;   /* the screen draws '\\' as a times sign */
        }
        if (p == P_RCOUNT) { put_int(out, (int)v + 1); return; }
        if (p > P_MOD0 && p < P_MOD0 + 4 * VSC_MOD_PARAMS && (p - P_MOD0) % VSC_MOD_PARAMS == 5 && !info->names && info->max == 15) { put_int(out, (int)v + 1); return; }
    } else if (p == G_TARGET) { out[0] = 'S'; out[1] = 'C'; put_int(out + 2, (int)v + 1); return; }
    else if (p == G_MSEL) { out[0] = 'M'; put_int(out + 1, (int)v + 1); return; }
    else if (p >= G_BG0 && p <= G_BG7) {   /* -60 .. +6 dB, 0: off */
        if (!v) { put_str(out, "OFF"); return; }
        put_unit(out, -60 + ((int)v * 66 + 63) / 127, 0, "DB"); return;
    } else if (p == G_BSHIFT) {           /* +-12 semitones */
        put_unit(out, ((int)v - 64) * 12 / 64, 1, "ST"); return;
    } else if (p >= G_BF0 && p <= G_BF7) { /* the band's frequency, with the bank's shift */
        int hz = (int)(vsc_reson_hz(p - G_BF0) + 0.5f);
        put_unit(out, hz, 0, hz < 10000 ? "HZ" : ""); return;
    } else if (p == G_BRESO || (p >= G_BD0 && p <= G_BD7)) {   /* the decay: of band 2, or of this band */
        float ms = vsc_reson_sec(p == G_BRESO ? 1u : p - G_BD0) * 1000.f;
        if (ms < 1000.f) put_unit(out, (int)(ms + 0.5f), 0, "MS");
        else { int c = (int)(ms / 10.f + 0.5f); if (c > 999) c = 999; out[0] = (char)('0' + c / 100); out[1] = '.'; out[2] = (char)('0' + (c / 10) % 10); out[3] = 'S'; out[4] = 0; }
        return;
    }
    if (info->flags & VSC_PF_BIPOLAR) { put_int(out, (int)v - (int)(info->max + 1) / 2); return; }
    put_int(out, (int)v);
}

VSC_HOT void vsc_memset(void *d, int v, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)v; }
void vsc_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    while (n--) *a++ = *b++;
}
size_t vsc_strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }

uint32_t vsc_crc32(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    while (bytes--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void vsc_track_defaults(struct vsc_project *p, unsigned t)
{
    for (unsigned i = 0; i < VSC_NP; ++i) p->t[t][i] = tinfo[i].def;
    /* A useful start: A captures the input, B captures what A plays (REC on B bounces
     * it), C is the input live through the putty (muted until wanted), D plays the reel. */
    static const uint8_t srcs[VSC_TRACKS] = {SRC_CAPTURE, SRC_CAPTURE, SRC_THRU, SRC_REEL};
    p->t[t][P_SRC] = srcs[t % VSC_TRACKS];
    if (t == 1) p->t[t][P_BOUNCE] = BOUNCE_IN;
    if (t == 2) p->t[t][P_MUTE] = 1;
    p->length[t] = 16;
    for (unsigned s = 0; s < VSC_STEPS; ++s) {
        struct vsc_step *st = &p->step[t][s];
        vsc_memset(st, 0, sizeof *st);
        st->note = 60; st->vel = 100; st->lp[0] = st->lp[1] = 255;
        st->len = 4; st->prob = 100;   /* half a step, always */
    }
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) { p->route[t][r].param = 255; p->route[t][r].depth = 0; p->route[t][r].flags = 0; }
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        unsigned type = mod_default_type[m];
        p->t[t][P_MOD(m, 0)] = (uint8_t)type;
        for (unsigned k = 1; k < VSC_MOD_PARAMS; ++k) p->t[t][P_MOD(m, k)] = mod_tables[type][k - 1].def;
    }
}
void vsc_project_defaults(struct vsc_project *p)
{
    vsc_memset(p, 0, sizeof *p);
    p->magic = VSC_MAGIC; p->version = VSC_FORMAT; p->bytes = sizeof *p;
    vsc_memcpy(p->name, "NEW PROJECT", 12);
    p->tempo = 110;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) vsc_track_defaults(p, t);
    for (unsigned g = 0; g < VSC_NG; ++g) p->g[g] = ginfo[g].def;
    for (unsigned m = 0; m < VSC_MACROS; ++m)
        for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r) { p->macro[m][r].param = 255; p->macro[m][r].depth = 0; }
}
int vsc_project_valid(const struct vsc_project *p)
{
    if (p->magic != VSC_MAGIC || p->version != VSC_FORMAT || p->bytes != sizeof *p) return 0;
    uint32_t want = p->crc;
    /* CRC covers everything after the crc field. */
    const uint8_t *body = (const uint8_t *)&p->name;
    return vsc_crc32(body, sizeof *p - offsetof(struct vsc_project, name)) == want;
}

int vsc_project_upgrade6(struct vsc_project *p)
{
    if (p->magic != VSC_MAGIC || p->version != 6u || p->bytes != VSC_PROJECT6_BYTES) return -1;
    /* everything after the old globals moves up to make room for RESON's */
    uint8_t *b = (uint8_t *)p;
    size_t from = offsetof(struct vsc_project, g) + VSC_NG_6, gap = VSC_NG - VSC_NG_6, n = VSC_PROJECT6_BYTES - from;
    while (n--) b[from + gap + n] = b[from + n];
    for (unsigned g = VSC_NG_6; g < VSC_NG; ++g) p->g[g] = ginfo[g].def;
    p->version = VSC_FORMAT; p->bytes = sizeof *p;
    p->crc = vsc_crc32(&p->name, sizeof *p - offsetof(struct vsc_project, name));
    return 0;
}
void vsc_scene_store(unsigned s)
{
    if (s >= VSC_SCENES) return;
    struct vsc_scene *sc = &vsc.scene[s];
    vsc_memcpy(sc->t, vsc.t, sizeof sc->t);
    for (unsigned m = 0; m < VSC_MACROS; ++m) sc->macro[m] = vsc.g[G_M0 + m];
    sc->used = 1;
}
VSC_HOT void vsc_scene_recall(unsigned s)
{
    if (s >= VSC_SCENES || !vsc.scene[s].used) return;
    const struct vsc_scene *sc = &vsc.scene[s];
    /* The source and its rate hold what's captured: keep the running ones. */
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned p = 0; p < VSC_NP; ++p) {
            if (p == P_SRC || p == P_RATE || p == P_INPUT) continue;
            vsc.t[t][p] = sc->t[t][p];
        }
    for (unsigned m = 0; m < VSC_MACROS; ++m) vsc.g[G_M0 + m] = sc->macro[m];
}

void vsc_route_set(unsigned t, unsigned src, unsigned param, int depth)
{
    if (t >= VSC_TRACKS || (src & ~(VSC_SRC_OTHER | 15u))) return;
    if (param >= VSC_NP && (param < VSC_GDEST || param >= VSC_GDEST + VSC_NG)) return;
    if (depth > 127) depth = 127;
    if (depth < -127) depth = -127;
    struct vsc_route *free_slot = 0;
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
        struct vsc_route *ro = &vsc.route[t][r];
        if (ro->param == param && ro->src == src) {
            if (!depth) { ro->param = 255; ro->depth = 0; }
            else ro->depth = (int8_t)depth;
            return;
        }
        if (ro->param == 255 && !free_slot) free_slot = ro;
    }
    if (depth && free_slot) { free_slot->src = (uint8_t)src; free_slot->param = (uint8_t)param; free_slot->depth = (int8_t)depth; }
}
int vsc_route_get(unsigned t, unsigned src, unsigned param)
{
    if (t >= VSC_TRACKS) return 0;
    for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r)
        if (vsc.route[t][r].param == param && vsc.route[t][r].src == src) return vsc.route[t][r].depth;
    return 0;
}
void vsc_macro_set(unsigned m, unsigned t, unsigned param, int depth)
{
    if (m >= VSC_MACROS || t >= VSC_TRACKS || param >= VSC_NP) return;
    if (depth > 127) depth = 127;
    if (depth < -127) depth = -127;
    struct vsc_mroute *free_slot = 0;
    for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r) {
        struct vsc_mroute *ro = &vsc.macro[m][r];
        if (ro->param == param && ro->track == t) {
            if (!depth) { ro->param = 255; ro->depth = 0; }
            else ro->depth = (int8_t)depth;
            return;
        }
        if (ro->param == 255 && !free_slot) free_slot = ro;
    }
    if (depth && free_slot) { free_slot->track = (uint8_t)t; free_slot->param = (uint8_t)param; free_slot->depth = (int8_t)depth; }
}
int vsc_macro_get(unsigned m, unsigned t, unsigned param)
{
    if (m >= VSC_MACROS) return 0;
    for (unsigned r = 0; r < VSC_MACRO_ROUTES; ++r)
        if (vsc.macro[m][r].param == param && vsc.macro[m][r].track == t) return vsc.macro[m][r].depth;
    return 0;
}

/* ---- the mod matrix's destinations: every continuous parameter, track and global */
static const char sect_of[VSC_NP] = {
    'I','I','I','I','I','I','I','I','I',
    'H','H','H','H','H','H','H','H',
    'R','R','R','R','R','R','R','R',
    'F','F','F','F','F','F','F',
    'H',
    'C','C','C','C','C','C','C','C','C','C','C','C',
    'X','X','X','X','X',
    '1','1','1','1','1','1','1','1','1','1','2','2','2','2','2','2','2','2','2','2',
    '3','3','3','3','3','3','3','3','3','3','4','4','4','4','4','4','4','4','4','4',
    'Q','Q','Q','Q','Q','Q','Q','Q','Q','Q'};
typedef char sect_size_check[sizeof sect_of == VSC_NP ? 1 : -1];
static int dest_ok(unsigned code)
{
    if (code < VSC_NP) {
        /* a modulator's own parameters are left out: a mod moving its own slot goes nowhere */
        if (code >= P_MOD0 && code < P_MOD0 + 4 * VSC_MOD_PARAMS) {
            unsigned k = (code - P_MOD0) % VSC_MOD_PARAMS;
            return k == 1 || k == 2;      /* RATE/DELAY and AMOUNT/DECAY of each slot can be modulated */
        }
        return tinfo[code].name[0] && !(tinfo[code].flags & VSC_PF_ENUM);
    }
    if (code >= VSC_GDEST && code < VSC_GDEST + VSC_NG) return !(ginfo[code - VSC_GDEST].flags & VSC_PF_ENUM);
    return 0;
}
int vsc_mod_dest_valid(unsigned code) { return dest_ok(code); }
unsigned vsc_mod_dest_count(void)
{
    unsigned n = 0;
    for (unsigned c = 0; c < VSC_GDEST + VSC_NG; ++c) n += (unsigned)dest_ok(c);
    return n;
}
unsigned vsc_mod_dest(unsigned i)
{
    for (unsigned c = 0; c < VSC_GDEST + VSC_NG; ++c) if (dest_ok(c) && i-- == 0) return c;
    return 255;
}
void vsc_mod_dest_name(unsigned code, char out[10])
{
    const char *n; char s;
    if (code < VSC_NP) { n = vsc_tparam_info(0, code)->name; s = sect_of[code]; }
    else if (code >= VSC_GDEST && code < VSC_GDEST + VSC_NG) {
        n = ginfo[code - VSC_GDEST].name;
        unsigned g = code - VSC_GDEST;
        s = g <= G_RLEVEL ? 'S' : g >= G_ASRC ? 'D' : g >= G_M0 ? 'P' : 'X';
    } else { out[0] = '-'; out[1] = '-'; out[2] = 0; return; }
    unsigned i = 0;
    out[i++] = s; out[i++] = '.';
    for (unsigned j = 0; n[j] && j < 7; ++j) out[i++] = n[j];
    out[i] = 0;
}

/* ---- the FM-1's own settings ---- */
struct vsc_device vsc_dev;
volatile uint32_t vsc_dev_changed;
void vsc_device_defaults(void)
{
    vsc_memset(&vsc_dev, 0, sizeof vsc_dev);
    vsc_dev.backlight = BACKLIGHT_LOW; vsc_dev.knob_cc = 1; vsc_dev.metro = 0;
    for (unsigned l = 0; l < VSC_LEARN; ++l) vsc_dev.learn[l].param = 255;
}
void vsc_device_seal(void)
{
    vsc_dev.magic = VSC_DEV_MAGIC; vsc_dev.version = VSC_DEV_VERSION; vsc_dev.bytes = sizeof vsc_dev;
    vsc_dev.crc = vsc_crc32(&vsc_dev.backlight, sizeof vsc_dev - offsetof(struct vsc_device, backlight));
}
static unsigned sound_param(unsigned i) { return i < P_LEVEL ? i : P_MOD0 + (i - P_LEVEL); }
void vsc_sound_store(unsigned slot, unsigned track)
{
    if (slot >= VSC_SOUNDS || track >= VSC_TRACKS) return;
    struct vsc_sound *s = &vsc_dev.sound[slot];
    for (unsigned i = 0; i < VSC_SOUND_NP; ++i) s->p[i] = vsc.t[track][sound_param(i)];
    vsc_memcpy(s->route, vsc.route[track], sizeof s->route);
    s->used = 1; ++vsc_dev_changed;
}
int vsc_sound_recall(unsigned slot, unsigned track)
{
    if (slot >= VSC_SOUNDS || track >= VSC_TRACKS || !vsc_dev.sound[slot].used) return -1;
    const struct vsc_sound *s = &vsc_dev.sound[slot];
    for (unsigned i = 0; i < VSC_SOUND_NP; ++i) {
        unsigned q = sound_param(i);
        if (s->p[i] <= vsc_tparam_info(track, q)->max) vsc.t[track][q] = s->p[i];
    }
    vsc_memcpy(vsc.route[track], s->route, sizeof s->route);
    return 0;
}
int vsc_device_valid(const struct vsc_device *d)
{
    return d->magic == VSC_DEV_MAGIC && d->version == VSC_DEV_VERSION && d->bytes == sizeof *d &&
           d->crc == vsc_crc32(&d->backlight, sizeof *d - offsetof(struct vsc_device, backlight)) &&
           d->backlight <= BACKLIGHT_HIGH;
}
