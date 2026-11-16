/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: screen and controls. A tape machine on the screen: smoked body,
 * cream lettering, aluminium knobs, amber VU meters. Everything is drawn from shapes
 * (no pictures), band by band. Portable: the platform hands in a control snapshot and
 * asks for 16-row bands of RGB565 pixels. Control context only. */
#include "vsc.h"

struct vsc_host_status vsc_host;

/* ---------------------------------------------------------------- colour */
#define RGB(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))
#define C_BODY  RGB(28, 26, 23)
#define C_PANEL RGB(43, 39, 35)
#define C_LINE  RGB(74, 68, 60)
#define C_DARK  RGB(38, 35, 31)
#define C_DEEP  RGB(58, 53, 47)
#define C_BLACK RGB(15, 14, 13)
#define C_CREAM RGB(239, 230, 210)
#define C_DIM   RGB(167, 159, 142)
#define C_DIMR  RGB(111, 104, 92)
#define C_AMBER RGB(242, 196, 109)
#define C_FACE  RGB(246, 220, 160)
#define C_BROWN RGB(90, 59, 37)
#define C_RED   RGB(226, 73, 47)
#define C_REDK  RGB(74, 42, 36)
#define C_TAPE  RGB(61, 42, 29)
#define C_TAPE2 RGB(86, 57, 42)
#define C_TAPE3 RGB(107, 70, 48)
#define C_HUB   RGB(207, 203, 193)
#define C_HUB2  RGB(142, 139, 132)
#define C_ALU   RGB(185, 182, 174)
#define C_ALU2  RGB(216, 213, 205)
#define C_GREEN RGB(127, 174, 106)
#define C_LED   RGB(47, 107, 42)
#define C_GLOW  RGB(74, 58, 36)

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
    {0,0,0x7f,0x41,0x41},{0x22,0x14,8,0x14,0x22},{0x41,0x41,0x7f,0,0},{8,8,0x2a,0x1c,8},{0x40,0x40,0x40,0x40,0x40}};

/* ------------------------------------------------------- band painting */
static uint16_t *band; static int by0;   /* the band being painted and its first row */
#define IN_BAND(y0, y1) ((y1) >= by0 && (y0) < by0 + VSC_BAND)
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
/* a box with its corners cut (rounded, at this size) */
static void box(int x, int y, int w, int h, uint16_t c)
{
    if (!IN_BAND(y, y + h)) return;
    rect(x + 1, y, w - 2, 1, c); rect(x, y + 1, w, h - 2, c); rect(x + 1, y + h - 1, w - 2, 1, c);
}
static void frame(int x, int y, int w, int h, uint16_t c)
{
    if (!IN_BAND(y, y + h)) return;
    hline(x + 1, y, w - 2, c); hline(x + 1, y + h - 1, w - 2, c); vline(x, y + 1, h - 2, c); vline(x + w - 1, y + 1, h - 2, c);
}
static int text(int x, int y, const char *s, uint16_t c)
{
    if (y + 8 <= by0 || y >= by0 + VSC_BAND) { while (*s) { x += 6; ++s; } return x; }
    for (; *s; ++s) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font[ch - 32];
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 7; ++cy)
                if (g[cx] >> cy & 1) px(x + cx, y + cy, c);
        x += 6;
    }
    return x;
}
static int text_w(const char *s) { return (int)vsc_strlen(s) * 6 - 1; }
static void text_c(int cx, int y, const char *s, uint16_t c) { text(cx - text_w(s) / 2, y, s, c); }
static void text_r(int xr, int y, const char *s, uint16_t c) { text(xr - text_w(s), y, s, c); }
static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) ++r; return r; }
static void disc(int cx, int cy, int r, uint16_t c)
{
    if (!IN_BAND(cy - r, cy + r)) return;
    int a = cy - r < by0 ? by0 : cy - r, b = cy + r > by0 + VSC_BAND - 1 ? by0 + VSC_BAND - 1 : cy + r;
    for (int y = a; y <= b; ++y) { int dy = y - cy, w = isqrt(r * r - dy * dy + r / 2); rect(cx - w, y, 2 * w + 1, 1, c); }
}
static void ring(int cx, int cy, int r, uint16_t c)
{
    if (!IN_BAND(cy - r, cy + r)) return;
    int a = cy - r < by0 ? by0 : cy - r, b = cy + r > by0 + VSC_BAND - 1 ? by0 + VSC_BAND - 1 : cy + r;
    for (int y = a; y <= b; ++y) {
        int dy = y - cy, w = isqrt(r * r - dy * dy + r / 2), wi = (dy * dy < (r - 1) * (r - 1)) ? isqrt((r - 1) * (r - 1) - dy * dy + r / 2) : -1;
        if (wi < 0) rect(cx - w, y, 2 * w + 1, 1, c);
        else { rect(cx - w, y, w - wi, 1, c); rect(cx + wi + 1, y, w - wi, 1, c); }
    }
}
static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (!IN_BAND(y0 < y1 ? y0 : y1, y0 > y1 ? y0 : y1)) return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void thick(int x0, int y0, int x1, int y1, uint16_t c) { line(x0, y0, x1, y1, c); line(x0 + 1, y0, x1 + 1, y1, c); }

/* ---------------------------------------------------------------- maths */
static float fsin(float x)   /* radians, any */
{
    const float tp = 6.2831853f;
    x -= tp * (float)(int)(x / tp); if (x < 0.f) x += tp;
    if (x > 3.1415927f) return -fsin(x - 3.1415927f);
    float y = x > 1.5707963f ? 3.1415927f - x : x, y2 = y * y;
    return y * (1.f - y2 * (1.f / 6.f - y2 * (1.f / 120.f - y2 * (1.f / 5040.f))));
}
static float fcos(float x) { return fsin(x + 1.5707963f); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void num(char *o, int v)
{
    char t[8]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 7);
    int i = 0; if (neg) o[i++] = '-';
    while (n) o[i++] = t[--n];
    o[i] = 0;
}
static void cat(char *d, const char *s) { unsigned l = (unsigned)vsc_strlen(d); while (*s && l < 23) d[l++] = *s++; d[l] = 0; }
static void cpy(char *d, const char *s) { d[0] = 0; cat(d, s); }
static void secs(char *o, float s)   /* "2.9S" */
{
    int t = (int)(s * 10.f + 0.5f);
    num(o, t / 10); char b[4] = {'.', (char)('0' + t % 10), 'S', 0}; cat(o, b);
}

/* ------------------------------------------------------------- pages */
enum { PG_TAPE, PG_MIXER, PG_INPUT, PG_BOUNCE, PG_RESON, PG_ROBOTS, PG_SAVE, PG_SETTINGS, PG_MASTER, PG_COUNT };
static unsigned page, view, rsel = 1, rob, learn_sel, slot_sel, ble_sel, sel;   /* sel: the selected track */
static uint32_t now_ms, notice_until, touch_at[4];
static const char *notice;
static char notice_buf[32];
static uint32_t prev_buttons, prev_keys;
static uint8_t learn_seen, used_mod;   /* a held modifier was used */
/* a knob slot: a parameter, or one of the page's own controls */
enum { K_NONE = 200, K_LOOP, K_BTAUDIO, K_BTMIDI, K_LIGHT, K_KNOBCC, K_BLE, K_CLEARMAP, K_BAND, K_START };
static unsigned slot(unsigned k)
{
    switch (page) {
    case PG_TAPE:
        if (view == 1) { static const uint8_t s[4] = {S_SPEED, S_PITCH, S_DUB, S_LEVEL}; return P_TRK(sel, s[k]); }
        { static const uint8_t s[4] = {P_SPEED, P_PITCH, K_LOOP, P_TLEVEL}; return s[k]; }
    case PG_MASTER:
        if (view < 4) return P_EQ(view, k);
        { static const uint8_t s[2][4] = {{P_CON, P_CTHR, P_CRATIO, P_CMAKE}, {P_CATK, P_CREL, P_CMIX, P_CKEY}}; return s[view - 4][k]; }
    case PG_MIXER: return P_TRK(k, view == 0 ? S_LEVEL : view == 1 ? S_TRIM : S_PAN);
    case PG_INPUT: { static const uint8_t s[4] = {P_ISRC, P_ITRIM, P_IDRIVE, P_IMON}; return s[k]; }
    case PG_BOUNCE: {
        static const uint8_t s[2][4] = {{P_RMIX, P_BDRIVE, P_BLEVEL, P_BCLEAR}, {P_BDST, P_BRESON, K_NONE, K_NONE}};
        return s[view & 1][k];
    }
    case PG_RESON: {
        static const uint8_t s[2][4] = {{P_RPROF, P_RMIX, P_RDECAY, P_RWIDTH}, {P_RSHIFT, P_RDUST, K_BAND, K_NONE}};
        return s[view & 1][k];
    }
    case PG_ROBOTS:
        if (view >= VSC_ROBOTS) return k == 3 ? K_CLEARMAP : K_NONE;
        return P_ROB(view, k == 0 ? R_RATE : k == 1 ? R_SHAPE : k == 2 ? R_SKEW : R_DEPTH);
    case PG_SETTINGS: {
        static const uint8_t s[2][4] = {{K_BTAUDIO, K_BTMIDI, K_LIGHT, K_START}, {K_BLE, K_KNOBCC, K_NONE, K_NONE}};
        return s[view & 1][k];
    }
    default: return K_NONE;
    }
}
static unsigned views(void)
{
    return page == PG_MASTER ? 6 : page == PG_TAPE ? 2 : page == PG_MIXER ? 3 : page == PG_BOUNCE || page == PG_RESON || page == PG_SETTINGS ? 2 : page == PG_ROBOTS ? VSC_ROBOTS + 1 : 1;
}
static float tape_hz(void) { return vsc_tape_hz(vsc.p[P_TAPESPD] % 3u); }
static void slot_text(unsigned s, char name[10], char val[10])
{
    name[0] = val[0] = 0;
    if (s < VSC_NP) {
        cpy(name, vsc_param_info(s)->name);
        if (page == PG_BOUNCE && s == P_RMIX) cpy(name, "WET");
        vsc_param_label(s, vsc.p[s], val);
        return;
    }
    switch (s) {
    case K_LOOP:
        cpy(name, "LOOP");
        if (vsc_meter.loop && vsc_meter.transport != TR_FIRST) secs(val, (float)vsc_meter.loop / tape_hz());
        else vsc_param_label(P_LOOP, vsc.p[P_LOOP], val);
        break;
    case K_BTAUDIO: cpy(name, "BT AUD"); cpy(val, vsc_host.bt_audio ? "ON" : "OFF"); break;
    case K_BTMIDI: cpy(name, "BT MID"); cpy(val, vsc_host.bt_midi ? "ON" : "OFF"); break;
    case K_LIGHT: { static const char *const l[4] = {"OFF", "LOW", "MID", "HIGH"}; cpy(name, "LIGHT"); cpy(val, l[vsc_dev.backlight & 3]); break; }
    case K_KNOBCC: cpy(name, "CC 110"); cpy(val, vsc_dev.knob_cc ? "KNOBS" : "OFF"); break;
    case K_BLE: cpy(name, "BLE"); cpy(val, vsc_host.ble_role ? "CENTRL" : "PERIPH"); break;
    case K_START: cpy(name, "START"); cpy(val, vsc_dev.start_blank ? "BLANK" : "LAST"); break;
    case K_CLEARMAP: cpy(name, "CLEAR"); cpy(val, ">"); break;
    case K_BAND: { char b[8]; cpy(name, "BAND "); b[0] = (char)('1' + rsel); b[1] = 0; cat(name, b); vsc_param_label(P_RG0 + rsel, vsc.p[P_RG0 + rsel], val); break; }
    default: break;
    }
}
static float slot_frac(unsigned s)
{
    if (s < VSC_NP) { unsigned m = vsc_param_info(s)->max; return m ? (float)vsc.p[s] / (float)m : 0.f; }
    if (s == K_LOOP) return vsc_meter.loop && vsc_meter.cap ? (float)vsc_meter.loop / (float)vsc_meter.cap : (float)vsc.p[P_LOOP] / 127.f;
    if (s == K_BAND) return (float)vsc.p[P_RG0 + rsel] / 127.f;
    if (s == K_LIGHT) return (float)vsc_dev.backlight / 3.f;
    if (s == K_START) return vsc_dev.start_blank ? 0.f : 1.f;
    return 0.f;
}
void vsc_ui_notice(const char *t, uint32_t ms) { notice = t; notice_until = now_ms + ms; }
static void say(const char *a, const char *b, uint32_t ms) { cpy(notice_buf, a); cat(notice_buf, b); vsc_ui_notice(notice_buf, ms); }

/* ---- the robots' gestures: HOME held and a knob turned records it */
static int g_rec = -1;          /* the robot recording, -1 none */
static unsigned g_param, g_n, g_target;
static uint8_t g_v0, g_buf[96];
static uint32_t g_at, g_start;
static int target_of(unsigned p) { for (unsigned i = 0; i < VSC_RTARGETS; ++i) if (vsc_robot_target(i) == p) return (int)i; return -1; }
static void gesture_start(unsigned p)
{
    int ti = target_of(p);
    if (ti < 0) { vsc_ui_notice("NO ROBOT FOR THIS", 900); return; }
    unsigned r = VSC_ROBOTS;
    for (unsigned i = 0; i < VSC_ROBOTS && r == VSC_ROBOTS; ++i) if (vsc.p[P_ROB(i, R_TARGET)] == (unsigned)ti) r = i;
    for (unsigned i = 0; i < VSC_ROBOTS && r == VSC_ROBOTS; ++i) if (!vsc.p[P_ROB(i, R_DEPTH)]) r = i;
    if (r == VSC_ROBOTS) r = 0;
    vsc.p[P_ROB(r, R_DEPTH)] = 0;   /* (the robot rests while its gesture is taken) */
    g_rec = (int)r; g_param = p; g_target = (unsigned)ti; g_v0 = vsc.p[p]; g_n = 0; g_at = g_start = now_ms;
    char n[8]; vsc_target_name((unsigned)ti, n);
    say("RECORDING ROBOT: ", n, 1500);
}
static void gesture_sample(void)
{
    if (g_rec < 0) return;
    while (now_ms - g_at >= 40u && g_n < sizeof g_buf) { g_buf[g_n++] = vsc.p[g_param]; g_at += 40u; }
}
static void gesture_end(void)
{
    if (g_rec < 0) return;
    unsigned r = (unsigned)g_rec; g_rec = -1;
    if (g_n < 3) { vsc_ui_notice("TOO SHORT", 800); vsc.p[g_param] = g_v0; return; }
    int max = vsc_param_info(g_param)->max;
    for (unsigned i = 0; i < VSC_GEST; ++i) {
        unsigned j = i * (g_n - 1u) / (VSC_GEST - 1u);
        int d = ((int)g_buf[j] - (int)g_v0) * 127 / (max ? max : 1);
        vsc.gest[r][i] = (int8_t)clampi(d, -127, 127);
    }
    vsc.gest_ms[r] = (uint16_t)(g_n * 40u);
    vsc.p[g_param] = g_v0;
    vsc.p[P_ROB(r, R_TARGET)] = (uint8_t)g_target; vsc.p[P_ROB(r, R_SHAPE)] = RS_GESTURE; vsc.p[P_ROB(r, R_DEPTH)] = 127;
    say(r ? "ROBOT 2 " : "ROBOT 1 ", "PLAYS IT", 1200);
}

/* ---- editing */
static void edit(unsigned s, int d, unsigned *req, unsigned *arg)
{
    if (s < VSC_NP) {
        const struct vsc_pinfo *in = vsc_param_info(s);
        int v = clampi((int)vsc.p[s] + d, 0, in->max);
        if (s == P_RPROF) { if (v != vsc.p[s]) { vsc_reson_profile((unsigned)v); } return; }
        if (s == P_ISRC && v != IN_USB && !vsc_host.bt_audio) { *req = HOST_BT_AUDIO; *arg = 1; vsc_ui_notice("BLUETOOTH AUDIO ON", 1200); }
        vsc.p[s] = (uint8_t)v;
        return;
    }
    switch (s) {
    case K_LOOP:
        if (vsc_meter.loop && vsc_meter.transport != TR_FIRST) vsc_event(EV_LOOP, (unsigned)(d & 255), 0);
        else vsc.p[P_LOOP] = (uint8_t)clampi((int)vsc.p[P_LOOP] + d, 0, 127);
        break;
    case K_BTAUDIO: *req = HOST_BT_AUDIO; *arg = d > 0; break;
    case K_BTMIDI: *req = HOST_BT_MIDI; *arg = d > 0; break;
    case K_LIGHT: vsc_dev.backlight = (uint8_t)clampi((int)vsc_dev.backlight + d, 0, BACKLIGHT_HIGH); ++vsc_dev_changed; break;
    case K_KNOBCC: vsc_dev.knob_cc = d > 0; ++vsc_dev_changed; break;
    case K_START: vsc_dev.start_blank = d < 0; ++vsc_dev_changed; break;
    case K_BLE: *req = HOST_BLE_ROLE; *arg = d > 0; break;
    case K_CLEARMAP:
        if (d > 0 && vsc_dev.learn[learn_sel].param != 255) { vsc_dev.learn[learn_sel].param = 255; ++vsc_dev_changed; vsc_ui_notice("MAPPING CLEARED", 900); }
        break;
    case K_BAND: vsc.p[P_RG0 + rsel] = (uint8_t)clampi((int)vsc.p[P_RG0 + rsel] + d, 0, 127); break;
    default: break;
    }
}

void vsc_ui_init(void)
{
    page = PG_TAPE; view = 0; rsel = 1; rob = 0;
    prev_buttons = prev_keys = 0;
    learn_seen = vsc_learn_seen;
    g_rec = -1;
    vsc_ui_notice("VISCERA 4-TRACK " VSC_VERSION, 1500);
#ifdef VSC_BENCH
    { void vsc_bench_setup(void); vsc_bench_setup(); }
#endif
}
static void open_page(unsigned pg)
{
    if (page == pg) view = (view + 1u) % views();
    else { page = pg; view = 0; }
}
#define B(n) (1u << (n))
static void apply_knob_cc(void);
static void learn_arm(unsigned kind, unsigned what, const char *name)
{
    vsc_learn_kind = (uint8_t)kind; vsc_learn_param = (uint8_t)what; vsc_learn_armed = 1;
    say("LEARN ", name, 3000);
    used_mod = 1;
}
/* the keys: the first four white keys (F3 G3 A3 B3) are the tracks, the next eight
 * (C4 to C5) RESON's bands on its page */
static const uint8_t trk_key[VSC_TRACKS] = {0, 2, 4, 6}, band_key[VSC_RBANDS] = {7, 9, 11, 12, 14, 16, 18, 19};
static unsigned key_track(unsigned k) { for (unsigned t = 0; t < VSC_TRACKS; ++t) if (trk_key[t] == k) return t; return VSC_TRACKS; }
static unsigned key_band(unsigned k) { for (unsigned b = 0; b < VSC_RBANDS; ++b) if (band_key[b] == k) return b; return VSC_RBANDS; }
static const char *const act_name[ACT_COUNT] = {"REC", "PLAY", "ARM 1", "ARM 2", "ARM 3", "ARM 4", "BOUNCE"};
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg)
{
    unsigned req = HOST_NONE;
    now_ms = in->now_ms;
    const uint32_t down = in->buttons & ~prev_buttons, up = ~in->buttons & prev_buttons;
    const uint32_t kdown = in->keys & ~prev_keys;
    const int home_held = (in->buttons & B(BTN_HOME)) != 0, save_held = (in->buttons & B(BTN_SAVE)) != 0;
    const int clr_held = (in->buttons & B(BTN_ARP)) != 0;   /* ARP is CLEAR */
    if (vsc_learn_seen != learn_seen) { learn_seen = vsc_learn_seen; vsc_ui_notice("LEARNED", 900); }
    if (!(prev_buttons & (B(BTN_HOME) | B(BTN_SAVE) | B(BTN_ARP)))) used_mod = 0;
    if ((up & B(BTN_ARP)) && !used_mod) vsc_ui_notice("HOLD ARP: KEY CLEARS, PLAY ERASES", 1500);

    /* the pages */
    if ((up & B(BTN_HOME)) && !used_mod) open_page(PG_TAPE);
    if ((up & B(BTN_SAVE)) && !used_mod) open_page(PG_SAVE);
    if (down & B(BTN_EDIT)) { if (page == PG_MIXER) { if (++view >= views()) { page = PG_INPUT; view = 0; } } else if (page == PG_INPUT) { page = PG_MIXER; view = 0; } else { page = PG_MIXER; view = 0; } }
    if (down & B(BTN_FX)) open_page(PG_MASTER);
    if (down & B(BTN_ENV)) open_page(PG_RESON);
    if (down & B(BTN_SEQ)) open_page(PG_BOUNCE);
    if (down & B(BTN_LFO)) open_page(PG_ROBOTS);
    if (down & B(BTN_GLO)) open_page(PG_SETTINGS);
    if ((up & B(BTN_HOME)) && g_rec >= 0) gesture_end();

    /* the transport: PLAY and REC act at once (punch-ins land where they're pressed) */
    if (down & B(BTN_PLAY)) {
        if (save_held && page == PG_SAVE) { *arg = slot_sel; req = HOST_SAVE; used_mod = 1; }
        else if (save_held) learn_arm(1, ACT_PLAY, "PLAY: SEND A NOTE");
        else if (clr_held) { vsc_event(EV_ERASE, 0, 0); vsc_ui_notice("TAPE ERASED", 1100); used_mod = 1; }
        else vsc_event(EV_PLAY, 0, 0);
    }
    if (down & B(BTN_REC)) {
        if (save_held) learn_arm(1, ACT_REC, "REC: SEND A NOTE");
        else if (page == PG_BOUNCE) {
            int e = vsc_bounce_plan(0, 0);
            if (vsc_meter.bounce) vsc_ui_notice("BOUNCING...", 900);
            else if (e == BE_NOLOOP) vsc_ui_notice("RECORD A LOOP FIRST", 1300);
            else if (e == BE_EMPTY) vsc_ui_notice("PICK TRACKS: KEYS F3-B3", 1300);
            else if (e == BE_NOROOM) vsc_ui_notice("NO FREE TRACK: CLEAR ONE", 1500);
            else { vsc_event(EV_BOUNCE, 0, 0); vsc_ui_notice("BOUNCING", 900); }
        } else if (page == PG_SAVE) {
            if (vsc_host.slot_used[slot_sel]) { *arg = slot_sel; req = HOST_LOAD; } else { *arg = slot_sel; req = HOST_SAVE; }
        } else if (page == PG_SETTINGS && view == 1) {
            if (vsc_host.ble_count) { *arg = ble_sel; req = HOST_BLE_CONNECT; } else req = HOST_BLE_SCAN;
        } else {
            unsigned arm = 0;
            for (unsigned t = 0; t < VSC_TRACKS; ++t) arm |= vsc.p[P_TRK(t, S_ARM)] << t;
            if (!arm && !vsc_meter.recording && vsc_meter.transport != TR_FIRST) vsc_ui_notice("ARM A TRACK: KEYS F3-B3", 1300);
            else vsc_event(EV_REC, 0, 0);
        }
    }

    /* SELECT: the view; ALGORITHM: RESON's place, the robot; PRESETS: the page's own */
    if (in->enc[0]) view = (unsigned)clampi((int)view + in->enc[0], 0, (int)views() - 1);
    if (in->enc[1]) {
        int d = in->enc[1];
        if (page == PG_RESON) vsc.p[P_RTGT] = (uint8_t)clampi((int)vsc.p[P_RTGT] + d, 0, RT_INPUT);
        else if (page == PG_TAPE || page == PG_MIXER) { sel = (unsigned)clampi((int)sel + d, 0, VSC_TRACKS - 1); if (page == PG_TAPE) view = 1; }
        else if (page == PG_ROBOTS) view = (unsigned)clampi((int)view + d, 0, VSC_ROBOTS);
    }
    if (in->enc[6]) {
        int d = in->enc[6];
        switch (page) {
        case PG_TAPE: vsc.p[P_TAPESPD] = (uint8_t)clampi((int)vsc.p[P_TAPESPD] + d, 0, TS_LO); break;
        case PG_INPUT: vsc.p[P_IREC] = (uint8_t)clampi((int)vsc.p[P_IREC] + d, 0, IREC_REPLACE); break;
        case PG_BOUNCE: vsc.p[P_BDST] = (uint8_t)clampi((int)vsc.p[P_BDST] + d, 0, 4); break;
        case PG_RESON: rsel = (unsigned)clampi((int)rsel + d, 0, VSC_RBANDS - 1); break;
        case PG_ROBOTS:
            if (view < VSC_ROBOTS) vsc.p[P_ROB(view, R_TARGET)] = (uint8_t)clampi((int)vsc.p[P_ROB(view, R_TARGET)] + d, 0, VSC_RTARGETS - 1);
            else learn_sel = (unsigned)clampi((int)learn_sel + d, 0, VSC_LEARN - 1);
            break;
        case PG_SAVE: slot_sel = (unsigned)clampi((int)slot_sel + d, 0, VSC_PROJECTS - 1); break;
        case PG_SETTINGS: ble_sel = (unsigned)clampi((int)ble_sel + d, 0, vsc_host.ble_count ? vsc_host.ble_count - 1 : 0); break;
        default: break;
        }
    }
    /* KNOB 1-4. Held: SAVE learns it from MIDI, HOME records a robot gesture of it. */
    for (unsigned e = 2; e < 6; ++e) {
        int d = in->enc[e];
        if (!d) continue;
        unsigned s = slot(e - 2);
        touch_at[e - 2] = now_ms | 1u;
        if (save_held && s < VSC_NP) { learn_arm(0, s, "MOVE A MIDI CC"); continue; }
        if (home_held && s < VSC_NP && g_rec < 0) { used_mod = 1; gesture_start(s); }
        if (s != K_NONE) edit(s, d, &req, arg);
    }
    gesture_sample();

    /* keys: the four lowest white keys are the tracks: arm (on BOUNCE: pick the sources;
     * with HOME: clear); on RESON the next eight white keys pick a band; the others play
     * RESON */
    for (unsigned kk = 0; kk < 27; ++kk) {
        if (!(kdown & B(kk))) continue;
        unsigned k = key_track(kk), band = key_band(kk);
        if (k < VSC_TRACKS) {
            if (save_held) { learn_arm(1, ACT_ARM1 + k, "ARM: SEND A NOTE"); continue; }
            if (clr_held) { vsc_event(EV_CLEAR, k, 0); say("CLEARED TRACK ", (const char[]){(char)('1' + k), 0}, 1000); used_mod = 1; continue; }
            if (home_held) { sel = k; used_mod = 1; if (page == PG_TAPE) view = 1; say("TRACK ", (const char[]){(char)('1' + k), 0}, 700); continue; }
            if (page == PG_BOUNCE) { vsc.p[P_BSRC] ^= (uint8_t)(1u << k); continue; }
            vsc_event(EV_ARM, k, 0);
            continue;
        }
        if (page == PG_RESON && band < VSC_RBANDS) { rsel = band; if (view == 0) view = 1; continue; }
        vsc_event(EV_NOTE_ON, 53 + kk, 100);
    }
    apply_knob_cc();
    prev_buttons = in->buttons;
    prev_keys = in->keys;
    return req;
}

/* ------------------------------------------------------------- drawing */
static void header(const char *title, const char *badge, uint16_t bg, uint16_t fg)
{
    if (!IN_BAND(0, 15)) return;
    rect(0, 0, 240, 14, C_PANEL); hline(0, 14, 240, C_LINE);
    text(5, 4, title, C_CREAM);
    if (badge && *badge) { int w = text_w(badge) + 10; box(235 - w, 2, w, 11, bg); text(240 - w, 4, badge, fg); }
}
static void footer(const char *hint)
{
    if (!IN_BAND(229, 240)) return;
    rect(0, 229, 240, 11, C_PANEL);
    text(5, 231, hint, C_DIM);
}
/* a knob: aluminium, its pointer from -135 to 135 degrees */
static void knob(int cx, int cy, int r, float f, int lit)
{
    if (!IN_BAND(cy - r - 2, cy + r + 2)) return;
    if (lit) disc(cx, cy, r + 2, C_GLOW);
    disc(cx, cy, r, C_ALU); disc(cx, cy, r - 3, C_ALU2);
    float a = (-135.f + 270.f * (f < 0.f ? 0.f : f > 1.f ? 1.f : f)) * (3.1415927f / 180.f);
    int x1 = cx + (int)(fsin(a) * (float)(r - 2) + 0.5f * (fsin(a) > 0 ? 1 : -1)), y1 = cy - (int)(fcos(a) * (float)(r - 2) + 0.5f * (fcos(a) > 0 ? 1 : -1));
    thick(cx, cy, x1, y1, C_BODY);
}
/* the four knob cells: knob, name, value */
static void knob_row(int y)
{
    if (!IN_BAND(y, y + 52)) return;
    hline(0, y, 240, C_DEEP);
    for (unsigned k = 0; k < 4; ++k) {
        unsigned s = slot(k);
        int cx = 30 + 60 * (int)k;
        if (s == K_NONE) continue;
        char n[10], v[10]; slot_text(s, n, v);
        int recent = touch_at[k] && now_ms - touch_at[k] < 600u;
        knob(cx, y + 18, 12, slot_frac(s), recent);
        text_c(cx, y + 35, n, C_DIM);
        text_c(cx, y + 45, v, recent ? C_AMBER : C_CREAM);
    }
}
/* a VU reading (0 VU: a sine at -6 dBFS) to where the needle sits, 0..1: by amplitude
 * from -20 to +3 dB, so the scale opens out toward 0 as a real VU's does */
static float vu_pos(float v) { float a = v * (1.f / 0.318f); return (a - 0.1f) * (1.f / 1.3125f); }
static float db_pos(int db) { float a = 1.f; int d = db; while (d > 0) { a *= 1.122018f; --d; } while (d < 0) { a *= 0.891251f; ++d; } return (a - 0.1f) * (1.f / 1.3125f); }
/* a VU meter: face, scale, red zone, needle */
static void vu(int x, int y, int w, int h, float v, int big_one)
{
    if (!IN_BAND(y, y + h)) return;
    box(x, y, w, h, C_AMBER); box(x + 2, y + 2, w - 4, h - 4, C_FACE);
    int px0 = x + w / 2, py0 = y + h - 3, R = h - 9;
    if (big_one) R = h - 14;
    /* the scale: -20 .. +3 dB over -48 .. +48 degrees */
    static const int8_t marks[] = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3};
    for (unsigned i = 0; i < sizeof marks; ++i) {
        float a = (-48.f + 96.f * db_pos(marks[i])) * (3.1415927f / 180.f);
        int len = marks[i] % 5 == 0 || marks[i] == 0 ? 4 : 2;
        uint16_t c = marks[i] > 0 ? C_RED : C_BROWN;
        line(px0 + (int)(fsin(a) * (float)R), py0 - (int)(fcos(a) * (float)R), px0 + (int)(fsin(a) * (float)(R - len)), py0 - (int)(fcos(a) * (float)(R - len)), c);
        if (marks[i] > 0) line(px0 + (int)(fsin(a) * (float)(R + 1)), py0 - (int)(fcos(a) * (float)(R + 1)), px0 + (int)(fsin(a) * (float)(R - 1)), py0 - (int)(fcos(a) * (float)(R - 1)), c);
    }
    if (big_one) {
        static const int8_t lab[] = {-20, -7, -3, 0, 3};
        for (unsigned i = 0; i < sizeof lab; ++i) {
            float a = (-48.f + 96.f * db_pos(lab[i])) * (3.1415927f / 180.f);
            char b[6]; if (lab[i] > 0) { b[0] = '+'; num(b + 1, lab[i]); } else num(b, lab[i]);
            text_c(px0 + (int)(fsin(a) * (float)(R + 8)), py0 - (int)(fcos(a) * (float)(R + 8)) - 3, b, lab[i] > 0 ? C_RED : C_BROWN);
        }
    }
    text(x + 5, y + 4, "VU", C_BROWN);
    float f = vu_pos(v); f = f < -0.03f ? -0.03f : f > 1.06f ? 1.06f : f;
    float a = (-48.f + 96.f * f) * (3.1415927f / 180.f);
    line(px0, py0, px0 + (int)(fsin(a) * (float)(R + 2)), py0 - (int)(fcos(a) * (float)(R + 2)), C_BLACK);
    rect(px0 - 4, py0, 9, 2, C_BROWN);
}

/* ---- TAPE */
static void reel(int cx, int cy, int r, float ang)
{
    disc(cx, cy, r, C_TAPE); ring(cx, cy, r, C_TAPE2);
    if (r > 20) ring(cx, cy, r - 7, C_TAPE3);
    disc(cx, cy, 13, C_HUB); ring(cx, cy, 13, C_HUB2); disc(cx, cy, 5, C_BLACK);
    for (unsigned k = 0; k < 3; ++k) {
        float a = ang + (float)k * 2.0943951f;
        thick(cx + (int)(fsin(a) * 6.f), cy - (int)(fcos(a) * 6.f), cx + (int)(fsin(a) * 11.f), cy - (int)(fcos(a) * 11.f), C_BLACK);
    }
}
static float reel_ang;
static uint32_t reel_pos;
static void draw_tape(void)
{
    char b[24];
    if (view == 1) { cpy(b, "TRACK "); char c[2] = {(char)('1' + sel), 0}; cat(b, c); }
    header("VISCERA 4-TRACK", view == 1 ? b : "TAPE", C_AMBER, C_BODY);
    /* the cassette window and its reels: the tape winds from left to right over the loop */
    if (IN_BAND(18, 118)) {
        box(6, 19, 228, 98, C_BLACK); frame(6, 19, 228, 98, C_LINE);
        hline(10, 23, 220, C_PANEL);
        uint32_t e = vsc_meter.loop ? vsc_meter.loop : vsc_meter.cap;
        float f = e ? (float)vsc_meter.pos / (float)e : 0.f;
        int r1 = 14 + (int)(22.f * (1.f - f)), r2 = 14 + (int)(22.f * f);
        reel(62, 64, r1, reel_ang); reel(178, 64, r2, reel_ang * 1.3f);
        line(62 - r1 / 3, 64 + r1, 90, 107, C_TAPE3); line(62 - r1 / 3 + 1, 64 + r1, 91, 107, C_TAPE3);
        hline(90, 107, 60, C_TAPE3); hline(90, 108, 60, C_TAPE3);
        line(150, 107, 178 + r2 / 3, 64 + r2, C_TAPE3); line(150, 108, 178 + r2 / 3 + 1, 64 + r2, C_TAPE3);
        disc(90, 107, 3, C_HUB2); disc(150, 107, 3, C_HUB2);
        box(110, 101, 20, 12, C_ALU); vline(119, 101, 12, C_HUB2);
        if (vsc_meter.recording || vsc_meter.transport == TR_FIRST) disc(120, 96, 2, C_RED);
        /* the counter: tenths of a second */
        box(99, 30, 42, 15, C_DARK); frame(99, 30, 42, 15, C_LINE);
        int ts = (int)((float)vsc_meter.pos / tape_hz() * 10.f) % 1000;
        for (unsigned d = 0; d < 3; ++d) {
            int x = 102 + 13 * (int)d; rect(x, 32, 11, 11, C_BLACK);
            char c[2] = {(char)('0' + (d == 0 ? ts / 100 : d == 1 ? ts / 10 % 10 : ts % 10)), 0};
            text(x + 3, 34, c, C_CREAM);
        }
        if (vsc_meter.transport == TR_FIRST) cpy(b, "FIRST");
        else if (vsc_meter.loop) secs(b, (float)vsc_meter.loop / tape_hz());
        else cpy(b, "EMPTY");
        text_c(120, 49, b, vsc_meter.transport == TR_FIRST ? C_RED : C_DIM);
    }
    /* the transport lamps and the TAPE SPEED switch */
    if (IN_BAND(120, 136)) {
        int rec_on = vsc_meter.recording || vsc_meter.transport == TR_FIRST;
        box(6, 121, 50, 14, C_PANEL); disc(14, 128, 3, rec_on ? C_RED : C_REDK); text(21, 125, "REC", rec_on ? C_CREAM : C_DIM);
        box(60, 121, 56, 14, C_PANEL);
        for (int i = 0; i < 7; ++i) vline(66 + i, 125 + i / 2, 7 - i / 2 * 2, vsc_meter.playing ? C_AMBER : C_DIMR);
        text(76, 125, "PLAY", vsc_meter.playing ? C_CREAM : C_DIM);
        box(120, 121, 46, 14, C_PANEL); rect(126, 125, 6, 6, vsc_meter.playing ? C_DIMR : C_CREAM); text(136, 125, "STOP", vsc_meter.playing ? C_DIM : C_CREAM);
        box(170, 121, 64, 14, C_PANEL); frame(170, 121, 64, 14, C_LINE);
        text(175, 125, "SPD", C_DIM);
        vsc_param_label(P_TAPESPD, vsc.p[P_TAPESPD], b); text(199, 125, b, C_AMBER);
    }
    /* the four tracks */
    if (IN_BAND(139, 173)) for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        int x = 6 + 58 * (int)t, w = 54;
        int armed = vsc.p[P_TRK(t, S_ARM)], recn = vsc_meter.recording >> t & 1u;
        box(x, 140, w, 32, C_DARK);
        if (armed) frame(x, 140, w, 32, recn ? C_RED : C_REDK);
        char c[2] = {(char)('1' + t), 0};
        if (t == sel) { box(x + 3, 142, 9, 11, view == 1 ? C_AMBER : C_DEEP); }
        text(x + 5, 144, c, t == sel && view == 1 ? C_BODY : vsc_meter.has[t] ? C_CREAM : C_DIM);
        disc(x + w - 8, 148, 3, armed ? (recn && (now_ms / 200u) & 1u ? C_REDK : C_RED) : C_REDK);
        if (armed) text(x + 13, 144, recn ? "REC" : "ARM", C_RED);
        else if (vsc.p[P_TRK(t, S_MUTE)]) text(x + 13, 144, "MUTE", C_DIM);
        if (vsc_meter.has[t]) {
            /* its shape over the loop, and where the tape is */
            for (int i = 0; i < 46; ++i) { unsigned v = vsc_meter.overview[t][i * 60 / 46] / 3u; vline(x + 4 + i, 160 - (int)v, (int)v * 2 + 1, C_HUB2); }
            uint32_t e = vsc_meter.loop ? vsc_meter.loop : vsc_meter.cap;
            int ph = e ? (int)((float)vsc_meter.tpos[t] * 46.f / (float)e) : 0;
            vline(x + 4 + ph, 154, 13, C_AMBER);
            float lv = vsc_meter.pk[t]; int lw = (int)(lv * 46.f); lw = lw > 46 ? 46 : lw;
            rect(x + 4, 168, lw, 2, lv > 0.85f ? C_AMBER : C_GREEN);
        } else text(x + 6, 158, "EMPTY", C_DIMR);
    }
    knob_row(177);
    footer(view == 1 ? "HOME+KEY OR ALGO: TRACK" : vsc_meter.loop ? "KEYS F3-B3: ARM  REC: PUNCH" : "KEYS F3-B3: ARM  REC: FIRST TAKE");
}

/* ---- MIXER */
static void draw_mixer(void)
{
    static const char *const row_name[3] = {"LEVEL", "TRIM", "PAN"};
    header("MIXER", row_name[view % 3], C_DEEP, C_AMBER);
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        int x = 4 + 59 * (int)t, cx = x + 27;
        int armed = vsc.p[P_TRK(t, S_ARM)], has = vsc_meter.has[t];
        if (IN_BAND(18, 226)) {
            box(x, 18, 55, 208, C_DARK);
            if (armed) frame(x, 18, 55, 208, (vsc_meter.recording >> t & 1u) ? C_RED : C_REDK);
        }
        vu(x + 4, 22, 47, 34, has ? vsc_meter.vu[t] : 0.f, 0);
        if (IN_BAND(60, 70)) {
            const char *src = (vsc_meter.recording >> t & 1u) ? "REC" : armed ? "ARMED" : has ? "TAPE" : "EMPTY";
            text_c(cx, 61, src, (vsc_meter.recording >> t & 1u) ? C_RED : has ? C_CREAM : C_DIMR);
        }
        unsigned row = view % 3;
        knob(cx, 82, 9, (float)vsc.p[P_TRK(t, S_TRIM)] / 127.f, row == 1);
        disc(x + 47, 73, 2, vsc_meter.sat[t] > 40 ? C_AMBER : C_GLOW);
        if (vsc_meter.sat[t] > 40) ring(x + 47, 73, 4, C_GLOW);
        if (IN_BAND(94, 102)) text_c(cx, 95, "TRIM", row == 1 ? C_AMBER : C_DIM);
        knob(cx, 114, 7, (float)vsc.p[P_TRK(t, S_PAN)] / 127.f, row == 2);
        if (IN_BAND(124, 132)) text_c(cx, 125, "PAN", row == 2 ? C_AMBER : C_DIM);
        if (IN_BAND(136, 200)) {
            rect(cx - 1, 138, 3, 60, C_BLACK);
            for (int k = 0; k <= 4; ++k) hline(cx - 9, 138 + k * 15, 4, C_DEEP), hline(cx + 6, 138 + k * 15, 4, C_DEEP);
            int fy = 138 + (int)((1.f - (float)vsc.p[P_TRK(t, S_LEVEL)] / 127.f) * 52.f);
            box(cx - 10, fy, 21, 9, row == 0 ? C_AMBER : C_ALU2); hline(cx - 10, fy + 4, 21, C_BODY);
        }
        if (IN_BAND(203, 221)) {
            box(x + 8, 204, 39, 16, armed ? C_REDK : C_DEEP);
            disc(x + 16, 212, 3, armed ? C_RED : C_REDK);
            char c[2] = {(char)('1' + t), 0};
            text(x + 23, 209, c, armed ? C_CREAM : C_DIM);
            if (vsc.p[P_TRK(t, S_MUTE)]) text(x + 30, 209, "M", C_AMBER);
        }
    }
    footer(view == 0 ? "KNOBS 1-4: LEVELS  EDIT: NEXT" : view == 1 ? "KNOBS 1-4: TRIM INTO PREAMP" : "KNOBS 1-4: PAN  EDIT: INPUT");
}

/* ---- INPUT */
static void draw_input(void)
{
    char b[24];
    unsigned to = 4;
    for (unsigned t = 0; t < VSC_TRACKS && to == 4; ++t) if (vsc.p[P_TRK(t, S_ARM)]) to = t;
    if (to < 4) { cpy(b, "TO TRK "); char c[2] = {(char)('1' + to), 0}; cat(b, c); }
    else cpy(b, "NOT ARMED");
    header("INPUT", b, to < 4 ? C_REDK : C_DEEP, C_CREAM);
    if (to < 4 && IN_BAND(3, 12)) disc(240 - text_w(b) - 15, 7, 2, C_RED);
    if (IN_BAND(20, 62)) {
        text(6, 21, "SOURCE", C_DIM);
        static const char *const nm[3] = {"BT", "USB", "BOTH"};
        for (unsigned s = 0; s < 3; ++s) {
            int x = 6 + 78 * (int)s, sel = vsc.p[P_ISRC] == s;
            box(x, 31, 72, 30, sel ? C_AMBER : C_PANEL);
            if (!sel) frame(x, 31, 72, 30, C_LINE);
            text(x + 6, 36, nm[s], sel ? C_BODY : C_CREAM);
            const char *st; int live;
            if (s == 0) { st = !vsc_host.bt_audio ? "BT OFF" : vsc_host.bt_audio_playing ? "PLAYING" : vsc_host.bt_audio_connected ? "LINKED" : "NO LINK"; live = vsc_meter.bt_in; }
            else if (s == 1) { st = vsc_host.usb_connected ? "HOST" : "NO HOST"; live = vsc_meter.usb_in; }
            else { st = "MIXED"; live = vsc_meter.bt_in || vsc_meter.usb_in; }
            text(x + 6, 50, st, sel ? C_BROWN : C_DIMR);
            disc(x + 64, 40, 3, live ? (sel ? C_LED : C_GREEN) : C_DEEP);
        }
    }
    if (IN_BAND(66, 146)) {
        box(6, 67, 228, 78, C_TAPE);
        vu(10, 71, 220, 70, vsc_meter.vu_in, 1);
        int ol = vsc_meter.pk_in > 0.92f, hot = vsc_meter.sat_in > 40;
        text(195, 77, "OL", C_BROWN);
        disc(216, 80, 3, ol ? C_RED : hot ? C_AMBER : C_GLOW);
    }
    knob(34, 166, 13, (float)vsc.p[P_ITRIM] / 127.f, 0);
    knob(94, 166, 13, (float)vsc.p[P_IDRIVE] / 127.f, vsc_meter.sat_in > 40);
    if (IN_BAND(150, 202)) {
        text_c(34, 186, "TRIM", C_DIM); vsc_param_label(P_ITRIM, vsc.p[P_ITRIM], b); text_c(34, 196, b, C_CREAM);
        text_c(94, 186, "DRIVE", C_DIM); vsc_param_label(P_IDRIVE, vsc.p[P_IDRIVE], b); text_c(94, 196, b, vsc_meter.sat_in > 40 ? C_AMBER : C_CREAM);
        box(128, 152, 106, 20, C_PANEL); text(134, 159, "MONITOR", C_DIM);
        vsc_param_label(P_IMON, vsc.p[P_IMON], b); text_r(228, 159, b, C_CREAM);
        box(128, 176, 106, 20, C_PANEL); text(134, 183, "REC MODE", C_DIM);
        vsc_param_label(P_IREC, vsc.p[P_IREC], b); text_r(228, 183, b, C_CREAM);
    }
    if (IN_BAND(205, 224)) {
        box(6, 206, 228, 18, C_DARK);
        static const char *const nm[3] = {"BT", "USB", "BT+USB"};
        cpy(b, nm[vsc.p[P_ISRC] % 3]); cat(b, " IN > TAPE ");
        char r[8]; num(r, (int)(tape_hz() / 1000.f + 0.5f)); cat(b, r); cat(b, "K");
        text(12, 211, b, C_CREAM);
    }
    footer("K1 SOURCE  K4 MONITOR  PRE: MODE");
}

/* ---- BOUNCE */
static void draw_bounce(void)
{
    char b[24];
    unsigned src = 0, dst = 0;
    int e = vsc_bounce_plan(&src, &dst);
    header("BOUNCE", vsc_meter.bounce ? "PRINTING" : e ? "" : "READY", vsc_meter.bounce ? C_RED : C_DEEP, vsc_meter.bounce ? C_BODY : C_AMBER);
    if (IN_BAND(20, 124)) {
        text(6, 21, "FROM", C_DIM);
        for (unsigned t = 0; t < VSC_TRACKS; ++t) {
            int y = 30 + 21 * (int)t, pick = vsc.p[P_BSRC] >> t & 1u, has = vsc_meter.has[t];
            box(6, y, 40, 18, pick ? (has ? C_AMBER : C_GLOW) : C_PANEL);
            char c[2] = {(char)('1' + t), 0}; text(12, y + 6, c, pick ? C_BODY : has ? C_CREAM : C_DIMR);
            if (!has) text(20, y + 6, "--", pick ? C_BODY : C_DIMR);
            if (pick) { hline(46, y + 9, 12, C_HUB); }
        }
        vline(58, 39, 64, C_HUB); hline(58, 68, 12, C_HUB);
        int ron = vsc.p[P_BRESON];
        box(70, 56, 50, 26, C_PANEL); frame(70, 56, 50, 26, ron ? C_AMBER : C_LINE);
        text(76, 61, "RESON", ron ? C_CREAM : C_DIMR);
        vsc_param_label(P_RPROF, vsc.p[P_RPROF], b); text(76, 72, ron ? b : "BYPASS", ron ? C_AMBER : C_DIMR);
        hline(120, 68, 10, C_HUB);
        box(130, 56, 40, 26, C_PANEL); frame(130, 56, 40, 26, C_LINE);
        text(137, 61, "SAT", C_CREAM); vsc_param_label(P_BDRIVE, vsc.p[P_BDRIVE], b); text(135, 72, b, C_DIM);
        hline(170, 68, 14, C_HUB);
        box(184, 50, 50, 38, e ? C_PANEL : C_REDK); frame(184, 50, 50, 38, e ? C_LINE : C_RED);
        cpy(b, "TO ");
        char c[2] = {(char)('1' + (e ? 0 : dst)), 0}; if (e) c[0] = '-'; cat(b, c);
        text(190, 56, b, C_CREAM);
        if (!e) disc(225, 59, 3, vsc_meter.bounce ? C_RED : C_REDK);
        text(190, 74, vsc.p[P_BDST] ? "FIXED" : "FREE", C_DIM);
        const char *why = e == BE_NOLOOP ? "RECORD A LOOP FIRST" : e == BE_EMPTY ? "PICK TRACKS: KEYS F3-B3" : e == BE_NOROOM ? "NO FREE TRACK" :
                          vsc.p[P_BCLEAR] ? "SOURCES CLEARED AFTER" : "SOURCES KEPT";
        text(64, 100, why, e ? C_AMBER : C_DIMR);
    }
    if (IN_BAND(126, 170)) {
        box(6, 128, 228, 40, C_BLACK); frame(6, 128, 228, 40, C_LINE);
        float f = (float)vsc_meter.bounce_pct / 100.f;
        disc(30, 148, 8 + (int)(8.f * (1.f - f)), C_TAPE); disc(30, 148, 7, C_HUB); disc(30, 148, 3, C_BLACK);
        disc(210, 148, 8 + (int)(8.f * f), C_TAPE); disc(210, 148, 7, C_HUB); disc(210, 148, 3, C_BLACK);
        rect(52, 144, 136, 8, C_DARK);
        rect(52, 144, (int)(136.f * f), 8, C_RED);
        num(b, vsc_meter.bounce_pct); cat(b, "%");
        text_c(120, 134, vsc_meter.bounce ? b : "REC: PRINT ONE LOOP", vsc_meter.bounce ? C_CREAM : C_DIM);
    }
    knob_row(174);
    footer(view == 0 ? "F3-B3: FROM  PRE: TO" : "K1 TO  K2 RESON IN/OUT");
}

/* ---- RESON */
static void draw_reson(void)
{
    char b[24];
    unsigned tg = vsc.p[P_RTGT];
    vsc_param_label(P_RTGT, tg, b);
    if (tg) { char c[24]; cpy(c, "ON "); cat(c, b); cpy(b, c); }
    header("RESON", b, tg ? C_DEEP : C_PANEL, tg ? C_AMBER : C_DIM);
    if (IN_BAND(18, 168)) {
        box(6, 19, 228, 148, C_DARK);
        for (unsigned k = 0; k < VSC_RBANDS; ++k) {
            int x = 18 + 27 * (int)k;
            int lit = vsc_meter.rb_level[k] > 30;
            rect(x - 2, 24, 5, 5, lit ? C_AMBER : C_GLOW);
            rect(x, 34, 2, 106, C_BLACK);
            int y = 34 + (int)((1.f - (float)vsc.p[P_RG0 + k] / 127.f) * 98.f);
            box(x - 6, y, 14, 8, k == rsel ? C_AMBER : C_ALU2); hline(x - 6, y + 3, 14, C_BODY);
            /* the band's ring, as a meter */
            int lv = vsc_meter.rb_level[k] * 98 / 255;
            if (lv) rect(x + 9, 140 - lv, 2, lv, C_TAPE3);
        }
        text(10, 146, "200", C_DIM); text(94, 146, "1K", C_DIM); text(196, 146, "4K8", C_DIM);
        text(10, 157, "HZ", C_DIMR);
        if (!tg) text(60, 157, "ALGO: PUT IT ON", C_AMBER);
        else { vsc_param_label(P_RPROF, vsc.p[P_RPROF], b); text(60, 157, b, C_AMBER); }
    }
    knob_row(172);
    footer("ALGO: WHERE  KEYS C4-C5: BAND");
}

/* ---- ROBOTS & MIDI LEARN */
static void robot_box(unsigned r, int x)
{
    char b[24], n[8];
    int sel = view == r;
    box(x, 19, 112, 70, C_DARK); if (sel) frame(x, 19, 112, 70, C_AMBER);
    vsc_target_name(vsc.p[P_ROB(r, R_TARGET)], n);
    cpy(b, r ? "R2 " : "R1 "); cat(b, n);
    text(x + 5, 23, b, sel ? C_AMBER : C_CREAM);
    if (IN_BAND(34, 62)) {
        rect(x + 5, 35, 102, 26, C_BLACK);
        unsigned sh = vsc.p[P_ROB(r, R_SHAPE)];
        float sk = (float)vsc.p[P_ROB(r, R_SKEW)] / 127.f; sk = sk < 0.03f ? 0.03f : sk > 0.97f ? 0.97f : sk;
        int py = 0;
        for (int i = 0; i < 102; ++i) {
            float ph = (float)i / 102.f * 2.f; ph -= (float)(int)ph;
            float w = ph < sk ? 0.5f * ph / sk : 0.5f + 0.5f * (ph - sk) / (1.f - sk), o;
            switch (sh) {
            case RS_SINE: o = fsin(w * 6.2831853f); break;
            case RS_TRI: o = w < 0.5f ? 4.f * w - 1.f : 3.f - 4.f * w; break;
            case RS_SAW: o = 2.f * w - 1.f; break;
            case RS_SQUARE: o = ph < sk ? 1.f : -1.f; break;
            case RS_SH: o = (float)((i / 13 * 37) % 9) / 4.f - 1.f; break;
            case RS_DRIFT: o = fsin((float)i * 0.07f) * 0.6f + fsin((float)i * 0.023f) * 0.4f; break;
            default: o = (float)vsc.gest[r][(unsigned)i * (VSC_GEST - 1u) / 101u] / 127.f; break;
            }
            int y = 48 - (int)(o * 11.f);
            if (i) line(x + 4 + i, py, x + 5 + i, y, sel ? C_AMBER : C_HUB);
            py = y;
        }
        int dep = vsc.p[P_ROB(r, R_DEPTH)];
        int my = 48 - (int)(vsc_meter.robot[r] * 11.f * (dep ? 1.f : 0.f));
        if (dep) disc(x + 106, my, 2, C_CREAM);
    }
    vsc_param_label(P_ROB(r, R_RATE), vsc.p[P_ROB(r, R_RATE)], b); text(x + 5, 66, "RATE", C_DIM); text(x + 44, 66, vsc.p[P_ROB(r, R_SHAPE)] == RS_GESTURE ? "REC" : b, C_CREAM);
    vsc_param_label(P_ROB(r, R_SHAPE), vsc.p[P_ROB(r, R_SHAPE)], b); text(x + 5, 77, b, C_DIM);
    vsc_param_label(P_ROB(r, R_DEPTH), vsc.p[P_ROB(r, R_DEPTH)], n); cpy(b, "D "); cat(b, n); text(x + 44, 77, b, C_CREAM);
}
static void draw_robots(void)
{
    header("ROBOTS + LEARN", vsc_learn_armed ? "LISTENING" : "", C_AMBER, C_BODY);
    if (IN_BAND(18, 90)) { robot_box(0, 6); robot_box(1, 122); }
    if (IN_BAND(94, 164)) {
        text(6, 96, "MIDI LEARN", view == VSC_ROBOTS ? C_AMBER : C_DIM);
        unsigned first = learn_sel > 2 ? learn_sel - 2 : 0;
        for (unsigned i = 0; i < 3; ++i) {
            unsigned l = first + i;
            int y = 106 + 19 * (int)i;
            const struct vsc_learn *e = &vsc_dev.learn[l];
            int sel = view == VSC_ROBOTS && l == learn_sel;
            box(6, y, 228, 16, sel ? C_TAPE : C_DARK); if (sel) frame(6, y, 228, 16, C_AMBER);
            char b[24], c[8];
            if (e->param == 255) { num(c, (int)l + 1); cpy(b, c); cat(b, "  --"); text(11, y + 5, b, C_DIMR); continue; }
            cpy(b, e->kind ? "NOTE " : "CC "); num(c, e->cc); cat(b, c);
            text(11, y + 5, b, C_CREAM); text(76, y + 5, ">", C_DIM);
            if (e->kind) cpy(b, e->param < ACT_COUNT ? act_name[e->param] : "?");
            else {
                unsigned p = e->param;
                if (p >= P_STRIP && p < P_BSRC) { c[0] = 'T'; c[1] = (char)('1' + (p - P_STRIP) / VSC_TP); c[2] = ' '; c[3] = 0; cpy(b, c); cat(b, vsc_param_info(p)->name); }
                else cpy(b, vsc_param_info(p)->name);
            }
            text(88, y + 5, b, C_CREAM);
        }
    }
    knob_row(170);
    footer(view < VSC_ROBOTS ? "PRE: TARGET  HOME+TURN: GESTURE" : "SAVE+TURN OR KEY: LEARN");
}

/* ---- PROJECTS */
static void draw_save(void)
{
    header("PROJECTS", "DESK ONLY", C_DEEP, C_DIM);
    for (unsigned s = 0; s < VSC_PROJECTS; ++s) {
        int y = 22 + 24 * (int)s;
        if (!IN_BAND(y, y + 20)) continue;
        int sel = s == slot_sel;
        box(6, y, 228, 20, sel ? C_TAPE : C_DARK); if (sel) frame(6, y, 228, 20, C_AMBER);
        char b[8]; num(b, (int)s + 1);
        text(14, y + 7, b, C_DIM);
        text(34, y + 7, vsc_host.slot_used[s] ? vsc_host.slot_name[s] : "EMPTY", vsc_host.slot_used[s] ? C_CREAM : C_DIMR);
        if (sel) text_r(226, y + 7, vsc_host.slot_used[s] ? "REC LOAD" : "REC SAVE", C_AMBER);
    }
    if (IN_BAND(214, 226)) text(6, 216, "SAVE+PLAY: SAVE OVER IT", C_DIM);
    footer("PRESETS: SLOT  TAPE STAYS IN RAM");
}

/* ---- SETTINGS */
static void draw_settings(void)
{
    char b[24], c[8];
    header("SETTINGS", VSC_VERSION, C_DEEP, C_DIM);
    if (IN_BAND(18, 110)) {
        text(6, 21, view == 1 ? "BLUETOOTH MIDI DEVICES" : "PANEL", C_DIM);
        if (view == 1) {
            if (!vsc_host.ble_count) text(10, 36, "REC: SCAN", C_CREAM);
            for (unsigned i = 0; i < vsc_host.ble_count && i < 5; ++i) {
                int y = 32 + 15 * (int)i;
                box(6, y, 228, 13, i == ble_sel ? C_TAPE : C_DARK);
                text(10, y + 3, vsc_host.ble_name[i][0] ? vsc_host.ble_name[i] : "?", i == ble_sel ? C_AMBER : C_CREAM);
            }
        } else {
            /* the load, per part: the tape and desk, RESON, the rest */
            static const char *const nm[4] = {"TAPE+DESK", "RESON", "OTHER", "SYSTEM"};
            for (unsigned i = 0; i < 4; ++i) {
                int y = 34 + 14 * (int)i;
                text(10, y, nm[i], C_DIM);
                unsigned v = i < 2 ? vsc_host.part[i] : i == 2 ? vsc_host.part[2] : vsc_host.part[VSC_PARTS];
                rect(84, y, 100, 7, C_DARK); rect(84, y, (int)(v > 100u ? 100u : v), 7, v > 60u ? C_AMBER : C_GREEN);
                num(c, (int)v); cat(c, "%"); text(190, y, c, C_CREAM);
            }
            if (vsc_host.cpu == 255) cpy(b, "CPU: ON THE FM-1");
            else { cpy(b, "CPU "); num(c, vsc_host.cpu); cat(b, c); cat(b, "%  PEAK "); num(c, vsc_host.cpu_peak); cat(b, c); cat(b, "%"); }
            text(10, 94, b, vsc_host.cpu > 80 && vsc_host.cpu != 255 ? C_RED : C_CREAM);
            if (vsc_host.relief) text(190, 94, "EASED", C_AMBER);
        }
    }
    knob_row(174);
    footer(view == 0 ? "SELECT: BLUETOOTH MIDI" : "REC: SCAN / CONNECT");
}

/* ---- MASTER: the EQ's curve, the compressor's reduction and curve */
static int8_t eq_curve[220];          /* the response, in half-dBs, one a column */
static uint32_t eq_curve_key = ~0u;
static void eq_curve_make(void)
{
    uint32_t key = vsc_eq_live;
    for (unsigned i = 0; i < 16; ++i) key = key * 31u + vsc.p[P_EQ0 + i];
    if (key == eq_curve_key) return;
    eq_curve_key = key;
    for (int x = 0; x < 220; ++x) {
        float f = 20.f; { float e = (float)x / 219.f * 9.965784f; int i = (int)e; f = 20.f * (float)(1u << i) * (1.f + (e - (float)i) * (0.6565f + (e - (float)i) * 0.3435f)); }
        float w = 6.2831853f * f / (float)VSC_SR, c1 = fcos(w), c2 = fcos(2.f * w), s1 = fsin(w), s2 = fsin(2.f * w);
        float db = 0.f;
        for (unsigned b = 0; b < 4; ++b) {
            if (!(vsc_eq_live >> b & 1u)) continue;
            const float *k = vsc_eq_coef[b];
            float nr = k[0] + k[1] * c1 + k[2] * c2, ni = -(k[1] * s1 + k[2] * s2);
            float dr = 1.f + k[3] * c1 + k[4] * c2, di = -(k[3] * s1 + k[4] * s2);
            float m = (nr * nr + ni * ni) / (dr * dr + di * di + 1e-12f);
            /* 10 log10 m */
            int e = 0; while (m >= 2.f) { m *= 0.5f; ++e; } while (m < 1.f && e > -60) { m *= 2.f; --e; }
            db += 3.0103f * ((float)e + (m - 1.f) * (1.4425f - 0.4425f * (m - 1.f)));
        }
        db = db < -30.f ? -30.f : db > 30.f ? 30.f : db;
        eq_curve[x] = (int8_t)(db * 2.f);
    }
}
static void draw_master(void)
{
    char b[24], c[8];
    if (view < 4) {
        cpy(b, "EQ BAND "); c[0] = (char)('1' + view); c[1] = 0; cat(b, c);
        header("MASTER", b, C_DEEP, C_AMBER);
        eq_curve_make();
        if (IN_BAND(18, 168)) {
            box(6, 19, 228, 148, C_BLACK); frame(6, 19, 228, 148, C_LINE);
            /* the grid: 100 Hz, 1 kHz, 10 kHz; +-12 dB */
            static const int16_t gx[3] = {51, 124, 197};   /* (log position of 100, 1k, 10k over 20..20k) */
            for (unsigned i = 0; i < 3; ++i) vline(gx[i], 24, 138, C_PANEL);
            for (int d = -12; d <= 12; d += 6) hline(16, 93 - d * 3, 208, d ? C_PANEL : C_DEEP);
            text(gx[0] - 8, 155, "100", C_DIMR); text(gx[1] - 5, 155, "1K", C_DIMR); text(gx[2] - 8, 155, "10K", C_DIMR);
            text(10, 54, "+12", C_DIMR); text(10, 126, "-12", C_DIMR);
            int py = 0;
            for (int x = 0; x < 220; ++x) {
                int y = 93 - eq_curve[x] * 3 / 2; y = y < 22 ? 22 : y > 164 ? 164 : y;
                if (x) { line(10 + x - 1, py, 10 + x, y, C_AMBER); line(10 + x - 1, py + 1, 10 + x, y + 1, C_AMBER); }
                py = y;
            }
            for (unsigned k = 0; k < 4; ++k) {
                int x = 10 + vsc.p[P_EQ(k, E_FREQ)] * 219 / 127;
                int y = 93 - eq_curve[x - 10 < 0 ? 0 : x - 10 > 219 ? 219 : x - 10] * 3 / 2;
                y = y < 26 ? 26 : y > 160 ? 160 : y;
                disc(x, y, k == view ? 6 : 5, k == view ? C_AMBER : C_DEEP);
                c[0] = (char)('1' + k); c[1] = 0; text(x - 2, y - 3, c, k == view ? C_BODY : C_CREAM);
            }
        }
        knob_row(172);
        footer("SELECT: BAND, THEN COMP");
        return;
    }
    header("MASTER", "COMP", vsc.p[P_CON] ? C_AMBER : C_DEEP, vsc.p[P_CON] ? C_BODY : C_DIM);
    if (IN_BAND(18, 168)) {
        box(6, 19, 228, 148, C_BLACK); frame(6, 19, 228, 148, C_LINE);
        /* the reduction: 0..20 dB down */
        float gr = vsc_meter.comp_gr; gr = gr < 0.f ? 0.f : gr > 20.f ? 20.f : gr;
        int h = (int)(gr * 120.f / 20.f);
        box(16, 26, 22, 124, C_PANEL);
        if (h > 0) rect(18, 28, 18, h, gr > 10.f ? C_RED : C_AMBER);
        for (int i = 0; i <= 20; i += 5) { int y = 28 + i * 120 / 20; hline(40, y, 4, C_DIM); num(c, i); text(46, y - 3, c, C_DIMR); }
        text(14, 155, "GR", C_DIM);
        /* the curve: in -40..0 dB across, out up */
        const int x0 = 84, y0 = 26, sz = 124;
        frame(x0, y0, sz, sz, C_LINE);
        static const float sl[3] = {0.5f, 0.75f, 0.9f};
        float thr = (float)vsc.p[P_CTHR] - 30.f, slope = sl[vsc.p[P_CRATIO] % 3];
        int px_ = x0 + (int)((thr + 40.f) * (float)sz / 40.f);
        line(x0, y0 + sz, px_, y0 + sz - (px_ - x0), C_DIM);
        line(px_, y0 + sz - (px_ - x0), x0 + sz, y0 + sz - (px_ - x0) - (int)((float)(x0 + sz - px_) * (1.f - slope)), C_AMBER);
        float in = vsc_meter.comp_in;
        if (vsc.p[P_CON] && in > -40.f) {
            float o = in > thr ? thr + (in - thr) * (1.f - slope) : in; if (in > 0.f) in = 0.f;
            disc(x0 + (int)((in + 40.f) * (float)sz / 40.f), y0 + sz - (int)((o + 40.f) * (float)sz / 40.f), 2, C_CREAM);
        }
        text(x0, y0 + sz + 4, "IN", C_DIMR); text(x0 + sz + 4, y0, "OUT", C_DIMR);
        vsc_param_label(P_CKEY, vsc.p[P_CKEY], c); cpy(b, "KEY "); cat(b, c);
        text_r(x0 + sz, y0 + sz + 4, vsc.p[P_CKEY] ? b : "THE MIX", vsc.p[P_CKEY] ? C_AMBER : C_DIMR);
    }
    knob_row(172);
    footer(view == 4 ? "AFTER RAVEL'S GLUE COMP" : "KEY: WHAT IT LISTENS TO");
}

static void draw_notice(void)
{
    if (!notice || (int32_t)(notice_until - now_ms) <= 0 || !*notice) return;
    if (!IN_BAND(100, 126)) return;
    int w = text_w(notice) + 16; if (w > 236) w = 236;
    box(120 - w / 2, 102, w, 22, C_AMBER);
    box(120 - w / 2 + 2, 104, w - 4, 18, C_BODY);
    text_c(120, 109, notice, C_CREAM);
}

void vsc_ui_frame(uint32_t now)
{
    now_ms = now;
    /* the reels turn with the tape: a turn about every 1.6 s of tape at 1x */
    uint32_t p = vsc_meter.pos;
    if (vsc_meter.playing) {
        int32_t d = (int32_t)(p - reel_pos);
        if (d < 0) d += (int32_t)(vsc_meter.loop ? vsc_meter.loop : vsc_meter.cap);
        reel_ang += (float)d / tape_hz() * 3.9f;
        if (reel_ang > 62.83f) reel_ang -= 62.83f;
    }
    reel_pos = p;
}
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels)
{
    band = pixels; by0 = (int)y0;
    for (unsigned i = 0; i < VSC_SCREEN_W * VSC_BAND; ++i) pixels[i] = C_BODY;
    switch (page) {
    case PG_MIXER: draw_mixer(); break;
    case PG_INPUT: draw_input(); break;
    case PG_BOUNCE: draw_bounce(); break;
    case PG_RESON: draw_reson(); break;
    case PG_ROBOTS: draw_robots(); break;
    case PG_SAVE: draw_save(); break;
    case PG_SETTINGS: draw_settings(); break;
    case PG_MASTER: draw_master(); break;
    default: draw_tape(); break;
    }
    draw_notice();
}

/* ------------------------------------------------------------- panel LEDs */
void vsc_ui_leds(uint8_t out[41])
{
    uint8_t glow = vsc_dev.backlight ? 1 : 0;
    for (unsigned i = 0; i < 41; ++i) out[i] = glow;
    static const uint8_t page_btn[PG_COUNT] = {BTN_HOME, BTN_EDIT, BTN_EDIT, BTN_SEQ, BTN_ENV, BTN_LFO, BTN_SAVE, BTN_GLO, BTN_FX};
    out[page_btn[page]] = 2;
    if (vsc_meter.playing) out[BTN_PLAY] = 2;
    if (vsc_meter.transport == TR_FIRST || vsc_meter.bounce) out[BTN_REC] = 2;
    else if (vsc_meter.recording) out[BTN_REC] = (now_ms / 250u) & 1u ? 2 : 0;
    for (unsigned b = 0; b < 14; ++b) if (prev_buttons & (1u << b)) out[b] = 2;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) {
        uint8_t *o = &out[14 + trk_key[t]];
        if (page == PG_BOUNCE) *o = vsc.p[P_BSRC] >> t & 1u ? 2 : glow;
        else if (vsc_meter.recording >> t & 1u) *o = (now_ms / 250u) & 1u ? 2 : 0;
        else *o = vsc.p[P_TRK(t, S_ARM)] ? 2 : glow;
    }
    if (page == PG_RESON) for (unsigned k = 0; k < VSC_RBANDS; ++k) out[14 + band_key[k]] = k == rsel || vsc_meter.rb_level[k] > 30 ? 2 : glow;
    for (unsigned k = 0; k < 27; ++k) if (prev_keys & (1u << k)) out[14 + k] = 2;
}

/* CC 110-113: the knob of that column on the page on screen, with pickup. Called from the
 * MIDI task; it only queues, the control task applies it. */
static volatile uint8_t knob_cc_val[4], knob_cc_new[4];
static int16_t knob_cc_last[4] = {-1, -1, -1, -1};
static uint8_t knob_cc_held[4];
static unsigned knob_cc_view = 0xffff;
void vsc_ui_knob_cc(unsigned knob_n, unsigned value)
{
    if (knob_n > 3) return;
    knob_cc_val[knob_n] = (uint8_t)value; knob_cc_new[knob_n] = 1;
}
static void apply_knob_cc(void)
{
    unsigned v_ = page * 16u + view;
    if (v_ != knob_cc_view) { knob_cc_view = v_; for (unsigned k = 0; k < 4; ++k) { knob_cc_held[k] = 0; knob_cc_last[k] = -1; } }
    for (unsigned k = 0; k < 4; ++k) {
        if (!knob_cc_new[k]) continue;
        knob_cc_new[k] = 0;
        int v = knob_cc_val[k];
        unsigned s = slot(k);
        if (s >= VSC_NP || s == P_RPROF) { knob_cc_last[k] = (int16_t)v; continue; }
        int max = vsc_param_info(s)->max, cur = vsc.p[s];
        if (!max) continue;
        int target = (v * max + 63) / 127, now127 = (cur * 127 + max / 2) / max, prev = knob_cc_last[k];
        if (!knob_cc_held[k] && ((v - now127 <= 1 && now127 - v <= 1) ||
            (prev >= 0 && ((prev <= now127 && v >= now127) || (prev >= now127 && v <= now127))))) knob_cc_held[k] = 1;
        knob_cc_last[k] = (int16_t)v;
        if (!knob_cc_held[k] || target == cur) continue;
        vsc.p[s] = (uint8_t)target;
        touch_at[k] = now_ms | 1u;
    }
}
