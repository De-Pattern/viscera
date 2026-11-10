/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: parameter table, labels, defaults, scenes and routing. */
#include "vsc.h"

static const char *const n_mode[] = {"PLAY", "LOOP", "KEYS", "THRU"};
static const char *const n_loopm[] = {"ONCE", "LOOP", "BOUNCE"};
static const char *const n_input[] = {"BT", "USB", "BT+USB", "T1", "T2", "T3", "T4", "MAIN"};
static const char *const n_qual[] = {"HI", "LO"};
static const char *const n_off_on[] = {"OFF", "ON"};
static const char *const n_gquant[] = {"FREE", "SEMI", "OCT", "5TH", "TUNE"};
static const char *const n_gtrig[] = {"FREE", "HITS", "NOTES"};
static const char *const n_ftype[] = {"LP", "BP", "HP", "PEAKS", "RESON", "OFF", "REBLD"};
static const char *const n_fscale[] = {"OCT", "5TH", "MAJ", "MIN", "SUS", "HARM", "WHOLE", "CLUST"};
static const char *const n_kscale[] = {"CHROM", "MAJOR", "MINOR", "PENTA", "BLUES", "WHOLE", "FIFTHS", "OCTAVE",
                                        "HARM", "MEMBR", "BAR"};
static const char *const n_kdmode[] = {"SUSTN", "CHOKE"};
static const char *const n_wmode[] = {"MIX", "SEND"};
static const char *const n_cmode[] = {"BITS", "REDUX", "BOTH"};
static const char *const n_xover[] = {"LOW", "MID", "HIGH"};
static const char *const n_ntype[] = {"WHITE", "PINK", "DUST"};
static const char *const n_mtype[] = {"WAVE", "RANDOM", "FOLLOW", "ADSR"};
static const char *const n_wshape[] = {"SINE", "TRI", "SQUARE", "SAW", "RAMP", "S+H"};
static const char *const n_sync[] = {"FREE", "STRGHT", "TRIPLT", "DOTTED"};
static const char *const n_fin[] = {"TRACK", "MATER", "T1", "T2", "T3", "T4", "MAIN", "INPUT",
                                    "LOW", "MID", "HIGH", "HITS", "PITCH", "BRIGHT"};
static const char *const n_afocus[] = {"FULL", "LOW", "MID", "HIGH"};
static const char *const n_ahits[] = {"OFF", "BALLS", "STEPS"};
static const char *const n_parts[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const n_unvoic[] = {"HOLD", "FADE"};
static const char *const n_dtime[] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4"};
static const char *const n_clock[] = {"AUTO", "INT", "MIDI"};
static const char *const n_seqm[] = {"STEPS", "CAROM"};
static const char *const n_rquant[] = {"FREE", "1/16", "1/8"};
static const char *const n_rwalls[] = {"OCTAVE", "SAME", "5TH"};

#define C(n, d) {n, 127, d, 0, 0}
#define B(n, d) {n, 127, d, VSC_PF_BIPOLAR, 0}
#define E(n, list, d) {n, sizeof list / sizeof list[0] - 1, d, VSC_PF_ENUM, list}
#define X {"", 0, 0, VSC_PF_ENUM, 0}
#define MODSLOT E("TYPE", n_mtype, 0), X, X, X, X, X, X, X, X, X

static const struct vsc_pinfo tinfo[VSC_NP] = {
    /* MATERIAL */
    E("DEVICE", n_mode, 0), {"SAMPLE", 16, 0, VSC_PF_ENUM, 0}, C("START", 0), C("LENGTH", 127),
    B("SPEED", 96), {"PITCH", 48, 24, VSC_PF_BIPOLAR, 0},
    E("LOOP", n_loopm, 1), E("INPUT", n_input, 0), C("SOS", 100), {"SIZE", 3, 0, VSC_PF_ENUM, 0},
    E("RATE", n_qual, 0), C("LEVEL", 100),
    C("ATTACK", 0), C("DECAY", 64), C("SUSTN", 127), C("RELEAS", 30), C("GLIDE", 0), C("VELO", 64),
    /* GRAIN */
    C("WET", 0), C("SIZE", 50), C("RATE", 60), {"PITCH", 48, 24, VSC_PF_BIPOLAR, 0}, C("SPRAY", 20), B("SHAPE", 64),
    C("SCAN", 30), C("REVRS", 0), E("LOCK", n_off_on, 0), C("DETUNE", 0), E("QUANT", n_gquant, 0), C("FDBK", 0),
    E("W.MODE", n_wmode, 0),
    /* FILTER */
    E("DEVICE", n_ftype, 4), C("CUTOFF", 120), C("RESO", 20), C("SPREAD", 64), E("SCALE", n_fscale, 0), C("WET", 127),
    E("W.MODE", n_wmode, 0),
    C("DECAY", 70), {"PITCH", 48, 24, VSC_PF_BIPOLAR, 0}, B("SLOPE", 64), B("TONE", 64), E("SCALE", n_kscale, 0), C("WAVES", 0),
    C("W.RATE", 40), C("NOISE", 0), C("N.RATE", 50), B("TILT", 64), C("DETUNE", 0), E("D.MODE", n_kdmode, 0),
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
    /* DISSECT */
    E("TRIG", n_gtrig, 0), C("EXCITE", 0), E("FOLLOW", n_off_on, 0),
};
/* REBUILD (FILTER) uses RESON's parameters for its own: the partials it rebuilds from
 * the listener, how far they shift, how slowly they follow, their stretch and tilt */
static const struct vsc_pinfo rebuild_tab[] = {
    E("PARTS", n_parts, 5), {"SHIFT", 48, 24, VSC_PF_BIPOLAR, 0}, C("SMEAR", 70), C("STRTCH", 0), B("TILT", 64),
    C("DETUNE", 0), E("UNVOIC", n_unvoic, 0), E("SNAP", n_off_on, 0),
};
static const struct vsc_pinfo *rebuild_info(unsigned p)
{
    switch (p) {
    case P_KSCALE: return &rebuild_tab[0];
    case P_KPITCH: return &rebuild_tab[1];
    case P_KDECAY: return &rebuild_tab[2];
    case P_KWAVES: return &rebuild_tab[3];
    case P_KTILT: return &rebuild_tab[4];
    case P_KDETUN: return &rebuild_tab[5];
    case P_KDMODE: return &rebuild_tab[6];
    case P_KFOLLOW: return &rebuild_tab[7];
    default: return 0;
    }
}
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
    if (track < VSC_TRACKS && vsc.t[track][P_FTYPE] == FT_REBUILD) { const struct vsc_pinfo *r = rebuild_info(p); if (r) return r; }
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
void vsc_param_label(const struct vsc_pinfo *info, unsigned p, unsigned v, char out[8], int global)
{
    out[0] = 0;
    if (!info->name[0]) return;
    if (info->names) { put_str(out, info->names[v > info->max ? info->max : v]); return; }
    if (!global) {
        if (p == P_SLOT) {
            if (v >= VSC_BANK_SLOTS) { put_str(out, "LOOP"); return; }
            out[0] = 'S'; put_int(out + 1, (int)v + 1); return;
        }
        if (p == P_PITCH || p == P_GPITCH || p == P_KPITCH) { put_int(out, (int)v - 24); return; }
        if (p == P_SIZE || p == P_RCOUNT) { put_int(out, (int)v + 1); return; }
        if (p > P_MOD0 && p < P_MOD0 + 4 * VSC_MOD_PARAMS && (p - P_MOD0) % VSC_MOD_PARAMS == 5 && !info->names && info->max == 15) { put_int(out, (int)v + 1); return; }
        if (p == P_SPEED) {
            /* -2.00 .. +2.00 */
            int c = ((int)v - 64) * 100 / 32;
            int neg = c < 0; if (neg) c = -c;
            int i = 0;
            if (neg) out[i++] = '-';
            out[i++] = (char)('0' + c / 100); out[i++] = '.';
            out[i++] = (char)('0' + (c / 10) % 10); out[i++] = (char)('0' + c % 10);
            out[i++] = 'X'; out[i] = 0; return;
        }
    } else if (p == G_TARGET) { out[0] = 'S'; out[1] = 'C'; put_int(out + 2, (int)v + 1); return; }
    else if (p == G_MSEL) { out[0] = 'M'; put_int(out + 1, (int)v + 1); return; }
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
    /* A useful start: track 1 plays the first sample, track 2 loops the
     * Bluetooth input, track 3 plays notes, track 4 passes input through. */
    static const uint8_t modes[VSC_TRACKS] = {MODE_PLAY, MODE_LOOP, MODE_KEYS, MODE_THRU};
    p->t[t][P_MODE] = modes[t % VSC_TRACKS];
    p->t[t][P_SLOT] = (uint8_t)t;
    p->t[t][P_SIZE] = 0;
    if (p->t[t][P_MODE] == MODE_KEYS) p->t[t][P_LOOPM] = 1;
    p->length[t] = 16;
    for (unsigned s = 0; s < VSC_STEPS; ++s) {
        struct vsc_step *st = &p->step[t][s];
        vsc_memset(st, 0, sizeof *st);
        st->note = 60; st->vel = 100; st->lp[0] = st->lp[1] = 255;
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

/* ---- format 4 (0.2.x) projects: the same layout without the DISSECT parameters ---- */
struct vsc_scene_v4 { uint8_t used, pad[3]; uint8_t t[VSC_TRACKS][VSC_NP_V4]; uint8_t macro[VSC_MACROS]; };
struct vsc_project_v4 {
    uint32_t magic, version, bytes, crc;
    char name[12];
    uint16_t tempo, pad0;
    uint8_t t[VSC_TRACKS][VSC_NP_V4];
    uint8_t g[VSC_NG_V4];
    uint8_t length[VSC_TRACKS];
    struct vsc_route route[VSC_TRACKS][VSC_MOD_ROUTES];
    struct vsc_mroute macro[VSC_MACROS][VSC_MACRO_ROUTES];
    struct vsc_step step[VSC_TRACKS][VSC_STEPS];
    struct vsc_scene_v4 scene[VSC_SCENES];
};
unsigned vsc_project_v4_bytes(void) { return sizeof(struct vsc_project_v4); }
/* overlapping copy, for moving fields up in place */
static void move_up(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    if (a <= b) { while (n--) *a++ = *b++; return; }
    while (n--) a[n] = b[n];
}
#define OLD(f) ((const uint8_t *)p + offsetof(struct vsc_project_v4, f))
int vsc_project_upgrade(struct vsc_project *p, int check_crc)
{
    const struct vsc_project_v4 *o = (const struct vsc_project_v4 *)p;
    if (o->magic != VSC_MAGIC || o->version != 4 || o->bytes != sizeof(struct vsc_project_v4)) return -1;
    if (check_crc && vsc_crc32(&o->name, sizeof *o - offsetof(struct vsc_project_v4, name)) != o->crc) return -1;
    /* every field moves up (or stays): copy from the last to the first, so nothing is
     * overwritten before it has moved */
    for (int sc = VSC_SCENES - 1; sc >= 0; --sc) {
        const uint8_t *os = OLD(scene) + (size_t)sc * sizeof(struct vsc_scene_v4);
        struct vsc_scene *ns = &p->scene[sc];
        move_up(ns->macro, os + offsetof(struct vsc_scene_v4, macro), VSC_MACROS);
        for (int t = VSC_TRACKS - 1; t >= 0; --t) {
            move_up(ns->t[t], os + offsetof(struct vsc_scene_v4, t) + (size_t)t * VSC_NP_V4, VSC_NP_V4);
            for (unsigned q = VSC_NP_V4; q < VSC_NP; ++q) ns->t[t][q] = tinfo[q].def;
        }
        move_up(ns, os, 4);   /* used and pad */
    }
    move_up(p->step, OLD(step), sizeof p->step);
    move_up(p->macro, OLD(macro), sizeof p->macro);
    move_up(p->route, OLD(route), sizeof p->route);
    move_up(p->length, OLD(length), sizeof p->length);
    move_up(p->g, OLD(g), VSC_NG_V4);
    for (unsigned g = VSC_NG_V4; g < VSC_NG; ++g) p->g[g] = ginfo[g].def;
    for (int t = VSC_TRACKS - 1; t >= 0; --t) {
        move_up(p->t[t], OLD(t) + (size_t)t * VSC_NP_V4, VSC_NP_V4);
        for (unsigned q = VSC_NP_V4; q < VSC_NP; ++q) p->t[t][q] = tinfo[q].def;
    }
    p->version = VSC_FORMAT; p->bytes = sizeof *p;
    p->crc = vsc_crc32(&p->name, sizeof *p - offsetof(struct vsc_project, name));
    return 0;
}
#undef OLD

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
    /* Material mode and loop sizes change buffers: keep the running ones. */
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned p = 0; p < VSC_NP; ++p) {
            if (p == P_MODE || p == P_SIZE || p == P_QUAL || p == P_INPUT) continue;
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
    'M','M','M','M','M','M','M','M','M','M','M','M','M','M','M','M','M','M',
    'G','G','G','G','G','G','G','G','G','G','G','G','G',
    'F','F','F','F','F','F','F',
    'F','F','F','F','F','F','F','F','F','F','F','F',
    'C','C','C','C','C','C','C','C','C','C','C','C',
    'X','X','X','X','X',
    '1','1','1','1','1','1','1','1','1','1','2','2','2','2','2','2','2','2','2','2',
    '3','3','3','3','3','3','3','3','3','3','4','4','4','4','4','4','4','4','4','4',
    'Q','Q','Q','Q','Q','Q','Q','Q','Q','Q',
    'G','F','F'};
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
    vsc_dev.magic = VSC_DEV_MAGIC; vsc_dev.version = 2; vsc_dev.bytes = sizeof vsc_dev;
    vsc_dev.crc = vsc_crc32(&vsc_dev.backlight, sizeof vsc_dev - offsetof(struct vsc_device, backlight));
}
static unsigned sound_param(unsigned i) { return i < P_LEVEL ? i : P_MOD0 + (i - P_LEVEL); }
void vsc_sound_store(unsigned slot, unsigned track)
{
    if (slot >= VSC_SOUNDS || track >= VSC_TRACKS) return;
    struct vsc_sound *s = &vsc_dev.sound[slot];
    for (unsigned i = 0; i < VSC_SOUND_NP; ++i) s->p[i] = vsc.t[track][sound_param(i)];
    s->ext[0] = vsc.t[track][P_GTRIG]; s->ext[1] = vsc.t[track][P_KEXCITE]; s->ext[2] = vsc.t[track][P_KFOLLOW];
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
    static const uint8_t ext_param[3] = {P_GTRIG, P_KEXCITE, P_KFOLLOW};
    for (unsigned i = 0; i < 3; ++i) if (s->ext[i] <= tinfo[ext_param[i]].max) vsc.t[track][ext_param[i]] = s->ext[i];
    vsc_memcpy(vsc.route[track], s->route, sizeof s->route);
    return 0;
}
int vsc_device_valid(const struct vsc_device *d)
{
    return d->magic == VSC_DEV_MAGIC && d->version == 2 && d->bytes == sizeof *d &&
           d->crc == vsc_crc32(&d->backlight, sizeof *d - offsetof(struct vsc_device, backlight)) &&
           d->backlight <= BACKLIGHT_HIGH;
}
