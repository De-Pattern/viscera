/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: screen and controls. Portable: the platform hands in a control
 * snapshot and asks for 16-row bands of RGB565 pixels. Control context only. */
#include "vsc.h"

struct vsc_host_status vsc_host;

/* ---------------------------------------------------------------- colour */
#define RGB(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))
static const uint16_t C_BG = RGB(0, 0, 0), C_PANEL = RGB(30, 29, 27), C_LINE = RGB(52, 50, 46),
    C_DIM = RGB(111, 107, 100), C_MID = RGB(169, 163, 151), C_FG = RGB(230, 223, 209),
    C_INK = RGB(243, 239, 231), C_WARN = RGB(196, 120, 96);
/* the modulator colours, as on the S-4's action buttons: blue, yellow, green, red */
static const uint16_t C_MOD[4] = {RGB(96, 168, 228), RGB(226, 204, 92), RGB(116, 204, 148), RGB(228, 108, 92)};
static unsigned hash3(int a, int b, unsigned c)
{
    unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static uint16_t mix565(uint16_t a, uint16_t b, int t256)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (br - ar) * t256 / 256) << 11) | ((ag + (bg - ag) * t256 / 256) << 5) | (ab + (bb - ab) * t256 / 256));
}

/* ------------------------------------------------------------------ font */
/* 5x7, ASCII 32..95, column bytes with bit 0 at the top. */
static const uint8_t font[64][5] = {
    {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},{0x24,0x2a,0x7f,0x2a,0x12},
    {0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},{0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},
    {8,0x2a,0x1c,0x2a,8},{8,8,0x3e,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{0,8,0x14,0x22,0x41},
    {0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,8,0},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,1,1},{0x3e,0x41,0x41,0x51,0x32},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},{0x7f,2,4,2,0x7f},
    {0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f},{0x63,0x14,8,0x14,0x63},{3,4,0x78,4,3},{0x61,0x51,0x49,0x45,0x43},
    {0,0,0x7f,0x41,0x41},{2,4,8,0x10,0x20},{0x41,0x41,0x7f,0,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40}};

/* ------------------------------------------------------- band painting */
static uint16_t *band; static int by0;   /* current band and its first row */
static void px(int x, int y, uint16_t c)
{
    y -= by0;
    if ((unsigned)x < VSC_SCREEN_W && (unsigned)y < VSC_BAND) band[y * VSC_SCREEN_W + x] = c;
}
static void rect(int x, int y, int w, int h, uint16_t c)
{
    int y1 = y + h, a = y < by0 ? by0 : y, b = y1 > by0 + VSC_BAND ? by0 + VSC_BAND : y1;
    if (x < 0) { w += x; x = 0; }
    if (x + w > VSC_SCREEN_W) w = VSC_SCREEN_W - x;
    for (int yy = a; yy < b; ++yy) for (int xx = 0; xx < w; ++xx) band[(yy - by0) * VSC_SCREEN_W + x + xx] = c;
}
static void hline(int x, int y, int w, uint16_t c) { rect(x, y, w, 1, c); }
static void vline(int x, int y, int h, uint16_t c) { rect(x, y, 1, h, c); }
static int text(int x, int y, const char *s, uint16_t c, int scale)
{
    if (y + 8 * scale <= by0 || y >= by0 + VSC_BAND) { while (*s) { x += 6 * scale; ++s; } return x; }
    for (; *s; ++s) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font[ch - 32];
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 7; ++cy)
                if (g[cx] >> cy & 1) {
                    if (scale == 1) px(x + cx, y + cy, c);
                    else rect(x + cx * scale, y + cy * scale, scale, scale, c);
                }
        x += 6 * scale;
    }
    return x;
}
static int text_n(int x, int y, const char *s, uint16_t c, unsigned max_chars)
{
    char buf[48]; unsigned i = 0;
    while (s[i] && i < max_chars && i < sizeof buf - 1) { buf[i] = s[i]; ++i; }
    buf[i] = 0;
    return text(x, y, buf, c, 1);
}
static int text_w(const char *s, int scale) { return (int)vsc_strlen(s) * 6 * scale; }
static void num(char *o, int v)
{
    char t[8]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 7);
    int i = 0; if (neg) o[i++] = '-';
    while (n) o[i++] = t[--n];
    o[i] = 0;
}

/* ------------------------------------------------------------- pages
 * Each button opens a menu, and every menu is a set of views of four parameters, one
 * for each of KNOB 1-4. SELECT steps through a menu's views (the picture above stays
 * the same), ALGORITHM picks the track, and PRESETS does the page's own job: the
 * device in a slot, a modulator's type, a route, a step, a project slot. */
enum { PG_MIX, PG_MAT, PG_GRAIN, PG_FILTER, PG_COLOR, PG_MODS, PG_SPACE, PG_SCENE, PG_STEPS, PG_PROJECT, PG_SETTINGS, PG_SOUND, PG_COUNT };
static const char *const page_name[PG_COUNT] = {"MIX", "MATERIAL", "GRAIN", "FILTER", "COLOR", "MODS", "SPACE",
                                                "PERFORM", "STEPS", "PROJECT", "SETTINGS", "SOUNDS"};
enum { S_NONE, S_T, S_G, S_TEMPO, S_STEP, S_TT, S_SET, S_MAT };
struct slot { uint8_t kind, id, tk; };
#define T(p) {S_T, p, 0}
#define G(p) {S_G, p, 0}
#define N {S_NONE, 0, 0}
#define TT(p) {{S_TT, p, 0}, {S_TT, p, 1}, {S_TT, p, 2}, {S_TT, p, 3}}
static const struct slot v_mix[6][4] = {TT(P_LEVEL), TT(P_PAN), TT(P_SEND), TT(P_DJF), TT(P_MUTE),
                                        {G(G_MAIN), G(G_MCOMP), G(G_INMON), N}};
static const struct slot v_play[2][4] = {{T(P_SLOT), T(P_SPEED), T(P_START), T(P_LEN)}, {T(P_PITCH), T(P_GAIN), T(P_LOOPM), N}};
static const struct slot v_loop[3][4] = {{T(P_SPEED), T(P_START), T(P_LEN), T(P_GAIN)}, {T(P_DUB), T(P_INPUT), T(P_SIZE), T(P_QUAL)},
                                         {T(P_LOOPM), T(P_PITCH), N, N}};
static const struct slot v_keys[4][4] = {{T(P_PITCH), T(P_START), T(P_LEN), T(P_GAIN)}, {T(P_SLOT), T(P_LOOPM), T(P_GLIDE), T(P_VELO)},
                                         {T(P_ATK), T(P_DEC), T(P_SUS), T(P_REL)}, {T(P_SPEED), N, N, N}};
static const struct slot v_thru[1][4] = {{T(P_INPUT), T(P_GAIN), N, N}};
static const struct slot v_grain[3][4] = {{T(P_GPITCH), T(P_GDENS), T(P_GSIZE), T(P_GSHAPE)},
                                          {T(P_GTIME), T(P_GSPRAY), T(P_GDETUNE), T(P_GMIX)},
                                          {T(P_GREV), T(P_GFDBK), T(P_GQUANT), T(P_GWMODE)}};
static const struct slot v_reson[4][4] = {{T(P_FCUT), T(P_FRES), T(P_KDECAY), T(P_KPITCH)},
                                          {T(P_KSLOPE), T(P_KTONE), T(P_KSCALE), T(P_FMIX)},
                                          {T(P_KWAVES), T(P_KWRATE), T(P_KNOISE), T(P_KNRATE)},
                                          {T(P_KTILT), T(P_KDETUN), T(P_KDMODE), T(P_FWMODE)}};
static const struct slot v_svf[1][4] = {{T(P_FCUT), T(P_FRES), T(P_FMIX), T(P_FWMODE)}};
static const struct slot v_peaks[2][4] = {{T(P_FCUT), T(P_FRES), T(P_FSPREAD), T(P_FSCALE)}, {T(P_FMIX), T(P_FWMODE), N, N}};
static const struct slot v_color[3][4] = {{T(P_DRIVE), T(P_COMP), T(P_CRUSH), T(P_TILT)},
                                          {T(P_NOISE), T(P_NDECAY), T(P_NTONE), T(P_CMIX)},
                                          {T(P_CMODE), T(P_XOVER), T(P_NTYPE), T(P_CWMODE)}};
static const struct slot v_space[2][4] = {{G(G_DLEVEL), G(G_DTIME), G(G_DFEED), G(G_DTONE)},
                                          {G(G_RLEVEL), G(G_RSIZE), G(G_RDAMP), G(G_DLONG)}};
static const struct slot v_scene[3][4] = {{G(G_M0), G(G_M1), G(G_M2), G(G_M3)}, {G(G_M4), G(G_M5), G(G_M6), G(G_M7)},
                                          {G(G_TARGET), G(G_MORPH), G(G_MSEL), N}};
static const struct slot v_steps[2][4] = {{{S_STEP, 0, 0}, {S_STEP, 1, 0}, {S_STEP, 2, 0}, {S_STEP, 3, 0}},
                                          {{S_STEP, 4, 0}, {S_STEP, 5, 0}, T(P_SEQM), N}};
static const struct slot v_carom[3][4] = {{T(P_SEQM), T(P_RSPEED), T(P_RGRAV), T(P_RLOSS)},
                                          {T(P_RSPREAD), T(P_RQUANT), T(P_RCOUNT), T(P_RWALLS)},
                                          {T(P_RJITTER), T(P_RGATE), N, N}};
static const struct slot v_settings[5][4] = {{{S_SET, 0, 0}, {S_SET, 1, 0}, {S_SET, 2, 0}, {S_SET, 3, 0}},
                                             {{S_SET, 0, 0}, T(P_INPUT), T(P_GAIN), G(G_INMON)},
                                             {G(G_CLOCK), {S_TEMPO, 0, 0}, G(G_SWING), G(G_INMON)},
                                             {{S_SET, 4, 0}, {S_SET, 5, 0}, {S_SET, 6, 0}, N},
                                             {N, N, N, N}};
#define SET_INPUT 1   /* SETTINGS views: 0 Bluetooth, 1 audio in, 2 clock, 3 panel, 4 MIDI learn */
#define SET_CLOCK 2
#define SET_PANEL 3
#define SET_LEARN 4
#define MATRIX_VIEW 12   /* MODS: views 0..11 are four modulators x three pages, then the matrix */
static const char *const mtype_name[4] = {"WAVE", "RANDOM", "FOLLOW", "ADSR"};

static unsigned track, page = PG_MIX, row, step_sel, step_page, octave, route_sel, learn_sel;
static unsigned mod_sel;      /* the modulator LFO+knob assigns: the one last viewed */
static uint32_t touch_at[4];  /* when each of KNOB 1-4 last turned: its cell shows the value */
static uint8_t mapping;       /* LFO held: the header takes the modulator's colour */
static uint32_t prev_buttons, prev_keys, rec_down_at;
static uint8_t temp_on, rec_long, live_rec, learn_seen;
static uint8_t temp_t[VSC_TRACKS][VSC_NP], temp_g[VSC_NG];
static const char *notice; static uint32_t notice_until, now_ms;
static char notice_buf[40];
static unsigned proj_slot, set_dev, sound_slot;
static uint8_t mod_hist[VSC_MODS][96]; static unsigned hist_w;   /* 0..255 */

void vsc_ui_notice(const char *t, uint32_t ms) { notice = t; notice_until = now_ms + ms; }

static unsigned row_count(void)
{
    switch (page) {
    case PG_MIX: return 6;
    case PG_MAT: { unsigned m = vsc.t[track][P_MODE]; return m == MODE_PLAY ? 2 : m == MODE_LOOP ? 3 : m == MODE_KEYS ? 4 : 1; }
    case PG_GRAIN: case PG_COLOR: case PG_SCENE: return 3;
    case PG_SETTINGS: return 5;
    case PG_FILTER: { unsigned f = vsc.t[track][P_FTYPE]; return f == FT_RING ? 4 : f == FT_PEAKS ? 2 : 1; }
    case PG_MODS: return MATRIX_VIEW + 1;
    case PG_SPACE: return 2;
    case PG_STEPS: return vsc.t[track][P_SEQM] == SEQ_CAROM ? 3 : 2;
    default: return 1;
    }
}
static struct slot slot_at(unsigned i)
{
    struct slot none = {S_NONE, 0, 0};
    unsigned r = row % row_count();
    if (i >= 4) return none;
    switch (page) {
    case PG_MIX: return v_mix[r][i];
    case PG_MAT: switch (vsc.t[track][P_MODE]) {
        case MODE_PLAY: return v_play[r][i];
        case MODE_LOOP: return v_loop[r][i];
        case MODE_KEYS: return v_keys[r][i];
        default: return v_thru[0][i];
        }
    case PG_GRAIN: return v_grain[r][i];
    case PG_FILTER: switch (vsc.t[track][P_FTYPE]) {
        case FT_RING: return v_reson[r][i];
        case FT_PEAKS: return v_peaks[r][i];
        case FT_OFF: return none;
        default: return v_svf[0][i];
        }
    case PG_COLOR: return v_color[r][i];
    case PG_SPACE: return v_space[r][i];
    case PG_SCENE: return v_scene[r][i];
    case PG_STEPS: return vsc.t[track][P_SEQM] == SEQ_CAROM ? v_carom[r][i] : v_steps[r][i];
    case PG_SETTINGS: return v_settings[r][i];
    case PG_MODS: {
        if (r == MATRIX_VIEW) { struct slot s = {S_MAT, (uint8_t)i, 0}; return s; }
        unsigned m = r / 3, pg = r % 3, k = pg < 2 ? 1 + pg * 4 + i : (i == 0 ? 9 : 99);
        if (k > 9) return none;
        if (!vsc_tparam_info(track, P_MOD(m, k))->name[0]) return none;
        struct slot s = {S_T, (uint8_t)P_MOD(m, k), 0}; return s;
    }
    default: return none;
    }
}
static const char *const step_names[6] = {"NOTE", "VELO", "LOCK", "VALUE", "LENGTH", "TRIG"};
static const char *const set_names[7] = {"BT AUD", "BT MIDI", "ROLE", "UNPAIR", "BACKLIT", "KNOB CC", "METRO"};
static const char *const backlight_names[4] = {"OFF", "LOW", "MID", "HIGH"};
static const char *const mat_names[4] = {"SOURCE", "DEST", "DEPTH", "POLAR"};
static void copy_n(char *d, const char *s, unsigned n) { unsigned i = 0; while (s[i] && i < n) { d[i] = s[i]; ++i; } d[i] = 0; }
static void slot_text(struct slot s, char name[10], char val[10], float *bar)
{
    *bar = -1.f; name[0] = val[0] = 0;
    if (s.kind == S_T || s.kind == S_TT) {
        unsigned t = s.kind == S_TT ? s.tk : track;
        const struct vsc_pinfo *in = vsc_tparam_info(t, s.id);
        unsigned v = vsc.t[t][s.id];
        copy_n(name, in->name, 7);
        if (s.kind == S_TT) { unsigned l = (unsigned)vsc_strlen(name); if (l > 6) l = 6; name[l] = ' '; name[l + 1] = (char)('1' + t); name[l + 2] = 0; }
        vsc_param_label(in, s.id, v, val, 0);
        if (!(in->flags & VSC_PF_ENUM)) *bar = (float)v / (float)in->max;
    } else if (s.kind == S_G) {
        const struct vsc_pinfo *in = vsc_gparam_info(s.id);
        unsigned v = vsc.g[s.id];
        copy_n(name, in->name, 7);
        vsc_param_label(in, s.id, v, val, 1);
        if (!(in->flags & VSC_PF_ENUM)) *bar = (float)v / (float)in->max;
    } else if (s.kind == S_TEMPO) {
        copy_n(name, "TEMPO", 7); num(val, vsc.tempo);
    } else if (s.kind == S_SET) {
        copy_n(name, set_names[s.id < 7 ? s.id : 0], 7);
        switch (s.id) {
        case 0: copy_n(val, vsc_host.bt_audio ? "ON" : "OFF", 7); break;
        case 1: copy_n(val, vsc_host.bt_midi ? "ON" : "OFF", 7); break;
        case 2: copy_n(val, vsc_host.ble_role ? "CENTRL" : "PERIPH", 7); break;
        case 4: copy_n(val, backlight_names[vsc_dev.backlight & 3], 7); break;
        case 5: copy_n(val, vsc_dev.knob_cc ? "110-3" : "OFF", 7); break;
        case 6: copy_n(val, vsc_dev.metro ? "ON" : "OFF", 7); break;
        default: copy_n(val, "TURN", 7); break;
        }
    } else if (s.kind == S_MAT) {
        const struct vsc_route *ro = &vsc.route[track][route_sel % VSC_MOD_ROUTES];
        copy_n(name, mat_names[s.id & 3], 7);
        int empty = ro->param == 255;
        switch (s.id) {
        case 0: val[0] = (char)('1' + (ro->src & 3)); val[1] = ' ';
                copy_n(val + 2, mtype_name[vsc.t[track][P_MOD(ro->src & 3, 0)] & 3], 6); break;
        case 1: if (empty) copy_n(val, "--", 3); else vsc_mod_dest_name(ro->param, val); break;
        case 2:
            if (empty) { copy_n(val, "--", 3); break; }
            num(val, ro->depth); *bar = ((float)ro->depth + 127.f) / 254.f; break;
        default: copy_n(val, ro->flags & 1 ? "BI" : "UNI", 4); break;
        }
    } else if (s.kind == S_STEP) {
        const struct vsc_step *st = &vsc.step[track][step_sel];
        copy_n(name, step_names[s.id], 7);
        switch (s.id) {
        case 0: num(val, st->note); break;
        case 1: num(val, st->vel); break;
        case 2: if (st->lp[0] < VSC_NP) copy_n(val, vsc_tparam_info(track, st->lp[0])->name, 7); else copy_n(val, "--", 3); break;
        case 3: if (st->lp[0] < VSC_NP) num(val, st->lv[0]); else copy_n(val, "--", 3); break;
        case 4: num(val, vsc.length[track] ? vsc.length[track] : 16); break;
        default: copy_n(val, st->on ? "ON" : "OFF", 4); break;
        }
    }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the code a route, macro or learn uses for a slot's parameter; 255 if it has none */
static unsigned slot_code(struct slot s, unsigned *t_out)
{
    *t_out = s.kind == S_TT ? s.tk : track;
    if (s.kind == S_T || s.kind == S_TT) {
        const struct vsc_pinfo *in = vsc_tparam_info(*t_out, s.id);
        return in->name[0] ? s.id : 255;
    }
    if (s.kind == S_G) return VSC_GDEST + s.id;
    return 255;
}
static void edit_slot(struct slot s, int d, unsigned *req, unsigned *arg)
{
    if (s.kind == S_T || s.kind == S_TT) {
        unsigned t = s.kind == S_TT ? s.tk : track;
        const struct vsc_pinfo *in = vsc_tparam_info(t, s.id);
        if (!in->name[0]) return;
        if (!(in->flags & VSC_PF_ENUM) && (d > 2 || d < -2)) d *= 2;
        vsc.t[t][s.id] = (uint8_t)clampi((int)vsc.t[t][s.id] + d, 0, in->max);
    } else if (s.kind == S_G) {
        const struct vsc_pinfo *in = vsc_gparam_info(s.id);
        vsc.g[s.id] = (uint8_t)clampi((int)vsc.g[s.id] + d, 0, in->max);
    } else if (s.kind == S_TEMPO) {
        vsc.tempo = (uint16_t)clampi((int)vsc.tempo + d, 30, 300);
    } else if (s.kind == S_SET) {
        if (s.id == 0) { *arg = !vsc_host.bt_audio; *req = HOST_BT_AUDIO; }
        else if (s.id == 1) { *arg = !vsc_host.bt_midi; *req = HOST_BT_MIDI; }
        else if (s.id == 2) { *arg = !vsc_host.ble_role; *req = HOST_BLE_ROLE; }
        else if (s.id == 3) *req = HOST_BLE_FORGET;
        else if (s.id == 4) { vsc_dev.backlight = (uint8_t)clampi((int)vsc_dev.backlight + d, 0, BACKLIGHT_HIGH); ++vsc_dev_changed; }
        else if (s.id == 5) { vsc_dev.knob_cc = d > 0; ++vsc_dev_changed; }
        else if (s.id == 6) { vsc_dev.metro = d > 0; ++vsc_dev_changed; }
    } else if (s.kind == S_MAT) {
        struct vsc_route *ro = &vsc.route[track][route_sel % VSC_MOD_ROUTES];
        if (s.id == 0) ro->src = (uint8_t)clampi((int)ro->src + d, 0, VSC_MODS - 1);
        else if (s.id == 1) {
            unsigned n = vsc_mod_dest_count(), at = 0;
            if (ro->param != 255) for (unsigned i = 0; i < n; ++i) if (vsc_mod_dest(i) == ro->param) { at = i + 1; break; }
            at = (unsigned)clampi((int)at + d, 0, (int)n);
            if (!at) { ro->param = 255; ro->depth = 0; }
            else { ro->param = (uint8_t)vsc_mod_dest(at - 1); if (!ro->depth) ro->depth = 32; }
        } else if (s.id == 2) { if (ro->param != 255) { int v = clampi(ro->depth + d * 2, -127, 127); ro->depth = (int8_t)(v ? v : (d > 0 ? 1 : -1)); } }
        else ro->flags ^= 1;
    } else if (s.kind == S_STEP) {
        struct vsc_step *st = &vsc.step[track][step_sel];
        switch (s.id) {
        case 0: st->note = (uint8_t)clampi(st->note + d, 0, 127); break;
        case 1: st->vel = (uint8_t)clampi(st->vel + d, 1, 127); break;
        case 2: { int p = st->lp[0] >= VSC_NP ? -1 : st->lp[0]; p = clampi(p + d, -1, VSC_NP - 1);
                  while (p >= 0 && p < VSC_NP && !vsc_tparam_info(track, (unsigned)p)->name[0]) p += d > 0 ? 1 : -1;
                  st->lp[0] = p < 0 || p >= VSC_NP ? 255 : (uint8_t)p;
                  if (st->lp[0] < VSC_NP) st->lv[0] = vsc.t[track][st->lp[0]]; break; }
        case 3: if (st->lp[0] < VSC_NP) st->lv[0] = (uint8_t)clampi(st->lv[0] + d, 0, vsc_tparam_info(track, st->lp[0])->max); break;
        case 4: { int l = vsc.length[track] ? vsc.length[track] : 16; vsc.length[track] = (uint8_t)clampi(l + d, 1, VSC_STEPS); break; }
        default: st->on = d > 0; break;
        }
    }
}
static void say(const char *a, const char *b, uint32_t ms)
{
    unsigned i = 0;
    for (; a && *a && i < 38; ++a) notice_buf[i++] = *a;
    for (; b && *b && i < 38; ++b) notice_buf[i++] = *b;
    notice_buf[i] = 0;
    vsc_ui_notice(notice_buf, ms);
}

/* ------------------------------------------------------------- input */
static void apply_knob_cc(unsigned *req, unsigned *arg);
void vsc_ui_init(void) { notice = "VISCERA " VSC_VERSION; notice_until = 1500; }

/* store the track's sound in the chosen slot; the FM-1 writes it to flash at once.
 * Like projects, only while stopped (a flash write stalls the sound). */
static unsigned store_sound(void)
{
    if (vsc_meter.playing) { vsc_ui_notice("STOP PLAYBACK TO SAVE", 1200); return HOST_NONE; }
    vsc_sound_store(sound_slot, track);
    vsc_ui_notice("SOUND SAVED", 900);
    return HOST_DEVICE_SAVE;
}
#define B(n) (1u << (n))
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg)
{
    unsigned req = HOST_NONE;
    now_ms = in->now_ms;
    uint32_t down = in->buttons & ~prev_buttons, up = ~in->buttons & prev_buttons;
    uint32_t kdown = in->keys & ~prev_keys, kup = ~in->keys & prev_keys;
    int lfo_held = (in->buttons & B(BTN_LFO)) != 0, arp_held = (in->buttons & B(BTN_ARP)) != 0;
    int home_held = (in->buttons & B(BTN_HOME)) != 0, save_held = (in->buttons & B(BTN_SAVE)) != 0;
    if (vsc_learn_seen != learn_seen) { learn_seen = vsc_learn_seen; vsc_ui_notice("LEARNED", 900); }

    /* TEMP: both octave buttons held; everything snaps back on release */
    int both = (in->buttons & (B(BTN_OCTD) | B(BTN_OCTU))) == (B(BTN_OCTD) | B(BTN_OCTU));
    if (both && !temp_on) { temp_on = 1; vsc_memcpy(temp_t, vsc.t, sizeof temp_t); vsc_memcpy(temp_g, vsc.g, sizeof temp_g); vsc_ui_notice("TEMP", 600); }
    else if (!both && temp_on && !(in->buttons & (B(BTN_OCTD) | B(BTN_OCTU)))) {
        temp_on = 0; vsc_memcpy(vsc.t, temp_t, sizeof temp_t); vsc_memcpy(vsc.g, temp_g, sizeof temp_g);
    }
    if (!temp_on) {
        /* on the steps view the octave buttons page through the 64 steps */
        int steps_view = page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_STEPS;
        if ((up & B(BTN_OCTD)) && !both && !(prev_buttons & B(BTN_OCTU))) {
            if (steps_view) { if (step_page > 0) --step_page; } else if (octave > 0) --octave;
        }
        if ((up & B(BTN_OCTU)) && !both && !(prev_buttons & B(BTN_OCTD))) {
            if (steps_view) { if (step_page < 3) ++step_page; } else if (octave < 4) ++octave;
        }
    }

    /* bit 31 of prev_buttons marks "a held button was used as a modifier" */
    uint32_t used = prev_buttons & B(31);
    if (!(in->buttons & (B(BTN_LFO) | B(BTN_ARP) | B(BTN_SAVE) | B(BTN_HOME)))) used = 0;
    /* menu buttons; pressing the button of the open menu goes to its next view */
    struct { unsigned btn, pg; } map[] = {{BTN_EDIT, PG_MAT}, {BTN_FX, PG_GRAIN}, {BTN_SCL, PG_FILTER},
        {BTN_ENV, PG_COLOR}, {BTN_SEQ, PG_STEPS}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (down & B(map[i].btn)) { if (page != map[i].pg) { page = map[i].pg; row = 0; } else row = (row + 1) % row_count(); }
    if ((up & B(BTN_HOME)) && !(prev_buttons & B(31))) { if (page != PG_MIX) { page = PG_MIX; row = 0; } else row = (row + 1) % row_count(); }
    if ((up & B(BTN_LFO)) && !(prev_buttons & B(31))) {
        /* LFO: the modulators, one press per modulator, then the matrix */
        if (page != PG_MODS) { page = PG_MODS; row = mod_sel * 3; }
        else { row = row >= MATRIX_VIEW ? 0 : (row / 3 + 1) * 3; if (row > MATRIX_VIEW) row = MATRIX_VIEW; }
    }
    if ((up & B(BTN_ARP)) && !(prev_buttons & B(31))) { if (page != PG_SCENE) { page = PG_SCENE; row = 0; } else row = (row + 1) % row_count(); }
    /* GLO: the global things, SPACE (the shared bus) then SETTINGS (the FM-1's own) */
    if (down & B(BTN_GLO)) { page = page == PG_SPACE ? PG_SETTINGS : PG_SPACE; row = 0; }
    /* SAVE: where things are kept, PROJECT then SOUNDS */
    if ((up & B(BTN_SAVE)) && !(prev_buttons & B(31))) { page = page == PG_PROJECT ? PG_SOUND : PG_PROJECT; row = 0; }
    if (page == PG_MODS && row < MATRIX_VIEW) mod_sel = row / 3;

    /* transport */
    /* SAVE + PLAY saves on PROJECT and SOUNDS; it mustn't start the transport too */
    if ((down & B(BTN_PLAY)) && !(save_held && (page == PG_PROJECT || page == PG_SOUND))) vsc_event(EV_PLAY, track, 0, 0);
    /* REC: the open page's action; held, its clear */
    int adsr_page = page == PG_MODS && row < MATRIX_VIEW && (vsc.t[track][P_MOD(row / 3, 0)] & 3) == MT_ADSR;
    if (down & B(BTN_REC)) {
        rec_down_at = now_ms; rec_long = 0;
        if (adsr_page) { vsc_event(EV_ACTION, track, ACT_MOD_TRIG, row / 3); rec_long = 2; }
    }
    if ((up & B(BTN_REC)) && rec_long == 2) vsc_event(EV_ACTION, track, ACT_MOD_TRIG, 0x80 | (row / 3));
    if ((in->buttons & B(BTN_REC)) && !rec_long && now_ms - rec_down_at > 900) {
        if (page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_CAROM) {
            rec_long = 1; vsc_event(EV_BALL_CLEAR, track, 0, 0); vsc_ui_notice("BALLS CLEARED", 900);
        } else if (page == PG_SPACE) {
            rec_long = 1; vsc_event(EV_ACTION, 0, ACT_SPACE_CLEAR, 0); vsc_ui_notice("SPACE CLEARED", 900);
        } else if (vsc.t[track][P_MODE] == MODE_LOOP && (page == PG_MAT || page == PG_MIX)) {
            rec_long = 1; vsc_event(EV_LOOP_CLEAR, track, 0, 0); vsc_ui_notice("LOOP CLEARED", 900);
        }
    }
    if ((up & B(BTN_REC)) && !rec_long) {
        if (page == PG_STEPS) {
            live_rec ^= 1;
            if (vsc.t[track][P_SEQM] == SEQ_CAROM) vsc_ui_notice(live_rec ? "PLAY NOTES TO ADD BALLS" : "BALL RECORD OFF", 1200);
            else vsc_ui_notice(live_rec ? "STEP RECORD ON" : "STEP RECORD OFF", 900);
        }
        else if (page == PG_PROJECT) { *arg = proj_slot; req = vsc_host.slot_used[proj_slot] ? HOST_LOAD : HOST_SAVE; }
        else if (page == PG_SOUND) {
            if (vsc_dev.sound[sound_slot].used) {
                vsc_sound_recall(sound_slot, track);
                say("SOUND ON TRACK ", (const char[]){(char)('1' + track), 0}, 1000);
            } else req = store_sound();
        }
        else if (page == PG_SETTINGS && row == 0) { if (vsc_host.ble_count) { *arg = set_dev; req = HOST_BLE_CONNECT; } else req = HOST_BLE_SCAN; }
        else if (page == PG_SETTINGS && row == SET_INPUT) {
            unsigned m = vsc.t[track][P_MODE];
            page = PG_MAT; row = m == MODE_LOOP ? 1 : 0;
        }
        else if (page == PG_SETTINGS && row == SET_LEARN) { vsc_dev.learn[learn_sel % VSC_LEARN].param = 255; ++vsc_dev_changed; vsc_ui_notice("MAPPING CLEARED", 800); }
        else if (page == PG_GRAIN) { vsc.t[track][P_GFREEZE] ^= 1; vsc_ui_notice(vsc.t[track][P_GFREEZE] ? "GRAINS LOCKED" : "GRAINS LISTENING", 900); }
        else if (page == PG_SPACE) { vsc_event(EV_ACTION, 0, ACT_SPACE_FREEZE, 0); vsc_ui_notice(vsc_meter.space_frozen ? "SPACE FLOWING" : "SPACE FROZEN", 900); }
        else if (page == PG_MAT && vsc.t[track][P_MODE] == MODE_PLAY) vsc_event(EV_ACTION, track, ACT_TAPE_REWIND, 0);
        else if (vsc.t[track][P_MODE] == MODE_LOOP) vsc_event(EV_REC, track, 0, 0);
        else vsc_ui_notice("REC: LOOP TRACKS ONLY", 900);
    }
    if (page == PG_PROJECT && save_held && (down & B(BTN_PLAY))) { *arg = proj_slot; req = HOST_SAVE; used = B(31); }
    if (page == PG_SOUND && save_held && (down & B(BTN_PLAY))) { req = store_sound(); used = B(31); }

    /* the left-hand encoders: SELECT 0, ALGORITHM 1, PRESETS 6 */
    if (in->enc[0]) {
        int d = in->enc[0];
        if (page == PG_PROJECT && d > 0) { page = PG_SOUND; row = 0; }
        else if (page == PG_SOUND && d < 0) page = PG_PROJECT;
        else row = (unsigned)clampi((int)row + d, 0, (int)row_count() - 1);
    }
    if (in->enc[1]) track = (unsigned)clampi((int)track + in->enc[1], 0, VSC_TRACKS - 1);
    if (in->enc[6]) {
        int d = in->enc[6];
        if (lfo_held) { mod_sel = (unsigned)clampi((int)mod_sel + d, 0, VSC_MODS - 1); used = B(31);
                        say("ASSIGNING MOD ", (const char[]){(char)('1' + mod_sel), 0}, 900); }
        else if (arp_held) { vsc.g[G_MSEL] = (uint8_t)clampi((int)vsc.g[G_MSEL] + d, 0, VSC_MACROS - 1); used = B(31);
                             say("MACRO ", (const char[]){(char)('1' + vsc.g[G_MSEL]), 0}, 900); }
        else switch (page) {
        case PG_MAT: vsc.t[track][P_MODE] = (uint8_t)clampi((int)vsc.t[track][P_MODE] + d, 0, MODE_THRU); row = 0; break;
        case PG_FILTER: vsc.t[track][P_FTYPE] = (uint8_t)clampi((int)vsc.t[track][P_FTYPE] + d, 0, FT_OFF); row = 0; break;
        case PG_MODS:
            if (row >= MATRIX_VIEW) route_sel = (unsigned)clampi((int)route_sel + d, 0, VSC_MOD_ROUTES - 1);
            else {
                unsigned m = row / 3, ty = (unsigned)clampi((int)(vsc.t[track][P_MOD(m, 0)] & 3) + d, 0, 3);
                if (ty != (vsc.t[track][P_MOD(m, 0)] & 3u)) { vsc.t[track][P_MOD(m, 0)] = (uint8_t)ty; vsc_mod_defaults(track, m); }
            }
            break;
        case PG_STEPS: {   /* the sequencer itself: STEPS or CAROM */
            unsigned m = (unsigned)clampi((int)vsc.t[track][P_SEQM] + d, SEQ_STEPS, SEQ_CAROM);
            if (m != vsc.t[track][P_SEQM]) { vsc.t[track][P_SEQM] = (uint8_t)m; row = 0; vsc_ui_notice(m == SEQ_CAROM ? "CAROM: NOTES BOUNCE" : "STEPS", 900); }
            break;
        }
        case PG_PROJECT: proj_slot = (unsigned)clampi((int)proj_slot + d, 0, VSC_PROJECTS - 1); break;
        case PG_SOUND: sound_slot = (unsigned)clampi((int)sound_slot + d, 0, VSC_SOUNDS - 1); break;
        case PG_SETTINGS:
            if (row == SET_INPUT) track = (unsigned)clampi((int)track + d, 0, VSC_TRACKS - 1);
            else if (row == SET_LEARN) learn_sel = (unsigned)clampi((int)learn_sel + d, 0, VSC_LEARN - 1);
            else set_dev = (unsigned)clampi((int)set_dev + d, 0, vsc_host.ble_count ? vsc_host.ble_count - 1 : 0);
            break;
        case PG_SCENE: vsc.g[G_TARGET] = (uint8_t)clampi((int)vsc.g[G_TARGET] + d, 0, VSC_SCENES - 1); break;
        default: track = (unsigned)clampi((int)track + d, 0, VSC_TRACKS - 1); break;
        }
    }
    /* KNOB 1-4: the view's four parameters; held LFO maps the modulator, held ARP
     * the macro, held SAVE arms MIDI learn */
    for (unsigned e = 2; e < 6; ++e) {
        int d = in->enc[e];
        if (!d) continue;
        struct slot s = slot_at(e - 2);
        unsigned t, code = slot_code(s, &t);
        int cont = code != 255 && (code >= VSC_GDEST ? !(vsc_gparam_info(code - VSC_GDEST)->flags & VSC_PF_ENUM)
                                                    : !(vsc_tparam_info(t, code)->flags & VSC_PF_ENUM));
        if (lfo_held && cont && page != PG_MODS) {
            int depth = vsc_route_get(t, mod_sel, code) + d * 3;
            vsc_route_set(t, mod_sel, code, depth);
            char b[12]; num(b, clampi(depth, -127, 127));
            say("MOD AMOUNT ", b, 900);
            used = B(31);
        } else if (arp_held && cont && code < VSC_NP) {
            unsigned m = vsc.g[G_MSEL];
            vsc_macro_set(m, t, code, vsc_macro_get(m, t, code) + d * 3);
            used = B(31);
        } else if (save_held && code != 255) {
            vsc_learn_track = (uint8_t)t; vsc_learn_param = (uint8_t)code; vsc_learn_armed = 1;
            vsc_ui_notice("LEARN: SEND A CC", 3000);
            used = B(31);
        } else edit_slot(s, d, &req, arg);
        touch_at[e - 2] = now_ms | 1u;
    }
    mapping = (uint8_t)(lfo_held ? 1 + mod_sel : 0);

    /* keys */
    for (unsigned k = 0; k < 27; ++k) {
        unsigned note = 53 + k + octave * 12 - 24;
        if (kdown & B(k)) {
            if (home_held && k >= 23) { track = k - 23; used = B(31); continue; }
            if (arp_held && k < VSC_SCENES) {
                if (save_held) { vsc_scene_store(k); vsc_ui_notice("SCENE STORED", 800); }
                else vsc_scene_recall(k);
                used = B(31); continue;
            }
            int carom = vsc.t[track][P_SEQM] == SEQ_CAROM;
            if (carom && live_rec) vsc_event(EV_BALL, track, note, 100);
            if (page == PG_STEPS && k < 16 && !carom) {
                unsigned s = step_page * 16 + k;
                step_sel = s;
                if (!live_rec) vsc.step[track][s].on ^= 1;
                continue;
            }
            vsc_event(EV_NOTE_ON, track, note, 100);
            if (live_rec && vsc_meter.playing && !carom) {
                struct vsc_step *st = &vsc.step[track][vsc_meter.step[track]];
                st->on = 1; st->note = (uint8_t)note; st->vel = 100;
            }
        }
        if (kup & B(k)) {
            if (page == PG_STEPS && k < 16 && vsc.t[track][P_SEQM] == SEQ_STEPS) continue;
            vsc_event(EV_NOTE_OFF, track, note, 0);
        }
    }
    apply_knob_cc(&req, arg);
    prev_buttons = (in->buttons & 0x7fffffffu) | used;
    prev_keys = in->keys;
    return req;
}

/* ------------------------------------------------------------- drawing */
#define VIS_Y 20
#define VIS_H 140
#define KNOB_Y 178
static void frame_box(void) { }

static const char *page_title(void)
{
    static char buf[16];
    switch (page) {
    case PG_MAT: { static const char *const m[] = {"PLAY", "LOOP", "KEYS", "THRU"}; return m[vsc.t[track][P_MODE] & 3]; }
    case PG_FILTER: return vsc.t[track][P_FTYPE] == FT_RING ? "RESON" : vsc.t[track][P_FTYPE] == FT_OFF ? "FILTER OFF" : "FILTER";
    case PG_MODS:
        if (row >= MATRIX_VIEW) return "MATRIX";
        { unsigned m = row / 3; copy_n(buf, mtype_name[vsc.t[track][P_MOD(m, 0)] & 3], 8);
          unsigned l = (unsigned)vsc_strlen(buf); buf[l] = ' '; buf[l + 1] = (char)('1' + m); buf[l + 2] = 0; return buf; }
    case PG_STEPS: return vsc.t[track][P_SEQM] == SEQ_CAROM ? "CAROM" : "STEPS";
    default: return page_name[page];
    }
}
static void draw_header(void)
{
    char b[24];
    /* the track in an inverted box, then the page; while LFO is held both take the modulator's colour */
    uint16_t box = mapping ? C_MOD[(mapping - 1) & 3] : C_FG;
    b[0] = (char)('1' + track); b[1] = 0;
    rect(3, 3, 11, 11, box); text(6, 5, b, C_BG, 1);
    const char *title = page_title();
    int tw = text_w(title, 1);
    if (mapping) { rect(16, 3, tw + 4, 11, box); text(18, 5, title, C_BG, 1); }
    else text(18, 5, title, C_FG, 1);
    int tx = 18 + tw + 6;
    unsigned views = row_count();
    if (page == PG_MODS) {
        if (row < MATRIX_VIEW) for (unsigned r = 0; r < 3; ++r) rect(tx + (int)r * 5, 8, 3, 3, r == row % 3 ? C_FG : C_LINE);
    } else if (views > 1) {
        for (unsigned r = 0; r < views; ++r) rect(tx + (int)r * 5, 8, 3, 3, r == row ? C_FG : C_LINE);
    }
    /* tempo after a metronome, then two small input squares (Bluetooth, USB) */
    int t10 = (int)(vsc_meter.tempo * 10.f + 0.5f);
    num(b, t10 / 10); int l = (int)vsc_strlen(b); b[l] = '.'; b[l + 1] = (char)('0' + t10 % 10); b[l + 2] = 0;
    int x = VSC_SCREEN_W - 14 - text_w(b, 1);
    uint16_t tc = vsc_meter.ext_clock && vsc.g[G_CLOCK] != CLOCK_INT ? C_WARN : C_FG;
    text(x, 5, b, tc, 1);
    for (int i = 0; i < 7; ++i) { px(x - 9 + 3 - i / 2, 5 + i, tc); px(x - 9 + 3 + i / 2, 5 + i, tc); }
    hline(x - 10, 12, 9, tc);
    { int sw = vsc_meter.playing ? ((now_ms / 300) & 1 ? 2 : -2) : 0; for (int i = 0; i < 5; ++i) px(x - 6 + sw * i / 5, 11 - i, tc); }
    rect(VSC_SCREEN_W - 10, 4, 3, 5, vsc_meter.bt_in ? C_FG : C_LINE);
    rect(VSC_SCREEN_W - 6, 4, 3, 9, vsc_meter.usb_in ? C_FG : C_LINE);
    if (temp_on) { rect(tx + 20, 3, 28, 11, C_WARN); text(tx + 22, 5, "TEMP", C_BG, 1); }
}
static void draw_knobs(void)
{
    char name[10], val[10];
    for (unsigned i = 0; i < 4; ++i) {
        int x = (int)i * 60 + 1, y = KNOB_Y;
        rect(x, y, 58, 30, C_PANEL);
        struct slot s = slot_at(i);
        float bar;
        slot_text(s, name, val, &bar);
        if (!name[0]) continue;
        unsigned t = track, code = 255;
        int mod = -1;
        if (s.kind == S_T || s.kind == S_TT || s.kind == S_G) {
            code = slot_code(s, &t);
            if (code != 255) for (unsigned m = 0; m < VSC_MODS && mod < 0; ++m) if (vsc_route_get(t, m, code)) mod = (int)m;
        }
        int locked = s.kind == S_T && vsc_lock[track][s.id] != 255;
        int touched = touch_at[i] && (int32_t)(now_ms - touch_at[i]) < 900;
        /* the label; while the knob turns, its value in the label's place */
        text_n(x + 3, y + 3, touched ? val : name, touched ? C_INK : C_MID, 9);
        if (bar >= 0.f) {
            int bipolar = 0;
            if (s.kind == S_T || s.kind == S_TT) bipolar = (vsc_tparam_info(t, s.id)->flags & VSC_PF_BIPOLAR) != 0;
            else if (s.kind == S_G) bipolar = (vsc_gparam_info(s.id)->flags & VSC_PF_BIPOLAR) != 0;
            else if (s.kind == S_MAT) bipolar = 1;
            int bx = x + 3, bw = 52, by = y + 16;
            rect(bx, by, bw, 5, C_LINE);
            uint16_t fill = locked ? C_WARN : C_FG;
            int at = (int)((float)bw * bar + 0.5f);
            if (bipolar) {
                int c = bx + bw / 2;
                if (bx + at >= c) rect(c, by + 1, bx + at - c + 1, 3, fill); else rect(bx + at, by + 1, c - bx - at, 3, fill);
                vline(c, by - 2, 9, C_INK); hline(c - 1, by - 2, 3, C_INK);   /* the centre tick */
            } else rect(bx, by, at, 5, fill);
            /* a modulated parameter shows where its modulator has it now */
            if (mod >= 0 && code < VSC_NP) {
                float v = bar + (float)vsc_route_get(t, (unsigned)mod, code) / 127.f * vsc_meter.mod_value[t][mod];
                v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
                rect(bx + (int)((float)(bw - 2) * v), by - 1, 2, 7, C_MOD[mod]);
            }
        } else if (!touched) {
            text_n(x + 3, y + 16, val, C_FG, 9);
        }
        if (mod >= 0) rect(x + 52, y + 3, 4, 4, C_MOD[mod]);    /* the mapped marker, top right */
    }
    /* key and button hints along the bottom */
    const char *hint;
    switch (page) {
    case PG_MAT: hint = vsc.t[track][P_MODE] == MODE_LOOP ? "PRESETS: DEVICE  REC: REC/DUB  HOLD: CLR"
                      : vsc.t[track][P_MODE] == MODE_PLAY ? "PRESETS: DEVICE  REC: RESTART" : "PRESETS: DEVICE  SELECT: PAGE"; break;
    case PG_GRAIN: hint = "REC: LOCK BUFFER  SELECT: PAGE"; break;
    case PG_FILTER: hint = "PRESETS: DEVICE  SELECT: PAGE"; break;
    case PG_SPACE: hint = "REC: FREEZE  HOLD REC: CLEAR"; break;
    case PG_MODS: hint = row >= MATRIX_VIEW ? "PRESETS: ROUTE  SELECT: PAGE"
                       : (vsc.t[track][P_MOD(row / 3, 0)] & 3) == MT_ADSR ? "PRESETS: TYPE  REC: TRIG  LFO+KNOB: MAP"
                       : "PRESETS: TYPE  HOLD LFO+KNOB: MAP"; break;
    case PG_SCENE: hint = "ARP+PAD: SCENE  ARP+KNOB: MAP MACRO"; break;
    case PG_STEPS: hint = vsc.t[track][P_SEQM] == SEQ_CAROM ? "PRESETS: STEPS  REC: ADD  HOLD: CLEAR" : "PRESETS: CAROM  OCT: PAGE  REC: REC"; break;
    case PG_PROJECT: hint = "PRESETS: SLOT REC: LOAD SAVE+PLAY: SAVE"; break;
    case PG_SOUND: hint = "PRESETS: SLOT REC: LOAD SAVE+PLAY: SAVE"; break;
    case PG_SETTINGS: hint = row == 0 ? "PRESETS: DEVICE  REC: SCAN/CONNECT" : row == SET_LEARN ? "SAVE+KNOB: LEARN  REC: CLEAR"
                                  : row == SET_INPUT ? "PRESETS: TRACK  REC: OPEN TRACK"
                                  : row == SET_PANEL ? "KEPT THROUGH POWER-OFF" : "SELECT: PAGE"; break;
    default: hint = "SELECT: PAGE  ALGORITHM: TRACK"; break;
    }
    int live = notice && (int32_t)(notice_until - now_ms) > 0;
    if (live) hint = notice;
    rect(1, 211, 238, 26, live ? C_LINE : C_PANEL);
    text_n(5, 221, hint, live ? C_INK : C_MID, 38);
}

/* visualisations, computed once per frame and painted band by band */
static uint8_t wave[VSC_SCREEN_W];      /* 0..60 amplitude per column */
static float curve[VSC_SCREEN_W];       /* 0..1 per column */
static int adsr_node[4][2];             /* the ADSR picture's corners: x, level 0..254 */
static void draw_vis(void)
{
    unsigned t = track;
    switch (page) {
    case PG_MAT: {
        /* the sample's name, its waveform mirrored, the played window bright, the playhead */
        const char *st = vsc.t[t][P_MODE] == MODE_LOOP
            ? (const char *const[]){"EMPTY: PRESS REC", "RECORDING", "LOOP", "OVERDUB"}[vsc_meter.loop_state[t] & 3]
            : vsc.t[t][P_MODE] == MODE_THRU ? "LIVE INPUT" : vsc.t[t][P_SLOT] < VSC_BANK_SLOTS
            ? (vsc_bank_frames(vsc.t[t][P_SLOT]) ? vsc_bank_name(vsc.t[t][P_SLOT]) : "EMPTY SLOT") : "LOOP";
        text_n(4, VIS_Y + 2, st, C_MID, 39);
        uint32_t len = vsc_meter.pos_len[t];
        float s0 = (float)vsc.t[t][P_START] / 127.f, l0 = (float)vsc.t[t][P_LEN] / 127.f;
        int a = (int)(s0 * 239.f), bnd = a + (int)(l0 * (239.f - (float)a));
        int ph = len ? (int)((float)vsc_meter.pos_play[t] / (float)len * 239.f) : -1;
        int mid = VIS_Y + 66;
        for (int xx = 0; xx < VSC_SCREEN_W; ++xx) {
            int h = wave[xx] * 52 / 60;
            int in = xx >= a && xx <= bnd;
            uint16_t c = in ? (xx <= ph ? C_INK : C_FG) : C_LINE;
            if (h) vline(xx, mid - h, h * 2 + 1, c); else px(xx, mid, C_LINE);
            /* the overview below: the whole source, small */
            int h2 = wave[xx] * 10 / 60;
            vline(xx, VIS_Y + 140 - h2, h2 + 1, in ? C_MID : C_LINE);
        }
        if (ph >= 0) { vline(ph, VIS_Y + 12, 110, C_INK); vline(ph + 1, VIS_Y + 12, 110, C_DIM); }
        for (int xx = 0; xx < VSC_SCREEN_W; xx += 15) px(xx, VIS_Y + 126, C_DIM);
        vline(a, VIS_Y + 128, 14, C_FG); vline(bnd, VIS_Y + 128, 14, C_FG);
        break;
    }
    case PG_GRAIN: {
        /* our own: the captured sound lies as a dust horizon; each grain lifts out of it
         * and rises as it plays (reversed grains sink), fading as it goes */
        int horizon = VIS_Y + 108;
        for (int xx = 0; xx < VSC_SCREEN_W; ++xx) {
            int h = wave[xx] * 18 / 60;
            for (int yy = 0; yy <= h; yy += 2) px(xx, horizon - yy + ((xx * 7 + yy) & 1), (yy & 2) ? C_DIM : C_MID);
            if ((xx & 3) == 0) px(xx, horizon + 3, C_LINE);
        }
        int w = (int)((float)vsc_meter.grain_write[t] / (float)VSC_GRAIN_FRAMES * 239.f);
        for (int yy = VIS_Y + 4; yy < horizon + 6; yy += 3) px(w, yy, vsc.t[t][P_GFREEZE] ? C_DIM : C_WARN);
        for (unsigned g = 0; g < VSC_GRAINS; ++g) {
            if (vsc_meter.grain_pos[t][g] == 0xffff) continue;
            int gx = (int)vsc_meter.grain_pos[t][g] * 239 / 65535;
            int age = vsc_meter.grain_amp[t][g];                          /* 0..255 through the grain */
            int rev = vsc.t[t][P_GREV] > 64 && (g & 1);
            int gy = rev ? horizon + 4 + age / 10 : horizon - 6 - age * 90 / 255;
            uint16_t c = age < 128 ? C_INK : age < 200 ? C_FG : C_MID;
            rect(gx - 1, gy - 1, 3, 3, c);
            for (int k = 1; k < 5; ++k) px(gx + ((k * 13 + (int)g) % 5) - 2, gy + (rev ? -k * 3 : k * 3), C_DIM);
        }
        if (vsc.t[t][P_GFREEZE]) text(4, VIS_Y + 4, "LOCKED", C_WARN, 1);
        text_n(4, VIS_Y + 134, vsc.t[t][P_GWMODE] ? "SEND" : "", C_DIM, 8);
        break;
    }
    case PG_FILTER: {
        /* dense bars, as on the S-4: the bank's shape in grey, the bands that ring lit */
        int base = VIS_Y + 140, h = 120, ring = vsc.t[t][P_FTYPE] == FT_RING;
        for (int i = 0; i < 47; ++i) {
            int xx = 4 + i * 5;
            int env = (int)(curve[xx] * (float)h);
            int lit = 0;
            if (ring)
                for (unsigned bb = 0; bb < VSC_RING_BANDS; ++bb) {
                    int d = xx - vsc_meter.ring_x[t][bb]; if (d < 0) d = -d;
                    if (d < 12) { int v = vsc_meter.ring_amp[t][bb] * (12 - d) / 12; if (v > lit) lit = v; }
                }
            int lh = lit * h / 255;
            if (env) rect(xx, base - env, 2, env, C_LINE);
            if (lh) { rect(xx, base - lh, 2, lh, C_MID); rect(xx, base - lh, 2, 2, C_INK); }
            else if (!ring && env) rect(xx, base - env, 2, 2, C_FG);
        }
        if (vsc.t[t][P_FTYPE] == FT_OFF) text(92, VIS_Y + 60, "BYPASS", C_DIM, 1);
        break;
    }
    case PG_COLOR: {
        /* our own: the sound as a body of matter that COLOR wears away. DRIVE tears its
         * edge, CRUSH coarsens it into blocks (REDUX shears its rows), COMP squashes it,
         * NOISE throws dust off it, TILT leans it. It breathes with the track's level. */
        float drive = (float)vsc.t[t][P_DRIVE] / 127.f, crush = (float)vsc.t[t][P_CRUSH] / 127.f;
        float comp = (float)vsc.t[t][P_COMP] / 127.f, noise = (float)vsc.t[t][P_NOISE] / 127.f;
        float tilt = (float)vsc.t[t][P_TILT] / 127.f * 2.f - 1.f, lvl = vsc_meter.peak[t];
        int step = 2 + (int)(crush * 9.f);
        int cx = 120, cy = VIS_Y + 72;
        float R = 46.f + lvl * 22.f, sx = 1.f + comp * 0.35f, sy = 1.f - comp * 0.45f;
        unsigned frame = now_ms / 70;
        int redux = vsc.t[t][P_CMODE] != 0 && crush > 0.02f;
        for (int yy = VIS_Y + 4; yy < VIS_Y + 142; yy += step) {
            if (yy + step < by0 || yy >= by0 + VSC_BAND) continue;
            int shear = redux ? (int)((hash3(yy / step, 7, frame / 2) & 15) - 8) * (int)(crush * 3.f) : 0;
            for (int xx = 6; xx < 234; xx += step) {
                float dx = ((float)(xx - cx) - tilt * (float)(cy - yy) * 0.5f) / sx, dy = (float)(yy - cy) / sy;
                float d2 = dx * dx + dy * dy;
                float j = 1.f + drive * 0.55f * ((float)(hash3(xx / step, yy / step, frame) & 255) / 128.f - 1.f);
                float r = R * j;
                if (d2 < r * r) {
                    float edge = d2 / (r * r);
                    uint16_t c = edge > 0.8f ? C_DIM : edge > 0.5f ? C_MID : ((xx / step + yy / step) & 1 ? C_FG : C_INK);
                    rect(xx + shear, yy, step > 2 ? step - 1 : 1, step > 2 ? step - 1 : 1, c);
                } else if (noise > 0.01f && (hash3(xx, yy, frame) & 1023) < (unsigned)(noise * noise * 60.f * (0.3f + lvl * 2.f)) && d2 < R * R * 4.f) {
                    px(xx + shear, yy, C_MID);
                }
            }
        }
        break;
    }
    case PG_SPACE: {
        /* our own: echoes leave as rings of grit that widen and thin out (TIME spaces them,
         * FDBK keeps more of them); the reverb is a drifting cloud whose spread is SIZE */
        float fb = (float)vsc.g[G_DFEED] / 127.f, dl = (float)vsc.g[G_DLEVEL] / 127.f;
        float rs = (float)vsc.g[G_RSIZE] / 127.f, rl = (float)vsc.g[G_RLEVEL] / 127.f;
        int frozen = vsc_meter.space_frozen;
        unsigned per = 200u + (unsigned)vsc.g[G_DTIME] * 120u;
        unsigned age = frozen ? per / 2 : now_ms % per;
        int cx = 66, cy = VIS_Y + 72;
        float a = 1.f;
        for (int k = 0; k < 7 && a > 0.06f; ++k) {
            int r = 8 + (int)(age * 14 / per) + k * 14;
            if (r > 64) break;
            uint16_t c = a * dl > 0.5f ? C_FG : a * dl > 0.2f ? C_MID : C_DIM;
            for (int yy = cy - r; yy <= cy + r; ++yy) {
                if (yy < by0 || yy >= by0 + VSC_BAND) continue;
                int dy = yy - cy, w2 = r * r - dy * dy, xx = 0;
                while ((xx + 1) * (xx + 1) <= w2) ++xx;     /* integer square root, small */
                if ((hash3(yy, k, 3) & 3) == 0) continue;     /* grit: rings are broken */
                px(cx - xx, yy, c); px(cx + xx, yy, c);
            }
            a *= fb;
        }
        unsigned f = frozen ? 0u : now_ms / 90;
        int n = 40 + (int)(rl * 220.f), spread = 14 + (int)(rs * 44.f);
        for (int i = 0; i < n; ++i) {
            int dx = (int)(hash3(i, 1, f / 6) % (unsigned)(spread * 2)) - spread, dy = (int)(hash3(i, 2, f / 6) % (unsigned)(spread * 2)) - spread;
            if (dx * dx + dy * dy > spread * spread) continue;
            int drift = (int)((f + (unsigned)i * 7u) % 9u) - 4;
            px(176 + dx + drift / 2, VIS_Y + 72 + dy - drift / 3, (i & 3) ? C_MID : C_FG);
        }
        if (frozen) text(4, VIS_Y + 4, "FROZEN", C_WARN, 1);
        break;
    }
    case PG_MODS:
        if (row >= MATRIX_VIEW) {
            /* the matrix: every route on this track, the selected one highlighted */
            unsigned first = route_sel > 8 ? route_sel - 8 : 0;
            for (unsigned r = first; r < first + 9 && r < VSC_MOD_ROUTES; ++r) {
                const struct vsc_route *ro = &vsc.route[track][r];
                int y = VIS_Y + 3 + (int)(r - first) * 15;
                if (r == route_sel) rect(0, y - 3, VSC_SCREEN_W, 13, C_PANEL);
                char b[12]; num(b, (int)r + 1);
                text(4, y, b, r == route_sel ? C_INK : C_DIM, 1);
                if (ro->param == 255) { text(28, y, "--", C_LINE, 1); continue; }
                char src[3] = {(char)('1' + (ro->src & 3)), 0, 0};
                text(28, y, src, C_MID, 1);
                text(40, y, mtype_name[vsc.t[track][P_MOD(ro->src & 3, 0)] & 3], C_MID, 1);
                char dn[10]; vsc_mod_dest_name(ro->param, dn);
                text_n(92, y, dn, C_FG, 9);
                int w = ro->depth * 30 / 127, cx = 196;
                hline(cx - 30, y + 3, 61, C_LINE);
                if (w > 0) rect(cx, y + 1, w, 5, C_FG); else rect(cx + w, y + 1, -w, 5, C_FG);
                if (ro->flags & 1) px(cx, y - 1, C_INK);
            }
            break;
        }
        {
            /* the modulator's shape, filled up to where it is now, as on the S-4 */
            unsigned m = row / 3, ty = vsc.t[track][P_MOD(m, 0)] & 3;
            int base = VIS_Y + 140, h = 118;
            float ph = vsc_meter.mod_phase[track][m];
            int phx = ty == MT_WAVE || ty == MT_RANDOM ? (int)(ph * 239.f) : 239;
            int prev = -1;
            for (int xx = 0; xx < VSC_SCREEN_W; ++xx) {
                float v = curve[xx];
                int y = base - (int)(v * (float)h);
                if (xx <= phx && (ty == MT_WAVE || ty == MT_RANDOM)) {
                    for (int yy = y + 2; yy < base; yy += 1) px(xx, yy, mix565(C_PANEL, C_MID, (xx * 256) / (phx + 1)));
                }
                if (prev >= 0) { int a0 = prev < y ? prev : y, b0 = prev < y ? y : prev; vline(xx, a0, b0 - a0 + 2, C_MOD[m]); }
                else rect(xx, y, 1, 2, C_MOD[m]);
                prev = y;
            }
            if (ty == MT_ADSR) {
                int yy = base - (int)(vsc_meter.mod_value[track][m] * (float)h);
                for (int xx = 0; xx < VSC_SCREEN_W; xx += 4) px(xx, yy, C_INK);
                for (unsigned k = 0; k < 4; ++k) rect(adsr_node[k][0] - 1, base - adsr_node[k][1] * h / 255 - 1, 3, 3, C_INK);
            }
            if (ty == MT_FOLLOW) for (int xx = 0; xx < VSC_SCREEN_W; xx += 2) vline(xx, base - (int)(curve[xx] * (float)h), (int)(curve[xx] * (float)h), C_LINE);
            /* the four slots along the top: type and live value */
            for (unsigned k = 0; k < VSC_MODS; ++k) {
                int x0 = 4 + (int)k * 59;
                char lab[10]; copy_n(lab, mtype_name[vsc.t[track][P_MOD(k, 0)] & 3], 6);
                text(x0, VIS_Y + 2, lab, k == m ? C_MOD[k] : C_DIM, 1);
                rect(x0, VIS_Y + 11, (int)(vsc_meter.mod_value[track][k] * 50.f), 2, k == m ? C_MOD[k] : C_LINE);
            }
        }
        break;
    case PG_MIX:
        for (unsigned k = 0; k < VSC_TRACKS; ++k) {
            int x = 8 + (int)k * 58;
            char lab[4] = {'T', (char)('1' + k), 0, 0};
            text(x, VIS_Y + 2, lab, k == track ? C_INK : C_DIM, 2);
            static const char *const modes[] = {"PLAY", "LOOP", "KEYS", "THRU"};
            text(x, VIS_Y + 22, modes[vsc.t[k][P_MODE] & 3], C_MID, 1);
            int hgt = (int)(vsc_meter.peak[k] * 90.f); if (hgt > 90) hgt = 90;
            rect(x, VIS_Y + 40, 10, 90, C_PANEL);
            rect(x, VIS_Y + 130 - hgt, 10, hgt, vsc.t[k][P_MUTE] ? C_DIM : C_FG);
            int lv = vsc.t[k][P_LEVEL] * 90 / 127;
            hline(x + 13, VIS_Y + 130 - lv, 8, C_INK);
            if (vsc.t[k][P_MUTE]) text(x + 14, VIS_Y + 40, "M", C_WARN, 1);
        }
        break;
    case PG_SCENE:
        for (unsigned s = 0; s < VSC_SCENES; ++s) {
            int x = 6 + (int)(s % 4) * 58, y = VIS_Y + 4 + (int)(s / 4) * 34;
            uint16_t c = vsc.scene[s].used ? (s == vsc.g[G_TARGET] ? C_INK : C_MID) : C_LINE;
            rect(x, y, 50, 26, C_PANEL);
            char lab[4] = {'S', (char)('1' + s), 0, 0};
            text(x + 4, y + 9, lab, c, 1);
            if (s == vsc.g[G_TARGET]) hline(x, y + 25, 50, C_FG);
        }
        for (unsigned m = 0; m < VSC_MACROS; ++m) {
            int x0 = m < 4 ? 6 : 124, y = VIS_Y + 80 + (int)(m & 3) * 14;
            char lab[4] = {'M', (char)('1' + m), 0, 0};
            text(x0, y, lab, m == vsc.g[G_MSEL] ? C_INK : C_DIM, 1);
            rect(x0 + 16, y + 2, 92, 3, C_PANEL);
            rect(x0 + 16, y + 2, vsc.g[G_M0 + m] * 92 / 127, 3, C_FG);
        }
        break;
    case PG_STEPS: if (vsc.t[track][P_SEQM] == SEQ_CAROM) {
        int x0 = 20, y0 = VIS_Y + 4, w = 200, h = 128;
        hline(x0, y0, w, C_LINE); hline(x0, y0 + h, w, C_LINE); vline(x0, y0, h, C_LINE); vline(x0 + w, y0, h + 1, C_LINE);
        static const char *const iv[3] = {"-12", "0", "-7"};
        static const char *const ivr[3] = {"+12", "0", "+7"};
        text(2, y0 + h / 2 - 3, iv[vsc.t[track][P_RWALLS] % 3], C_DIM, 1);
        text(x0 + w + 3, y0 + h / 2 - 3, ivr[vsc.t[track][P_RWALLS] % 3], C_DIM, 1);
        int count = 0;
        for (unsigned b = 0; b < VSC_BALLS; ++b) {
            if (!vsc_meter.ball_on[track][b]) continue;
            ++count;
            int bx = x0 + 2 + vsc_meter.ball_x[track][b] * (w - 4) / 255, byy = y0 + 2 + vsc_meter.ball_y[track][b] * (h - 4) / 255;
            int hit = vsc_meter.ball_hit[track][b];
            /* the ball is a small cluster of grains; it flares when it hits a wall */
            rect(bx - 1, byy - 1, 3, 3, hit > 90 ? C_INK : C_FG);
            px(bx - 3, byy, C_MID); px(bx + 3, byy + 1, C_MID); px(bx, byy - 3, C_DIM); px(bx + 1, byy + 3, C_DIM);
            if (hit > 30) { int r = 3 + (255 - hit) / 24; px(bx - r, byy, C_MID); px(bx + r, byy, C_MID); px(bx, byy - r, C_MID); px(bx, byy + r, C_MID); }
        }
        if (!count) text(x0 + 14, y0 + h / 2 - 3, live_rec ? "PLAY NOTES TO THROW BALLS" : "NO STEPS: PRESS REC, PLAY", C_DIM, 1);
        else if (!vsc_meter.playing) text(x0 + 4, y0 + h + 4, "PLAY TO SET THEM MOVING", C_DIM, 1);
        if (live_rec) text(x0 + w - 54, y0 + h + 4, "RECORDING", C_WARN, 1);
        break;
    } else {
        unsigned len = vsc.length[track] ? vsc.length[track] : 16;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned s = step_page * 16 + i;
            int x = 4 + (int)(i % 8) * 29, y = VIS_Y + 8 + (int)(i / 8) * 50;
            const struct vsc_step *st = &vsc.step[track][s];
            uint16_t c = s >= len ? C_BG : st->on ? C_FG : C_PANEL;
            rect(x, y, 25, 38, c);
            if (s == step_sel) { hline(x, y + 41, 25, C_INK); }
            if (vsc_meter.playing && vsc_meter.step[track] == s) rect(x, y - 4, 25, 2, C_WARN);
            if (st->lp[0] < VSC_NP) rect(x + 20, y + 3, 3, 3, st->on ? C_BG : C_MID);
        }
        char b[16] = "PAGE  /4"; b[5] = (char)('1' + step_page);
        text(4, VIS_Y + 118, b, C_DIM, 1);
        if (live_rec) text(150, VIS_Y + 118, "RECORDING", C_WARN, 1);
        break;
    }
    case PG_PROJECT:
        for (unsigned s = 0; s < VSC_PROJECTS; ++s) {
            int y = VIS_Y + 2 + (int)s * 17;
            char lab[16] = {(char)('1' + s), ' ', 0};
            if (s == proj_slot) rect(0, y - 3, VSC_SCREEN_W, 14, C_PANEL);
            text(6, y, lab, s == proj_slot ? C_INK : C_DIM, 1);
            text_n(24, y, vsc_host.slot_used[s] ? vsc_host.slot_name[s] : "EMPTY", vsc_host.slot_used[s] ? C_FG : C_LINE, 35);
        }
        if (!vsc_host.storage_ok) text(6, VIS_Y + 140, "STORAGE NOT AVAILABLE", C_WARN, 1);
        break;
    case PG_SOUND: {
        /* eight at a time; a sound is MATERIAL to COLOR and the modulators of one track */
        static const char *const mode_n[] = {"PLAY", "LOOP", "KEYS", "THRU"};
        static const char *const ft_n[] = {"LP", "BP", "HP", "PEAKS", "RESON", "NO FILTER"};
        unsigned first = sound_slot / 8u * 8u;
        for (unsigned s = first; s < first + 8u; ++s) {
            int y = VIS_Y + 2 + (int)(s - first) * 17;
            const struct vsc_sound *so = &vsc_dev.sound[s];
            char lab[4] = {(char)('0' + (s + 1) / 10), (char)('0' + (s + 1) % 10), 0};
            if (s == sound_slot) rect(0, y - 3, VSC_SCREEN_W, 14, C_PANEL);
            text(6, y, lab, s == sound_slot ? C_INK : C_DIM, 1);
            if (!so->used) { text(30, y, "EMPTY", C_LINE, 1); continue; }
            text(30, y, mode_n[so->p[P_MODE] & 3], C_FG, 1);
            text(80, y, ft_n[so->p[P_FTYPE] <= FT_OFF ? so->p[P_FTYPE] : FT_OFF], C_DIM, 1);
        }
        char t[24] = "REC LOADS ONTO TRACK "; t[21] = (char)('1' + track); t[22] = 0;
        text(6, VIS_Y + 140, t, C_DIM, 1);
        break;
    }
    case PG_SETTINGS: if (row == SET_INPUT) {
        /* AUDIO IN: every way sound gets in, in one place. The FM-1 has no analogue input. */
        static const char *const mode_n[] = {"PLAY", "LOOP", "KEYS", "THRU"};
        const char *bt = !vsc_host.bt_audio ? "OFF" : vsc_host.bt_audio_playing ? "PLAYING"
                       : vsc_host.bt_audio_connected ? "CONNECTED" : "WAITING";
        text(6, VIS_Y + 4, "BLUETOOTH", C_DIM, 1); text(80, VIS_Y + 4, bt, vsc_host.bt_audio ? C_FG : C_LINE, 1);
        text(6, VIS_Y + 18, "USB", C_DIM, 1);
        text(80, VIS_Y + 18, vsc_host.usb_connected ? "CONNECTED" : "NOT CONNECTED", vsc_host.usb_connected ? C_FG : C_LINE, 1);
        /* level meters: left over right, for each input */
        for (unsigned s = 0; s < 2; ++s) {
            int y = VIS_Y + 40 + (int)s * 18;
            text(6, y, s ? "USB" : "BT", C_DIM, 1);
            for (unsigned c = 0; c < 2; ++c) {
                float v = vsc_meter.in_peak[s][c]; if (v > 1.f) v = 1.f;
                int w = (int)(v * 190.f), yy = y + (int)c * 5 - 1;
                rect(40, yy, 190, 3, C_PANEL);
                rect(40, yy, w, 3, v > 0.95f ? C_WARN : C_FG);
            }
        }
        unsigned m = vsc.t[track][P_MODE] & 3;
        char b[32] = "TRACK "; b[6] = (char)('1' + track); b[7] = ' '; b[8] = 0;
        copy_n(b + 8, mode_n[m], 6);
        text(6, VIS_Y + 86, b, C_FG, 1);
        if (m == MODE_LOOP || m == MODE_THRU) {
            text(6, VIS_Y + 100, m == MODE_LOOP ? "LISTENS TO INPUT. REC ON ITS" : "PLAYS INPUT THROUGH ITS DEVICES.", C_DIM, 1);
            if (m == MODE_LOOP) text(6, VIS_Y + 112, "MATERIAL PAGE CATCHES A LOOP.", C_DIM, 1);
        } else {
            text(6, VIS_Y + 100, "PLAYS SAMPLES, NOT THE INPUT.", C_DIM, 1);
            text(6, VIS_Y + 112, "SET LOOP OR THRU: EDIT + PRESETS.", C_DIM, 1);
        }
        text(6, VIS_Y + 130, "SAMPLES: LOADED OVER USB WITH", C_LINE, 1);
        text(6, VIS_Y + 142, "THE M-VAVE WORKBENCH.", C_LINE, 1);
        break;
    } else if (row == SET_CLOCK) {
        text(6, VIS_Y + 4, "CLOCK", C_DIM, 1);
        text(80, VIS_Y + 4, vsc_meter.ext_clock ? "MIDI CLOCK ARRIVING" : "NO MIDI CLOCK", vsc_meter.ext_clock ? C_FG : C_LINE, 1);
        text(6, VIS_Y + 20, "FOLLOWING", C_DIM, 1);
        text(80, VIS_Y + 20, vsc.g[G_CLOCK] == CLOCK_INT || (vsc.g[G_CLOCK] == CLOCK_AUTO && !vsc_meter.ext_clock) ? "INTERNAL" : "MIDI", C_FG, 1);
        text(6, VIS_Y + 44, "MIDI IN FROM THE JACK, USB AND", C_DIM, 1);
        text(6, VIS_Y + 56, "BLUETOOTH. CH 1-4: TRACKS 1-4,", C_DIM, 1);
        text(6, VIS_Y + 68, "CH 16: SPACE, MIX AND MACROS.", C_DIM, 1);
        text(6, VIS_Y + 80, "PROGRAM CHANGE: SCENES.", C_DIM, 1);
        break;
    } else if (row == SET_PANEL) {
        /* PANEL: the FM-1's own settings, the version and the load */
        text(6, VIS_Y + 4, "VISCERA " VSC_VERSION, C_FG, 1);
        text(6, VIS_Y + 18, "THESE SETTINGS BELONG TO THE FM-1:", C_DIM, 1);
        text(6, VIS_Y + 30, "KEPT THROUGH POWER-OFF AND LOADS.", C_DIM, 1);
        text(6, VIS_Y + 50, "BACKLIT: HOW THE UNLIT KEYS GLOW", C_DIM, 1);
        text(6, VIS_Y + 62, "KNOB CC: CC 110-113 TURN KNOB 1-4", C_DIM, 1);
        text(6, VIS_Y + 74, "METRO: A CLICK ON EVERY BEAT", C_DIM, 1);
        if (vsc_host.cpu == 255) { text(6, VIS_Y + 100, "CPU: SHOWN ON THE FM-1", C_DIM, 1); break; }   /* the emulator */
        char b[16] = "CPU "; num(b + 4, vsc_host.cpu); unsigned l = (unsigned)vsc_strlen(b); b[l] = '%'; b[l + 1] = 0;
        text(6, VIS_Y + 100, b, vsc_host.cpu > 85 ? C_WARN : C_FG, 1);
        rect(60, VIS_Y + 102, 170, 3, C_PANEL);
        rect(60, VIS_Y + 102, vsc_host.cpu * 170 / 100, 3, vsc_host.cpu > 85 ? C_WARN : C_FG);
        if (vsc_host.cpu_peak) { int px0 = 60 + vsc_host.cpu_peak * 170 / 100; vline(px0 > 229 ? 229 : px0, VIS_Y + 99, 9, C_INK); }
        break;
    } else if (row == SET_LEARN) {
        int any = 0;
        unsigned first = learn_sel > 8 ? learn_sel - 8 : 0;
        for (unsigned l = first; l < first + 9 && l < VSC_LEARN; ++l) {
            const struct vsc_learn *L = &vsc_dev.learn[l];
            int y = VIS_Y + 3 + (int)(l - first) * 15;
            if (l == learn_sel) rect(0, y - 3, VSC_SCREEN_W, 13, C_PANEL);
            if (L->param == 255) { text(6, y, "--", C_LINE, 1); continue; }
            any = 1;
            char b[12] = "CH"; num(b + 2, L->ch + 1); text(6, y, b, C_MID, 1);
            char c[12] = "CC"; num(c + 2, L->cc); text(44, y, c, C_FG, 1);
            char tk[4] = {'T', (char)('1' + (L->track & 3)), 0, 0};
            if (L->param < VSC_NP) text(90, y, tk, C_DIM, 1);
            char dn[10]; vsc_mod_dest_name(L->param, dn); text_n(112, y, dn, C_FG, 9);
        }
        if (!any && VIS_Y + 120 >= by0) text(6, VIS_Y + 128, "HOLD SAVE, TURN A KNOB, SEND A CC", C_DIM, 1);
        break;
    } else {
        static const char *const states[] = {"OFF", "IDLE", "WAITING", "SCANNING", "CONNECTING", "CHECKING", "CONNECTED", "READY", "DISCONNECTING"};
        text(6, VIS_Y + 4, "BLUETOOTH AUDIO IN", C_DIM, 1);
        text(150, VIS_Y + 4, vsc_host.bt_audio ? (vsc_host.bt_audio_playing ? "PLAYING" : vsc_host.bt_audio_connected ? "LINKED" : "ON") : "OFF", C_FG, 1);
        text(6, VIS_Y + 20, "BLUETOOTH MIDI", C_DIM, 1);
        text(150, VIS_Y + 20, vsc_host.bt_midi ? states[vsc_host.ble_state % 9] : "OFF", C_FG, 1);
        text(6, VIS_Y + 36, "MIDI ROLE", C_DIM, 1);
        text(150, VIS_Y + 36, vsc_host.ble_role ? "CENTRAL" : "PERIPHERAL", C_FG, 1);
        text(6, VIS_Y + 52, "USB", C_DIM, 1);
        text(150, VIS_Y + 52, vsc_host.usb_connected ? "CONNECTED" : "--", C_FG, 1);
        for (unsigned i = 0; i < vsc_host.ble_count && i < 4; ++i)
            text_n(12, VIS_Y + 74 + (int)i * 14, vsc_host.ble_name[i], i == set_dev ? C_INK : C_DIM, 37);
        if (!vsc_host.ble_count && vsc_host.ble_role) text(12, VIS_Y + 74, "REC: SCAN FOR MIDI DEVICES", C_DIM, 1);
        break;
    }
    default: break;
    }
}

/* Per-frame preparation: loop overview, curves, mod history. */
static void compute_wave(void)
{
    unsigned t = track;
    for (int x = 0; x < VSC_SCREEN_W; ++x) wave[x] = 0;
    if (page == PG_GRAIN) {
        const uint8_t *ring = vsc_grain_ring(t);
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            uint32_t a = (uint32_t)x * VSC_GRAIN_FRAMES / VSC_SCREEN_W, b = (uint32_t)(x + 1) * VSC_GRAIN_FRAMES / VSC_SCREEN_W;
            float pk = 0.f;
            for (uint32_t i = a; i < b; i += 4) { float v = vsc_ulaw(ring[i]); if (v < 0) v = -v; if (v > pk) pk = v; }
            wave[x] = (uint8_t)(pk > 1.f ? 60 : pk * 60.f);
        }
        return;
    }
    if (vsc.t[t][P_MODE] == MODE_LOOP || (vsc.t[t][P_MODE] == MODE_KEYS && vsc.t[t][P_SLOT] >= VSC_BANK_SLOTS)) {
        uint32_t frames, cap;
        const uint8_t *pool = vsc_loop_memory(t, &frames, &cap);
        uint32_t base = vsc_loop_base(t), total = VSC_LOOP_SLICES * VSC_LOOP_SLICE;
        uint32_t n = frames ? frames : cap;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            uint32_t a = (uint32_t)x * n / VSC_SCREEN_W, b = (uint32_t)(x + 1) * n / VSC_SCREEN_W;
            float pk = 0.f;
            for (uint32_t i = a; i < b; i += 3) { float v = vsc_ulaw(pool[(base + i) % total]); if (v < 0) v = -v; if (v > pk) pk = v; }
            wave[x] = (uint8_t)(pk > 1.f ? 60 : pk * 60.f);
        }
        return;
    }
    /* PLAY / KEYS from the bank: the overview the audio side builds */
    if ((vsc.t[t][P_MODE] == MODE_PLAY || vsc.t[t][P_MODE] == MODE_KEYS) && vsc_meter.overview_slot[t] == vsc.t[t][P_SLOT]) {
        for (int x = 0; x < VSC_SCREEN_W; ++x) wave[x] = vsc_meter.overview[t][x];
        return;
    }
    /* THRU: a coarse picture from the main scope */
    for (int x = 0; x < VSC_SCREEN_W; ++x) {
        float v = vsc_scope[0][x * 128 / VSC_SCREEN_W];
        if (v < 0) v = -v;
        wave[x] = (uint8_t)(v > 1.f ? 60 : v * 60.f);
    }
}
static float fexp2(float x)
{
    int i = (int)x; if ((float)i > x) --i;
    float f = x - (float)i, p = 1.f + f * (0.6951786f + f * (0.2261598f + f * 0.0782371f));
    union { float f; uint32_t u; } v; v.f = p; v.u += (uint32_t)i << 23; return v.f;
}
static void compute_curve(void)
{
    unsigned t = track;
    if (page == PG_FILTER && vsc.t[t][P_FTYPE] == FT_RING) {
        /* the shape CUTOFF, SLOPE and RESO give the bank */
        float cut = (float)vsc.t[t][P_FCUT] / 127.f, reso = (float)vsc.t[t][P_FRES] / 127.f, slope = (float)vsc.t[t][P_KSLOPE] / 127.f;
        float width = 0.04f + (1.f - reso) * (1.f - reso) * 0.4f;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float u = (float)x / 239.f, d = (u - cut) / width, bp = 1.f / (1.f + d * d); bp *= bp;
            float lp = d <= 0.f ? 1.f : bp, hp = d >= 0.f ? 1.f : bp;
            float g = slope < 0.5f ? lp + (bp - lp) * slope * 2.f : bp + (hp - bp) * (slope - 0.5f) * 2.f;
            g *= 1.f + reso * 2.f * bp;
            curve[x] = g / 3.f > 1.f ? 1.f : g / 3.f;
        }
    } else if (page == PG_FILTER) {
        unsigned type = vsc.t[t][P_FTYPE];
        float fc = 20.f * fexp2((float)vsc.t[t][P_FCUT] / 127.f * 9.95f);
        float q = 0.5f + 12.f * (float)vsc.t[t][P_FRES] / 127.f;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float f = 20.f * fexp2((float)x / 239.f * 9.95f), r = f / fc, m;
            float d2 = (1.f - r * r) * (1.f - r * r) + (r / q) * (r / q);
            if (type == FT_LP) m = 1.f / d2;
            else if (type == FT_HP) m = r * r * r * r / d2;
            else if (type == FT_OFF) m = 1.f;
            else m = (r / q) * (r / q) / d2 * (type == FT_PEAKS ? 1.f : 1.f);
            if (type == FT_PEAKS) {
                float s = 0.f;
                for (int b = 0; b < 4; ++b) {
                    static const int8_t iv[8][4] = {{0,12,24,36},{0,7,12,19},{0,4,7,11},{0,3,7,10},{0,5,7,12},{0,12,19,24},{0,2,4,6},{0,1,2,3}};
                    float fb = fc * 0.25f * fexp2((float)iv[vsc.t[t][P_FSCALE] & 7][b] * (float)vsc.t[t][P_FSPREAD] / 127.f * 2.f / 12.f);
                    float rb = f / fb, qq = q * 3.f, dd = (1.f - rb * rb) * (1.f - rb * rb) + (rb / qq) * (rb / qq);
                    s += (rb / qq) * (rb / qq) / dd;
                }
                m = s * 0.5f;
            }
            /* log magnitude, -36..+12 dB */
            float db = 0.f, mm = m < 1e-6f ? 1e-6f : m;
            union { float f; uint32_t u; } u; u.f = mm;
            db = ((float)((int)(u.u >> 23) - 127) + (float)(u.u & 0x7fffff) / 8388608.f) * 3.0103f;
            float y = (db + 36.f) / 48.f;
            curve[x] = y < 0 ? 0 : y > 1 ? 1 : y;
        }
    } else if (page == PG_COLOR) {
        float drive = (float)vsc.t[t][P_DRIVE] / 127.f, bits = vsc.t[t][P_CMODE] == 1 ? 0.f : (float)vsc.t[t][P_CRUSH] / 127.f;
        float pre = 1.f + drive * drive * 24.f, lev = fexp2(1.f + (1.f - bits) * 14.f);
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float in = (float)x / 239.f * 2.f - 1.f, v = in * pre;
            if (v > 3.f) v = 3.f; if (v < -3.f) v = -3.f;
            v = v * (27.f + v * v) / (27.f + 9.f * v * v);
            if (bits > 0.01f) { float q = v * lev; int qi = (int)(q + (q < 0 ? -0.5f : 0.5f)); v = (float)qi / lev; }
            curve[x] = (v + 1.f) * 0.5f;
        }
    } else if (page == PG_SPACE) {
        float fb = (float)vsc.g[G_DFEED] / 127.f * 0.95f;
        static const float beats[9] = {0.125f, 1.f / 6.f, 0.25f, 1.f / 3.f, 0.375f, 0.5f, 2.f / 3.f, 0.75f, 1.f};
        float period = beats[vsc.g[G_DTIME] % 9] * 4.f * 60.f / (float)vsc.tempo;
        for (int x = 0; x < VSC_SCREEN_W; ++x) curve[x] = 0.f;
        float a = 1.f;
        for (int k = 0; k < 24 && a > 0.02f; ++k) {
            int x = (int)((float)(k + 1) * period / 4.f * 239.f);
            if (x >= VSC_SCREEN_W) break;
            curve[x] = a; if (x + 1 < VSC_SCREEN_W) curve[x + 1] = a;
            a *= fb;
        }
        /* reverb tail as a dim ramp */
        float rs = (float)vsc.g[G_RSIZE] / 127.f;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float tail = 0.5f * fexp2(-(float)x / (20.f + rs * 160.f) * 3.f);
            if (tail > curve[x]) curve[x] = tail;
        }
    }
}
/* the selected modulator's shape, for its picture */
static void compute_mod_curve(void)
{
    unsigned m = row / 3, ty = vsc.t[track][P_MOD(m, 0)] & 3;
    const uint8_t *b = &vsc.t[track][P_MOD(m, 0)];
    if (ty == MT_WAVE) {
        float amount = b[2] / 127.f, off = b[4] / 127.f * 2.f - 1.f, sk = b[5] / 127.f, fold = b[6] / 127.f, cv = b[7] / 127.f * 2.f - 1.f;
        if (sk < 0.02f) sk = 0.02f;
        if (sk > 0.98f) sk = 0.98f;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float p = (float)x / 239.f + b[3] / 127.f; if (p >= 1.f) p -= 1.f;
            p = p < sk ? p / sk * 0.5f : 0.5f + (p - sk) / (1.f - sk) * 0.5f;
            float v;
            switch (b[8]) {
            case 0: { float q = p < 0.75f ? p + 0.25f : p - 0.75f; float tri = q < 0.5f ? q * 4.f - 1.f : 3.f - q * 4.f; v = 0.5f - 0.5f * tri * (1.5f - 0.5f * tri * tri); break; }
            case 1: v = p < 0.5f ? p * 2.f : 2.f - p * 2.f; break;
            case 2: v = p < 0.5f ? 1.f : 0.f; break;
            case 3: v = 1.f - p; break;
            case 4: v = p; break;
            default: v = (float)(hash3((int)(p * 8.f), 5, 9) & 255) / 255.f; break;
            }
            if (fold > 0.f) {
                float y = (v * 2.f - 1.f) * (1.f + fold * 3.f);
                for (int i = 0; i < 4 && (y > 1.f || y < -1.f); ++i) y = y > 1.f ? 2.f - y : -2.f - y;
                v = (y + 1.f) * 0.5f;
            }
            v = cv > 0.f ? v + (v * v * v - v) * cv : v + ((1.f - (1.f - v) * (1.f - v) * (1.f - v)) - v) * -cv;
            v = v * amount + off;
            curve[x] = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        }
    } else if (ty == MT_ADSR) {
        float A = 0.001f + 4.f * (b[1] / 127.f) * (b[1] / 127.f), D = 0.005f + 4.f * (b[2] / 127.f) * (b[2] / 127.f);
        float S = b[3] / 127.f, R = 0.005f + 6.f * (b[4] / 127.f) * (b[4] / 127.f), H = 0.4f, tot = A + D + H + R;
        int xa = (int)(A / tot * 239.f), xd = xa + (int)(D / tot * 239.f), xh = xd + (int)(H / tot * 239.f);
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            float v;
            if (x <= xa) v = xa ? (float)x / (float)xa : 1.f;
            else if (x <= xd) v = 1.f - (1.f - S) * (float)(x - xa) / (float)(xd - xa ? xd - xa : 1);
            else if (x <= xh) v = S;
            else v = S * (1.f - (float)(x - xh) / (float)(239 - xh ? 239 - xh : 1));
            curve[x] = v * (b[8] / 127.f);
        }
        int amt = b[8] * 2;
        adsr_node[0][0] = 0; adsr_node[0][1] = 0;
        adsr_node[1][0] = xa; adsr_node[1][1] = amt;
        adsr_node[2][0] = xd; adsr_node[2][1] = (int)(S * (float)amt);
        adsr_node[3][0] = xh; adsr_node[3][1] = (int)(S * (float)amt);
    } else {
        /* RANDOM and FOLLOW: what they have done lately */
        for (int x = 0; x < VSC_SCREEN_W; ++x) curve[x] = (float)mod_hist[m][(hist_w + (unsigned)(x * 96 / VSC_SCREEN_W)) % 96] * (1.f / 255.f);
    }
}
void vsc_ui_frame(uint32_t now)
{
    now_ms = now;
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        mod_hist[m][hist_w] = (uint8_t)(vsc_meter.mod_value[track][m] * 255.f);
    }
    hist_w = (hist_w + 1) % 96;
    compute_wave();
    compute_curve();
    if (page == PG_MODS && row < MATRIX_VIEW) compute_mod_curve();
}
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels)
{
    band = pixels; by0 = (int)y0;
    /* Background: black. VSC_SCREEN_GRADIENT 1 brings back the alternative, a dark
     * violet-grey gradient lighter at the top (#2C2A32 to #100F12), kept for later. */
#ifndef VSC_SCREEN_GRADIENT
#define VSC_SCREEN_GRADIENT 0
#endif
    for (unsigned r = 0; r < VSC_BAND; ++r) {
        uint16_t c = VSC_SCREEN_GRADIENT ? mix565(RGB(44, 42, 50), RGB(16, 15, 18), (int)((y0 + r) * 256 / VSC_SCREEN_H)) : RGB(0, 0, 0);
        for (unsigned i = 0; i < VSC_SCREEN_W; ++i) pixels[r * VSC_SCREEN_W + i] = c;
    }
    draw_header();
    frame_box();
    draw_vis();
    draw_knobs();
}

/* ------------------------------------------------------------- panel LEDs */
void vsc_ui_leds(uint8_t out[41])
{
    uint8_t glow = vsc_dev.backlight ? 1 : 0;
    for (unsigned i = 0; i < 41; ++i) out[i] = glow;
    /* the open page's button */
    static const uint8_t page_btn[PG_COUNT] = {BTN_HOME, BTN_EDIT, BTN_FX, BTN_SCL, BTN_ENV, BTN_LFO, BTN_GLO,
                                               BTN_ARP, BTN_SEQ, BTN_SAVE, BTN_GLO, BTN_SAVE};
    out[page_btn[page]] = 2;
    if (vsc_meter.playing) out[BTN_PLAY] = 2;
    int recording = live_rec || (vsc.t[track][P_MODE] == MODE_LOOP && (vsc_meter.loop_state[track] == 1 || vsc_meter.loop_state[track] == 3));
    if (recording) out[BTN_REC] = (now_ms / 250u) & 1u ? 2 : 0;
    if (octave < 2) out[BTN_OCTD] = 2;
    if (octave > 2) out[BTN_OCTU] = 2;
    if (temp_on) out[BTN_OCTD] = out[BTN_OCTU] = 2;
    for (unsigned b = 0; b < 14; ++b) if (prev_buttons & (1u << b)) out[b] = 2;
    /* keys: what's held, the steps of the pattern on STEPS, scenes with ARP, tracks with HOME */
    if (page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_STEPS) {
        unsigned len = vsc.length[track] ? vsc.length[track] : 16;
        for (unsigned k = 0; k < 16; ++k) {
            unsigned st = step_page * 16 + k;
            out[14 + k] = st >= len ? 0 : vsc.step[track][st].on ? 2 : glow;
            if (vsc_meter.playing && vsc_meter.step[track] == st) out[14 + k] = vsc.step[track][st].on ? 0 : 2;
        }
    }
    if (prev_buttons & (1u << BTN_ARP)) for (unsigned k = 0; k < VSC_SCENES; ++k) out[14 + k] = vsc.scene[k].used ? 2 : glow;
    if (prev_buttons & (1u << BTN_HOME)) for (unsigned k = 23; k < 27; ++k) out[14 + k] = k - 23 == track ? 2 : glow;
    for (unsigned k = 0; k < 27; ++k) if (prev_keys & (1u << k)) out[14 + k] = 2;
}

/* CC 110-113: the knob of that column on the page on screen, with pickup. Called from the
 * MIDI task; it only queues, the control task applies it in vsc_ui_update. */
static volatile uint8_t knob_cc_val[4], knob_cc_new[4];
static int16_t knob_cc_last[4] = {-1, -1, -1, -1};
static uint8_t knob_cc_held[4];
static unsigned knob_cc_view = 0xffff;
void vsc_ui_knob_cc(unsigned knob, unsigned value)
{
    if (knob > 3) return;
    knob_cc_val[knob] = (uint8_t)value; knob_cc_new[knob] = 1;
}
static void apply_knob_cc(unsigned *req, unsigned *arg)
{
    unsigned view = page * 64u + row * 4u + track;
    if (view != knob_cc_view) { knob_cc_view = view; for (unsigned k = 0; k < 4; ++k) { knob_cc_held[k] = 0; knob_cc_last[k] = -1; } }
    for (unsigned k = 0; k < 4; ++k) {
        if (!knob_cc_new[k]) continue;
        knob_cc_new[k] = 0;
        int v = knob_cc_val[k];
        struct slot s = slot_at(k);
        int cur = -1, max = 0;
        if (s.kind == S_T || s.kind == S_TT) { unsigned t = s.kind == S_TT ? s.tk : track; max = vsc_tparam_info(t, s.id)->max; cur = vsc.t[t][s.id]; }
        else if (s.kind == S_G) { max = vsc_gparam_info(s.id)->max; cur = vsc.g[s.id]; }
        if (cur < 0 || !max) { knob_cc_last[k] = (int16_t)v; continue; }   /* steps, settings: not by CC */
        int target = (v * max + 63) / 127, now127 = (cur * 127 + max / 2) / max, prev = knob_cc_last[k];
        if (!knob_cc_held[k] && ((v - now127 <= 1 && now127 - v <= 1) ||
            (prev >= 0 && ((prev <= now127 && v >= now127) || (prev >= now127 && v <= now127))))) knob_cc_held[k] = 1;
        knob_cc_last[k] = (int16_t)v;
        if (!knob_cc_held[k]) continue;
        if (target == cur) continue;
        if (s.kind == S_G) vsc.g[s.id] = (uint8_t)target;
        else vsc.t[s.kind == S_TT ? s.tk : track][s.id] = (uint8_t)target;   /* absolute: no acceleration */
        touch_at[k] = now_ms | 1u;
    }
}
