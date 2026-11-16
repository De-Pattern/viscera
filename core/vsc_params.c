/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: the parameter table, value labels, defaults, the FM-1's own
 * settings, and the small helpers everything shares. */
#include "vsc.h"

struct vsc_device vsc_dev;
volatile uint32_t vsc_dev_changed;

static const char *const n_tapespd[] = {"HI", "MID", "LO"};
static const char *const n_isrc[] = {"BT", "USB", "BOTH"};
static const char *const n_mon[] = {"OFF", "ON", "AUTO"};
static const char *const n_irec[] = {"DUB", "REPLAC"};
static const char *const n_off_on[] = {"OFF", "ON"};
static const char *const n_no_yes[] = {"NO", "YES"};
static const char *const n_bdst[] = {"AUTO", "1", "2", "3", "4"};
static const char *const n_rtgt[] = {"OFF", "TRK 1", "TRK 2", "TRK 3", "TRK 4", "INPUT"};
static const char *const n_rprof[] = {"LIQUID", "GLASS", "THROAT", "DUST", "RADIO", "DROPS"};
static const char *const n_rshape[] = {"SINE", "TRI", "SAW", "SQUARE", "S+H", "DRIFT", "GESTUR"};

#define C(n, d) {n, 127, d, 0}
#define B(n, d) {n, 127, d, VSC_PF_BIPOLAR}
#define E(n, max, d) {n, max, d, VSC_PF_ENUM}
#define STRIP(lv, arm) C("TRIM", 80), B("PAN", 64), C("LEVEL", lv), E("MUTE", 1, 0), E("ARM", 1, arm), \
    B("SPEED", 64), {"PITCH", 24, 12, VSC_PF_BIPOLAR}, C("DUB", 127)
#define BAND(f, ty) C("FREQ", f), B("GAIN", 64), C("Q", 31), E("TYPE", ty, 0)
#define ROBOT(tg) E("TARGET", VSC_RTARGETS - 1, tg), C("RATE", 40), E("SHAPE", RS_COUNT - 1, RS_SINE), B("SKEW", 64), C("DEPTH", 0)
static const struct vsc_pinfo info[VSC_NP] = {
    B("SPEED", 64), {"PITCH", 24, 12, VSC_PF_BIPOLAR}, C("LOOP", 0), C("LEVEL", 100), E("TAPE", TS_LO, TS_MID),
    E("SOURCE", IN_BOTH, IN_BOTH), C("TRIM", 80), C("DRIVE", 30), E("MONITR", MON_AUTO, MON_AUTO), E("REC", IREC_REPLACE, IREC_DUB),
    STRIP(100, 1), STRIP(100, 0), STRIP(100, 0), STRIP(100, 0),
    {"FROM", 15, 7, VSC_PF_ENUM}, E("TO", 4, 0), E("RESON", 1, 1), C("DRIVE", 30), C("LEVEL", 100), E("CLEAR", 1, 0),
    E("ON", RT_INPUT, RT_OFF), E("MODE", RP_COUNT - 1, RP_DUST), C("MIX", 100), C("DECAY", 100), C("WIDTH", 76), B("SHIFT", 64), C("DUST", 70),
    C("BAND 1", 85), C("BAND 2", 95), C("BAND 3", 90), C("BAND 4", 88), C("BAND 5", 92), C("BAND 6", 86), C("BAND 7", 95), C("BAND 8", 84),
    BAND(30, EQ_CUT), BAND(55, 0), BAND(89, 0), BAND(110, EQ_CUT),
    {"THRESH", 30, 20, 0}, E("RATIO", 2, 0), E("ATTACK", 5, 4), E("RELEAS", 4, 4), {"MAKEUP", 15, 0, 0}, C("MIX", 127), E("KEY", VSC_TRACKS + 1, 0), E("COMP", 1, 0),
    E("SOURCE", VSC_TRACKS - 1, 0), E("SLICES", 2, 1), E("SNAP", 1, 1), E("TO", 4, 0),
    ROBOT(0), ROBOT(1),
};
const struct vsc_pinfo *vsc_param_info(unsigned p) { return p < VSC_NP ? &info[p] : &info[0]; }

/* the robots' targets */
static const uint8_t rtargets[VSC_RTARGETS] = {
    P_SPEED, P_PITCH, P_TLEVEL, P_IDRIVE,
    P_TRK(0, S_LEVEL), P_TRK(1, S_LEVEL), P_TRK(2, S_LEVEL), P_TRK(3, S_LEVEL),
    P_TRK(0, S_PAN), P_TRK(1, S_PAN), P_TRK(2, S_PAN), P_TRK(3, S_PAN),
    P_RMIX, P_RDECAY, P_RWIDTH, P_RSHIFT,
    P_TRK(0, S_SPEED), P_TRK(1, S_SPEED), P_TRK(2, S_SPEED), P_TRK(3, S_SPEED),
    P_TRK(0, S_PITCH), P_TRK(1, S_PITCH), P_TRK(2, S_PITCH), P_TRK(3, S_PITCH),
};
static const char *const rtarget_name[VSC_RTARGETS] = {
    "SPEED", "PITCH", "TAPE", "DRIVE", "LVL 1", "LVL 2", "LVL 3", "LVL 4",
    "PAN 1", "PAN 2", "PAN 3", "PAN 4", "R.MIX", "R.DCY", "R.WIDE", "R.SHFT",
    "SPD 1", "SPD 2", "SPD 3", "SPD 4", "PIT 1", "PIT 2", "PIT 3", "PIT 4",
};
unsigned vsc_robot_target(unsigned i) { return i < VSC_RTARGETS ? rtargets[i] : VSC_NP; }
static void put_str(char *o, const char *s) { unsigned i = 0; while (s[i] && i < 7) { o[i] = s[i]; ++i; } o[i] = 0; }
void vsc_target_name(unsigned i, char out[8]) { put_str(out, i < VSC_RTARGETS ? rtarget_name[i] : "-"); }
/* what the engine takes from the moved values each block (the EQ and compressor take their
 * knobs as set: their curves are worked out only when a knob moves) */
int vsc_modulatable(unsigned p)
{
    if (p == P_SPEED || p == P_PITCH || p == P_TLEVEL || p == P_ITRIM || p == P_IDRIVE) return 1;
    if (p >= P_STRIP && p < P_STRIP + VSC_TRACKS * VSC_TP) { unsigned k = (p - P_STRIP) % VSC_TP; return k != S_MUTE && k != S_ARM; }
    if (p == P_BDRIVE || p == P_BLEVEL) return 1;
    return p >= P_RMIX && p <= P_RG0 + 7;
}
void vsc_route_name(unsigned p, char out[10])
{
    const char *pre = "", *n = p < VSC_NP ? info[p].name : "-";
    char t[4] = "T1 ";
    if (p == P_SPEED) n = "TAPE SPD";
    else if (p == P_PITCH) n = "TAPE PIT";
    else if (p == P_TLEVEL) n = "TAPE LVL";
    else if (p == P_ITRIM || p == P_IDRIVE) pre = "IN ";
    else if (p == P_BDRIVE || p == P_BLEVEL) pre = "BN ";
    else if (p >= P_RMIX && p <= P_RG0 + 7) pre = "R ";
    else if (p >= P_STRIP && p < P_STRIP + VSC_TRACKS * VSC_TP) { t[1] = (char)('1' + (p - P_STRIP) / VSC_TP); pre = t; }
    unsigned i = 0;
    for (; *pre && i < 9; ++pre) out[i++] = *pre;
    for (; *n && i < 9; ++n) out[i++] = *n;
    out[i] = 0;
}

static void put_int(char *o, int v)
{
    char t[8]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 6);
    int i = 0; if (neg) o[i++] = '-';
    while (n) o[i++] = t[--n];
    o[i] = 0;
}
/* v/100 as "1.03" (two decimals), then a unit */
static void put_fix2(char *o, int v, const char *unit)
{
    char b[8]; int neg = v < 0; if (neg) v = -v;
    put_int(b, v / 100);
    unsigned i = 0;
    if (neg) o[i++] = '-';
    for (unsigned k = 0; b[k]; ++k) o[i++] = b[k];
    o[i++] = '.'; o[i++] = (char)('0' + v / 10 % 10); o[i++] = (char)('0' + v % 10);
    for (unsigned k = 0; unit[k] && i < 7; ++k) o[i++] = unit[k];
    o[i] = 0;
}
static void put_unit(char *o, int v, const char *unit)
{
    put_int(o, v);
    unsigned i = (unsigned)vsc_strlen(o);
    for (unsigned k = 0; unit[k] && i < 7; ++k) o[i++] = unit[k];
    o[i] = 0;
}
static float pexp2(float x)   /* 2^x, near enough for a label */
{
    int i = (int)(x < 0.f ? x - 1.f : x); float f = x - (float)i, m = 1.f + f * (0.6565f + f * 0.3435f);
    while (i > 0) { m *= 2.f; --i; } while (i < 0) { m *= 0.5f; ++i; }
    return m;
}
static float plog10(float x)   /* log10, near enough for a label */
{
    int e = 0; while (x >= 2.f) { x *= 0.5f; ++e; } while (x < 1.f && e > -40) { x *= 2.f; --e; }
    float l2 = (float)e + (x - 1.f) * (1.4425f - 0.4425f * (x - 1.f));
    return l2 * 0.30103f;
}
/* the strips' and the input's trims, and the tape's and strips' levels, in dB */
static int db_of(unsigned p, unsigned v)
{
    if (p == P_ITRIM || (p >= P_STRIP && p < P_BSRC && (p - P_STRIP) % VSC_TP == S_TRIM)) return ((int)v - 80) * 3 / 10;
    float g = (float)v * (1.f / 100.f); g = g * g;   /* (the engine's level curve: square) */
    return (int)(20.f * plog10(g) + (g < 1.f ? -0.5f : 0.5f));
}
static const float rb_base[VSC_RBANDS] = {200.f, 400.f, 800.f, 1200.f, 1600.f, 2400.f, 3200.f, 4800.f};
float vsc_reson_hz(unsigned b) { return rb_base[b & 7u] * pexp2(((float)vsc.p[P_RSHIFT] - 64.f) * (1.f / 64.f)); }
float vsc_tape_hz(unsigned s) { return s == TS_HI ? 22050.f : s == TS_MID ? 16000.f : 11025.f; }

void vsc_param_label(unsigned p, unsigned v, char out[8])
{
    out[0] = 0;
    switch (p) {
    case P_SPEED: put_fix2(out, (int)(pexp2(((float)v - 64.f) * (1.f / 64.f)) * 100.f + 0.5f), "X"); return;
    case P_PITCH: { int st = (int)v - 12; if (st > 0) { out[0] = '+'; put_unit(out + 1, st, "ST"); } else put_unit(out, st, "ST"); return; }
    case P_LOOP: if (!v) put_str(out, "FREE"); else put_fix2(out, (int)v * 5, "S"); return;
    case P_TAPESPD: put_str(out, n_tapespd[v % 3]); return;
    case P_ISRC: put_str(out, n_isrc[v % 3]); return;
    case P_IMON: put_str(out, n_mon[v % 3]); return;
    case P_IREC: put_str(out, n_irec[v & 1]); return;
    case P_BSRC: {
        unsigned i = 0;
        for (unsigned t = 0; t < VSC_TRACKS; ++t) if (v >> t & 1u) { if (i) out[i++] = '+'; out[i++] = (char)('1' + t); }
        if (!i) out[i++] = '-';
        out[i] = 0; return;
    }
    case P_BDST: put_str(out, n_bdst[v % 5]); return;
    case P_BRESON: put_str(out, n_off_on[v & 1]); return;
    case P_BCLEAR: put_str(out, n_no_yes[v & 1]); return;
    case P_RTGT: put_str(out, n_rtgt[v % 6]); return;
    case P_RPROF: put_str(out, n_rprof[v % RP_COUNT]); return;
    case P_RSHIFT: { int st = ((int)v - 64) * 12 / 64; if (st > 0) { out[0] = '+'; put_unit(out + 1, st, "ST"); } else put_unit(out, st, "ST"); return; }
    case P_RDECAY: { float s = 0.01f * pexp2((float)v * (8.f / 127.f)); put_fix2(out, (int)(s * 100.f + 0.5f), "S"); return; }
    default: break;
    }
    if (p >= P_EQ0 && p < P_CTHR) {
        unsigned b = (p - P_EQ0) / 4, k = (p - P_EQ0) % 4;
        if (k == E_FREQ) { int hz = (int)(20.f * pexp2((float)v * (9.965784f / 127.f)) + 0.5f);
                           if (hz >= 1000) put_fix2(out, hz / 10, "K"); else put_unit(out, hz, "HZ"); return; }
        if (k == E_GAIN) { int d = ((int)v - 64) * 15 / 64; if (d > 0) { out[0] = '+'; put_unit(out + 1, d, "DB"); } else put_unit(out, d, "DB"); return; }
        if (k == E_Q) { float q = 0.3f * pexp2((float)v * (5.f / 127.f)); put_fix2(out, (int)(q * 100.f + 0.5f), ""); return; }
        static const char *const lo[3] = {"BELL", "SHELF", "LO CUT"}, *const hi[3] = {"BELL", "SHELF", "HI CUT"};
        put_str(out, b == 0 ? lo[v % 3] : b == 3 ? hi[v % 3] : "BELL");
        return;
    }
    switch (p) {
    case P_CTHR: put_unit(out, (int)v - 30, "DB"); return;
    case P_CRATIO: { static const char *const r[3] = {"2:1", "4:1", "10:1"}; put_str(out, r[v % 3]); return; }
    case P_CATK: { static const char *const a[6] = {"0.1MS", "0.3MS", "1MS", "3MS", "10MS", "30MS"}; put_str(out, a[v % 6]); return; }
    case P_CREL: { static const char *const r[5] = {"0.1S", "0.3S", "0.6S", "1.2S", "AUTO"}; put_str(out, r[v % 5]); return; }
    case P_CMAKE: out[0] = '+'; put_unit(out + 1, (int)v, "DB"); return;
    case P_CKEY: if (!v) put_str(out, "MIX"); else if (v > VSC_TRACKS) put_str(out, "INPUT"); else { put_str(out, "TRK "); out[4] = (char)('0' + v); out[5] = 0; } return;
    case P_CON: put_str(out, v ? "ON" : "OFF"); return;
    case P_SLSRC: put_str(out, "TRK "); out[4] = (char)('1' + (v & 3u)); out[5] = 0; return;
    case P_SLN: put_unit(out, 4 << (v % 3), ""); return;
    case P_SLSNAP: put_str(out, v ? "HITS" : "GRID"); return;
    case P_SLDST: put_str(out, n_bdst[v % 5]); return;
    default: break;
    }
    if (p >= P_RG0 && p <= P_RG7) { if (v < 1) put_str(out, "OFF"); else put_unit(out, -60 + (int)v * 66 / 127, "DB"); return; }
    if (p >= P_ROB0) {
        unsigned k = (p - P_ROB0) % 5;
        if (k == R_TARGET) { vsc_target_name(v, out); return; }
        if (k == R_SHAPE) { put_str(out, n_rshape[v % RS_COUNT]); return; }
        if (k == R_RATE) { float hz = 0.02f * pexp2((float)v * (9.f / 127.f)); if (hz < 1.f) put_fix2(out, (int)(hz * 100.f + 0.5f), "HZ"); else put_unit(out, (int)(hz + 0.5f), "HZ"); return; }
        if (k == R_DEPTH) { put_unit(out, (int)v * 100 / 127, "%"); return; }
    }
    if (p == P_ITRIM || p == P_TLEVEL || p == P_BLEVEL || (p >= P_STRIP && p < P_BSRC && ((p - P_STRIP) % VSC_TP == S_TRIM || (p - P_STRIP) % VSC_TP == S_LEVEL))) {
        if (!v) { put_str(out, "OFF"); return; }
        int db = db_of(p, v);
        if (db > 0) { out[0] = '+'; put_unit(out + 1, db, "DB"); } else put_unit(out, db, "DB");
        return;
    }
    if (p >= P_STRIP && p < P_BSRC) {
        unsigned k = (p - P_STRIP) % VSC_TP;
        if (k == S_MUTE || k == S_ARM) { put_str(out, n_off_on[v & 1]); return; }
        if (k == S_SPEED) { vsc_param_label(P_SPEED, v, out); return; }
        if (k == S_PITCH) { vsc_param_label(P_PITCH, v, out); return; }
        if (k == S_PAN) { int d = (int)v - 64; if (!d) put_str(out, "C"); else { out[0] = d < 0 ? 'L' : 'R'; put_int(out + 1, d < 0 ? -d : d); } return; }
    }
    if (p == P_IDRIVE || p == P_BDRIVE) {
        put_str(out, v < 20 ? "CLEAN" : v < 60 ? "WARM" : v < 100 ? "HOT" : "FUZZ"); return;
    }
    const struct vsc_pinfo *in = vsc_param_info(p);
    if (in->flags & VSC_PF_BIPOLAR) { int d = (int)v - 64; if (d > 0) { out[0] = '+'; put_int(out + 1, d); } else put_int(out, d); return; }
    put_unit(out, (int)v * 100 / 127, "%");
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

void vsc_project_defaults(struct vsc_project *p)
{
    vsc_memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < VSC_NP; ++i) p->p[i] = info[i].def;
    /* the gestures: flat, a second long */
    for (unsigned r = 0; r < VSC_ROBOTS; ++r) p->gest_ms[r] = 1000;
}
int vsc_project_valid(const struct vsc_project *p)
{
    if (p->magic != VSC_MAGIC || p->version != VSC_FORMAT || p->bytes != sizeof *p) return 0;
    if (vsc_crc32(&p->name, sizeof *p - offsetof(struct vsc_project, name)) != p->crc) return 0;
    for (unsigned i = 0; i < VSC_NP; ++i) if (p->p[i] > info[i].max) return 0;
    return 1;
}
int vsc_project_upgrade(struct vsc_project *p)
{
    if (p->magic != VSC_MAGIC || p->version != 10 || p->bytes != VSC_FORMAT_10_BYTES) return 0;
    if (vsc_crc32(&p->name, VSC_FORMAT_10_BYTES - offsetof(struct vsc_project, name)) != p->crc) return 0;
    vsc_memset(p->route, 0, sizeof p->route);
    p->version = VSC_FORMAT; p->bytes = sizeof *p;
    p->crc = vsc_crc32(&p->name, sizeof *p - offsetof(struct vsc_project, name));
    return 1;
}

void vsc_device_defaults(void)
{
    vsc_memset(&vsc_dev, 0, sizeof vsc_dev);
    vsc_dev.backlight = BACKLIGHT_LOW; vsc_dev.knob_cc = 1;
    for (unsigned l = 0; l < VSC_LEARN; ++l) vsc_dev.learn[l].param = 255;
}
void vsc_device_seal(void)
{
    vsc_dev.magic = VSC_DEV_MAGIC; vsc_dev.version = VSC_DEV_VERSION; vsc_dev.bytes = sizeof vsc_dev;
    vsc_dev.crc = vsc_crc32(&vsc_dev.backlight, sizeof vsc_dev - offsetof(struct vsc_device, backlight));
}
int vsc_device_valid(const struct vsc_device *d)
{
    if (d->magic != VSC_DEV_MAGIC || d->version != VSC_DEV_VERSION || d->bytes != sizeof *d) return 0;
    if (d->crc != vsc_crc32(&d->backlight, sizeof *d - offsetof(struct vsc_device, backlight))) return 0;
    for (unsigned l = 0; l < VSC_LEARN; ++l) {
        const struct vsc_learn *e = &d->learn[l];
        if (e->param != 255 && (e->kind > 1 || (e->kind == 0 && e->param >= VSC_NP) || (e->kind == 1 && e->param >= ACT_COUNT))) return 0;
    }
    return d->backlight <= BACKLIGHT_HIGH;
}
