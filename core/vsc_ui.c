/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera PUTTY: screen and controls. Portable: the platform hands in a control
 * snapshot and asks for 16-row bands of RGB565 pixels. Control context only. */
#include "vsc.h"

struct vsc_host_status vsc_host;

/* ---------------------------------------------------------------- colour
 * Cream on black, as before, with a lavender accent (the putty) and small flashes of
 * red (recording, the playing step, a clip). */
#define RGB(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))
static const uint16_t C_BG = RGB(0, 0, 0), C_PANEL = RGB(30, 29, 27), C_LINE = RGB(52, 50, 46),
    C_DIM = RGB(111, 107, 100), C_MID = RGB(169, 163, 151), C_FG = RGB(230, 223, 209),
    C_INK = RGB(243, 239, 231), C_WARN = RGB(196, 120, 96),
    C_LAV = RGB(178, 152, 224), C_LAVD = RGB(96, 78, 136), C_LAVK = RGB(44, 34, 64), C_RED = RGB(236, 84, 84),
    C_FRAME = RGB(92, 88, 82), C_TOUCH = RGB(30, 24, 42);
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
/* 5x7, ASCII 32..95, column bytes with bit 0 at the top. Two are redrawn for the
 * putty: '^' is a right arrow (A^B: A goes into B) and '\' a times sign (2.00\, A\B). */
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
/* large and a little tighter, for values in the knob cells */
static int big_w(const char *s) { return (int)vsc_strlen(s) * 11 - 1; }
static void big(int x, int y, const char *s, uint16_t c) { char one[2] = {0, 0}; for (; *s; ++s, x += 11) { one[0] = *s; text(x, y, one, c, 2); } }
static void num(char *o, int v)
{
    char t[8]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 7);
    int i = 0; if (neg) o[i++] = '-';
    while (n) o[i++] = t[--n];
    o[i] = 0;
}
static char letter(unsigned t) { return (char)('A' + (t & 3u)); }

/* ------------------------------------------------------------- pages
 * Each button opens a page, and every page is a set of views of four parameters, one
 * for each of KNOB 1-4. SELECT steps through a page's views, ALGORITHM picks the
 * track, PRESETS does the page's own job. The putty pages are the track's mode:
 * opening HANDS, SHARDS or SPECTRA puts the track into it. */
enum { PG_HANDS, PG_SHARDS, PG_SPECTRA, PG_SOURCE, PG_MIX, PG_COLOR, PG_MODS, PG_SPACE, PG_SCENE, PG_SEQ,
       PG_PROJECT, PG_SETTINGS, PG_SOUND, PG_DISSECT, PG_RESON, PG_COUNT };
static const char *const page_name[PG_COUNT] = {"HANDS", "SHARDS", "SPECTRA", "SOURCE", "MIX", "COLOR", "MODS", "SPACE",
                                                "PERFORM", "SEQ", "PROJECT", "SETTINGS", "SOUNDS", "DISSECT", "RESON"};
#define PUTTY_PAGE(p) ((p) <= PG_SPECTRA)
enum { S_NONE, S_T, S_G, S_TEMPO, S_STEP, S_TT, S_SET, S_MAT, S_RBAND, S_RPROF };
struct slot { uint8_t kind, id, tk; };
#define T(p) {S_T, p, 0}
#define G(p) {S_G, p, 0}
#define N {S_NONE, 0, 0}
#define TT(p) {{S_TT, p, 0}, {S_TT, p, 1}, {S_TT, p, 2}, {S_TT, p, 3}}
#define ST(i) {S_STEP, i, 0}
static const struct slot v_hands[3][4] = {{T(P_STRETCH), T(P_PITCH), T(P_POS), T(P_SIZE)},
                                          {T(P_GRAIN), T(P_JITTER), T(P_REV), T(P_LEVEL)},
                                          {T(P_FREEZE), T(P_KEYS), T(P_GAIN), T(P_SOS)}};
static const struct slot v_shards[3][4] = {{T(P_SHAPE), T(P_DENS), T(P_SPRAY), T(P_TRIG)},
                                           {T(P_SSIZE), T(P_SPREAD), T(P_SREV), T(P_STEREO)},
                                           {T(P_STRETCH), T(P_PITCH), T(P_POS), T(P_SIZE)}};
static const struct slot v_spectra[3][4] = {{T(P_STRETCH), T(P_BLUR), T(P_SHIFT), T(P_CROSS)},
                                            {T(P_GATE), T(P_STILT), T(P_HOLD), T(P_SPHASE)},
                                            {T(P_PITCH), T(P_POS), T(P_SIZE), T(P_FREEZE)}};
static const struct slot v_source[3][4] = {{T(P_SRC), T(P_INPUT), T(P_RATE), T(P_SOS)},
                                           {T(P_BOUNCE), T(P_REEL), T(P_SPLICE), T(P_GAIN)},
                                           {T(P_KEYS), T(P_FREEZE), {S_SET, 8, 0}, N}};
static const struct slot v_mix[6][4] = {TT(P_LEVEL), TT(P_PAN), TT(P_SEND), TT(P_DJF), TT(P_MUTE),
                                        {G(G_MAIN), G(G_MCOMP), G(G_INMON), N}};
/* DISSECT: the listener (global), then the track's shards as its hits drive them */
static const struct slot v_dissect[2][4] = {{G(G_ASRC), G(G_ASENS), G(G_AFOCUS), G(G_AHITS)},
                                            {T(P_TRIG), T(P_DENS), T(P_SSIZE), T(P_SHAPE)}};
/* RESON: BANK, BAND, VOCODE, PING, MOTION, PARTICLES, SETUP. BAND's GAIN, FREQ and
 * DECAY are the selected band's (slot_at swaps them in). */
static const struct slot v_reson[7][4] = {{{S_RPROF, 0, 0}, G(G_BWIDTH), G(G_BRESO), G(G_BMIX)},
                                          {{S_RBAND, 0, 0}, G(G_BG0), G(G_BF0), G(G_BD0)},
                                          {G(G_BVMOD), G(G_BATK), G(G_BREL), G(G_BDEPTH)},
                                          {G(G_BTRIG), G(G_BQUANT), G(G_BHARD), G(G_BDUST)},
                                          {G(G_BLIQ), G(G_BFDBK), G(G_BSHIFT), G(G_BSPEC)},
                                          {G(G_BCOHES), G(G_BDROP), G(G_BMIGR), G(G_BOSC)},
                                          {G(G_BMODE), G(G_BSENS), G(G_BTRACK), N}};
#define RV_BAND 1
#define RV_VOC 2
#define RV_PING 3
static const struct slot v_color[3][4] = {{T(P_DRIVE), T(P_COMP), T(P_CRUSH), T(P_TILT)},
                                          {T(P_NOISE), T(P_NDECAY), T(P_NTONE), T(P_CMIX)},
                                          {T(P_CMODE), T(P_XOVER), T(P_NTYPE), T(P_CWMODE)}};
static const struct slot v_space[2][4] = {{G(G_DLEVEL), G(G_DTIME), G(G_DFEED), G(G_DTONE)},
                                          {G(G_RLEVEL), G(G_RSIZE), G(G_RDAMP), G(G_DLONG)}};
static const struct slot v_scene[3][4] = {{G(G_M0), G(G_M1), G(G_M2), G(G_M3)}, {G(G_M4), G(G_M5), G(G_M6), G(G_M7)},
                                          {G(G_TARGET), G(G_MORPH), G(G_MSEL), N}};
/* SEQ: the selected step's SLICE, PITCH, LEN and PROB first (the 856's fragments) */
enum { SF_SLICE, SF_PITCH, SF_LEN, SF_PROB, SF_VELO, SF_LOCK, SF_VALUE, SF_STEPS, SF_TRIG };
static const struct slot v_steps[3][4] = {{ST(SF_SLICE), ST(SF_PITCH), ST(SF_LEN), ST(SF_PROB)},
                                          {ST(SF_VELO), ST(SF_LOCK), ST(SF_VALUE), ST(SF_STEPS)},
                                          {ST(SF_TRIG), T(P_SEQM), N, N}};
static const struct slot v_carom[3][4] = {{T(P_SEQM), T(P_RSPEED), T(P_RGRAV), T(P_RLOSS)},
                                          {T(P_RSPREAD), T(P_RQUANT), T(P_RCOUNT), T(P_RWALLS)},
                                          {T(P_RJITTER), T(P_RGATE), N, N}};
static const struct slot v_settings[5][4] = {{{S_SET, 0, 0}, {S_SET, 1, 0}, {S_SET, 2, 0}, {S_SET, 3, 0}},
                                             {{S_SET, 0, 0}, T(P_INPUT), T(P_GAIN), G(G_INMON)},
                                             {G(G_CLOCK), {S_TEMPO, 0, 0}, G(G_SWING), G(G_INMON)},
                                             {{S_SET, 4, 0}, {S_SET, 5, 0}, {S_SET, 6, 0}, {S_SET, 7, 0}},
                                             {N, N, N, N}};
#define SET_INPUT 1   /* SETTINGS views: 0 Bluetooth, 1 audio in, 2 clock, 3 panel, 4 MIDI learn */
#define SET_CLOCK 2
#define SET_PANEL 3
#define SET_LEARN 4
#define MATRIX_VIEW 0    /* MODS: view 0 is the matrix, then 1..12: four modulators x three pages */
#define MOD_VIEWS 13
#define VMOD(r) (((r) - 1u) / 3u)   /* the modulator a view shows */
#define VPAGE(r) (((r) - 1u) % 3u)
static const char *const mtype_name[4] = {"WAVE", "RANDOM", "FOLLOW", "ADSR"};

static unsigned track, page = PG_HANDS, row, step_sel, step_page, octave = 2, route_sel, learn_sel;
static unsigned mod_sel;      /* the modulator LFO+knob assigns: the one last viewed */
static uint32_t touch_at[4];  /* when each of KNOB 1-4 last turned: its cell lights */
static uint8_t mapping;       /* LFO held: the title takes the modulator's colour */
static uint32_t prev_buttons, prev_keys, rec_down_at, hint_until;
static uint8_t temp_on, rec_mode, live_rec, learn_seen, oct_used, reel_armed;
static uint8_t temp_t[VSC_TRACKS][VSC_NP], temp_g[VSC_NG];
static const char *notice; static uint32_t notice_until, now_ms;
static char notice_buf[40];
static unsigned proj_slot, set_dev, sound_slot;
static uint8_t mod_hist[VSC_MODS][96]; static unsigned hist_w;   /* 0..255 */
static uint8_t dis_lev[96], dis_hit[96];   /* DISSECT: what the listener heard lately, a column a frame */
static uint32_t dis_hits_seen;
enum { RM_IDLE, RM_PENDING, RM_DUB, RM_ROBOT, RM_DONE };   /* what a held REC is doing */

void vsc_ui_notice(const char *t, uint32_t ms) { notice = t; notice_until = now_ms + ms; }
static void say(const char *a, const char *b, uint32_t ms);

/* ------------------------------------------------------------- robots
 * Hold REC on a putty page and turn a knob: the turn is recorded (up to about two seconds)
 * and, on letting go, loops on its own, relative to where the knob is. Four a track;
 * a fifth replaces the oldest. Not kept in projects. */
#define ROB_PTS 48
#define ROB_MS 40
struct robot { uint8_t param, n, base, age; uint32_t t0; int8_t pts[ROB_PTS]; };
static struct robot rob[VSC_TRACKS][VSC_ROBOTS];
static int rob_rec = -1;               /* the slot being recorded on this track, -1 none */
static unsigned rob_rec_t;
static uint8_t rob_age;
static int robot_of(unsigned t, unsigned p)
{
    for (unsigned r = 0; r < VSC_ROBOTS; ++r) if (rob[t][r].n && rob[t][r].param == p) return (int)r;
    return -1;
}
static void robot_start(unsigned t, unsigned p)
{
    int r = robot_of(t, p);
    if (r < 0) {
        unsigned best = 0;
        for (unsigned i = 0; i < VSC_ROBOTS; ++i) {
            if (!rob[t][i].n) { best = i; break; }
            if ((uint8_t)(rob_age - rob[t][i].age) > (uint8_t)(rob_age - rob[t][best].age)) best = i;
        }
        r = (int)best;
    }
    vsc_robot_param[t][r] = 255; vsc_robot_off[t][r] = 0.f;
    struct robot *R = &rob[t][r];
    R->param = (uint8_t)p; R->n = 1; R->base = vsc.t[t][p]; R->t0 = now_ms; R->age = rob_age++; R->pts[0] = 0;
    rob_rec = r; rob_rec_t = t;
}
static void robot_sample(void)
{
    if (rob_rec < 0) return;
    struct robot *R = &rob[rob_rec_t][rob_rec];
    unsigned at = (now_ms - R->t0) / ROB_MS;
    int d = (int)vsc.t[rob_rec_t][R->param] - (int)R->base;
    d = d > 127 ? 127 : d < -127 ? -127 : d;
    while (R->n <= at && R->n < ROB_PTS) R->pts[R->n++] = (int8_t)d;
}
static void robot_finish(void)
{
    if (rob_rec < 0) return;
    robot_sample();
    struct robot *R = &rob[rob_rec_t][rob_rec];
    vsc.t[rob_rec_t][R->param] = R->base;   /* the knob stays where it was; the robot moves around it */
    int any = 0;
    for (unsigned i = 0; i < R->n; ++i) any |= R->pts[i] != 0;
    if (R->n < 3 || !any) { R->n = 0; vsc_ui_notice("ROBOT: HOLD REC, TURN", 1100); }
    else { R->t0 = now_ms; vsc_robot_param[rob_rec_t][rob_rec] = R->param; vsc_ui_notice("ROBOT RUNNING", 900); }
    rob_rec = -1;
}
static void robots_clear(unsigned t)
{
    for (unsigned r = 0; r < VSC_ROBOTS; ++r) { vsc_robot_param[t][r] = 255; vsc_robot_off[t][r] = 0.f; rob[t][r].n = 0; }
    if (rob_rec >= 0 && rob_rec_t == t) rob_rec = -1;
}
static void robots_play(void)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned r = 0; r < VSC_ROBOTS; ++r) {
            const struct robot *R = &rob[t][r];
            if (vsc_robot_param[t][r] == 255 || R->n < 2) continue;
            uint32_t span = (uint32_t)R->n * ROB_MS, el = (now_ms - R->t0) % span;
            unsigned i = el / ROB_MS, j = i + 1 < R->n ? i + 1 : 0;
            float f = (float)(el % ROB_MS) * (1.f / ROB_MS);
            vsc_robot_off[t][r] = ((float)R->pts[i] + ((float)R->pts[j] - (float)R->pts[i]) * f) * (1.f / 127.f);
        }
}

/* RESON's selected band, and its profiles: each starts from the defaults (LIQUID
 * MEMORY) and changes what it lists, (parameter, value) pairs */
static unsigned rb_sel = 1, rb_prof;
static uint32_t rb_seen, rb_flash_at;
#define RP(g, v) (uint8_t)((g) - G_BTRACK), (uint8_t)(v)
static const char *const rprof_name[6] = {"LIQ MEM", "GLASS", "THROAT", "DUST", "RADIO", "DROPS"};
static const uint8_t rprof[] = {
    255,
    RP(G_BG0, 25), RP(G_BG1, 30), RP(G_BG2, 25), RP(G_BG3, 25), RP(G_BG4, 40), RP(G_BG5, 99), RP(G_BG6, 105), RP(G_BG7, 97),
    RP(G_BRESO, 41), RP(G_BHARD, 120), RP(G_BMIX, 90), RP(G_BWIDTH, 90), RP(G_BLIQ, 30), RP(G_BDUST, 30), RP(G_BCOHES, 90), 255,
    RP(G_BMODE, BM_VOCODE), RP(G_BG0, 100), RP(G_BG1, 108), RP(G_BG2, 104), RP(G_BG3, 96), RP(G_BG6, 40), RP(G_BVMOD, BV_OUTPUT),
    RP(G_BDEPTH, 100), RP(G_BATK, 30), RP(G_BREL, 70), RP(G_BRESO, 50), RP(G_BWIDTH, 50), RP(G_BMIX, 85), RP(G_BLIQ, 20), RP(G_BTRIG, BT_OFF), 255,
    RP(G_BMODE, BM_PING), RP(G_BDUST, 70), RP(G_BCOHES, 40), RP(G_BRESO, 100), RP(G_BG0, 85), RP(G_BG1, 95), RP(G_BG2, 90), RP(G_BG3, 88),
    RP(G_BG4, 92), RP(G_BG5, 86), RP(G_BG6, 95), RP(G_BG7, 84), RP(G_BDROP, 40), RP(G_BMIGR, 50), RP(G_BMIX, 100), RP(G_BTRIG, BT_FULL), 255,
    RP(G_BMODE, BM_FILTER), RP(G_BWIDTH, 20), RP(G_BLIQ, 0), RP(G_BG0, 40), RP(G_BG1, 60), RP(G_BG2, 105), RP(G_BG3, 105), RP(G_BG4, 105),
    RP(G_BG5, 105), RP(G_BG6, 50), RP(G_BG7, 40), RP(G_BMIX, 75), RP(G_BRESO, 30), RP(G_BTRIG, BT_OFF), 255,
    RP(G_BDROP, 50), RP(G_BMIGR, 20), RP(G_BDUST, 15), RP(G_BCOHES, 100), RP(G_BFDBK, 12), 255,
};
static void rprof_apply(unsigned n)
{
    const uint8_t *q = rprof;
    for (unsigned k = 0; k < n; ++k) { while (*q != 255) q += 2; ++q; }
    for (unsigned g = G_BMODE; g < VSC_NG; ++g) vsc.g[g] = vsc_gparam_info(g)->def;
    for (; *q != 255; q += 2) vsc.g[G_BTRACK + q[0]] = q[1];
    rb_prof = n;
}
/* RESON goes where its page is opened (the page is the mode, as the putty's) */
static void reson_here(void)
{
    if (vsc.g[G_BTRACK] != track + 1u) { vsc.g[G_BTRACK] = (uint8_t)(track + 1u); say("RESON ON TRACK ", (const char[]){letter(track), 0}, 900); }
    if (!vsc.g[G_BMODE]) vsc.g[G_BMODE] = BM_HYBRID;
}
static unsigned row_count(void)
{
    switch (page) {
    case PG_HANDS: case PG_SHARDS: case PG_SPECTRA: case PG_SOURCE: case PG_COLOR: case PG_SCENE: return 3;
    case PG_MIX: return 6;
    case PG_DISSECT: case PG_SPACE: return 2;
    case PG_RESON: return 7;
    case PG_SETTINGS: return 5;
    case PG_MODS: return MOD_VIEWS;
    case PG_SEQ: return 3;
    default: return 1;
    }
}
static struct slot slot_at(unsigned i)
{
    struct slot none = {S_NONE, 0, 0};
    unsigned r = row % row_count();
    if (i >= 4) return none;
    switch (page) {
    case PG_HANDS: return v_hands[r][i];
    case PG_SHARDS: return v_shards[r][i];
    case PG_SPECTRA: return v_spectra[r][i];
    case PG_SOURCE: return v_source[r][i];
    case PG_MIX: return v_mix[r][i];
    case PG_COLOR: return v_color[r][i];
    case PG_SPACE: return v_space[r][i];
    case PG_SCENE: return v_scene[r][i];
    case PG_SEQ: return vsc.t[track][P_SEQM] == SEQ_CAROM ? v_carom[r][i] : v_steps[r][i];
    case PG_SETTINGS: return v_settings[r][i];
    case PG_DISSECT: return v_dissect[r][i];
    case PG_RESON: { struct slot sl = v_reson[r][i]; if (r == RV_BAND && sl.kind == S_G) sl.id = (uint8_t)(sl.id + rb_sel); return sl; }
    case PG_MODS: {
        if (r == MATRIX_VIEW) { struct slot s = {S_MAT, (uint8_t)i, 0}; return s; }
        unsigned m = VMOD(r), pg = VPAGE(r), k = pg < 2 ? 1 + pg * 4 + i : (i == 0 ? 9 : 99);
        if (k > 9) return none;
        if (!vsc_tparam_info(track, P_MOD(m, k))->name[0]) return none;
        struct slot s = {S_T, (uint8_t)P_MOD(m, k), 0}; return s;
    }
    default: return none;
    }
}
static const char *const step_names[9] = {"SLICE", "PITCH", "LEN", "PROB", "VELO", "LOCK", "VALUE", "STEPS", "TRIG"};
static const char *const set_names[9] = {"BT AUD", "BT MIDI", "ROLE", "UNPAIR", "BACKLIT", "KNOB CC", "METRO", "REEL", "BUFFER"};
static const char *const backlight_names[4] = {"OFF", "LOW", "MID", "HIGH"};
static void copy_n(char *d, const char *s, unsigned n);
static const char *const mat_names[4] = {"SOURCE", "DEVICE", "PARAM", "AMOUNT"};
static const char *const mtype_short[4] = {"WAV", "RND", "FOL", "ENV"};
/* The matrix lists every route on every track (by the track it moves), then a NEW row. */
static unsigned matrix_count(void)
{
    unsigned n = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) n += vsc.route[t][r].param != 255;
    return n;
}
static struct vsc_route *matrix_at(unsigned i, unsigned *tt)
{
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r)
            if (vsc.route[t][r].param != 255 && i-- == 0) { *tt = t; return &vsc.route[t][r]; }
    *tt = track; return 0;
}
static unsigned matrix_index(const struct vsc_route *want)
{
    unsigned n = 0;
    for (unsigned t = 0; t < VSC_TRACKS; ++t)
        for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
            if (&vsc.route[t][r] == want) return n;
            n += vsc.route[t][r].param != 255;
        }
    return n;
}
/* the source as "A WAV1": track, the modulator's type and number */
static void source_name(unsigned t, unsigned src, char *out)
{
    unsigned st = vsc_src_track(t, src), m = src & 3u;
    out[0] = letter(st); out[1] = ' ';
    copy_n(out + 2, mtype_short[vsc.t[st][P_MOD(m, 0)] & 3], 4);
    out[5] = (char)('1' + m); out[6] = 0;
}
/* Destinations are grouped as the pages are: each track's SOURCE, PUTTY, COLOR, MIX
 * and MODS, then SPACE, PERFORM, MASTER and DISSECT. Inside a device they come in the
 * order of its page's knobs (PUTTY: the track's mode first), then the rest. */
enum { D_SRC, D_PUTTY, D_COLOR, D_MIX, D_MODS, D_TRACK_DEVICES };
#define D_SPACE (VSC_TRACKS * D_TRACK_DEVICES)
#define D_PERFORM (D_SPACE + 1)
#define D_MASTER (D_SPACE + 2)
#define D_DISSECT (D_SPACE + 3)
#define D_RESON (D_SPACE + 4)
#define D_COUNT (D_SPACE + 5)
static const char *const dev_short[D_TRACK_DEVICES] = {"SOURC", "PUTTY", "COLOR", "MIX", "MODS"};
static unsigned dev_of(unsigned t, unsigned code)
{
    if (code >= VSC_GDEST) {
        unsigned g = code - VSC_GDEST;
        if (g == G_DTIME || g == G_DFEED || g == G_DTONE || g == G_DLONG || g == G_DLEVEL || g == G_RSIZE || g == G_RDAMP || g == G_RLEVEL) return D_SPACE;
        if (g >= G_BTRACK) return D_RESON;
        if (g >= G_ASRC) return D_DISSECT;
        if (g >= G_M0 || g == G_MORPH) return D_PERFORM;
        return D_MASTER;
    }
    unsigned d = code < P_STRETCH ? D_SRC : code < P_DRIVE ? D_PUTTY : code < P_LEVEL ? D_COLOR : code < P_MOD0 ? D_MIX : D_MODS;
    return t * D_TRACK_DEVICES + d;
}
static unsigned dest_add(unsigned *list, unsigned n, unsigned code)
{
    if (code == 255 || !vsc_mod_dest_valid(code)) return n;
    for (unsigned i = 0; i < n; ++i) if (list[i] == code) return n;
    list[n] = code; return n + 1;
}
static unsigned dest_views(unsigned *list, unsigned n, const struct slot (*v)[4], unsigned rows, unsigned t)
{
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned i = 0; i < 4; ++i) {
            struct slot sl = v[r][i];
            if (sl.kind == S_T) n = dest_add(list, n, sl.id);
            else if (sl.kind == S_TT && sl.tk == t) n = dest_add(list, n, sl.id);
            else if (sl.kind == S_G) n = dest_add(list, n, VSC_GDEST + sl.id);
        }
    return n;
}
#define ROWS(v) (sizeof v / sizeof v[0])
/* every destination of a device, in page order; returns how many */
static unsigned dest_list(unsigned dev, unsigned list[VSC_NP + VSC_NG])
{
    unsigned n = 0;
    if (dev >= D_SPACE) {
        if (dev == D_SPACE) n = dest_views(list, n, v_space, ROWS(v_space), 0);
        else if (dev == D_PERFORM) n = dest_views(list, n, v_scene, ROWS(v_scene), 0);
        else if (dev == D_DISSECT) n = dest_views(list, n, v_dissect, 1, 0);
        else if (dev == D_RESON) n = dest_views(list, n, v_reson, 7, 0);
        else n = dest_views(list, n, &v_mix[5], 1, 0);
        for (unsigned g = 0; g < VSC_NG; ++g) if (dev_of(0, VSC_GDEST + g) == dev) n = dest_add(list, n, VSC_GDEST + g);
        return n;
    }
    unsigned t = dev / D_TRACK_DEVICES, d = dev % D_TRACK_DEVICES;
    switch (d) {
    case D_SRC: n = dest_views(list, n, v_source, ROWS(v_source), t); break;
    case D_PUTTY: {
        unsigned e = vsc.t[t][P_ENGINE];
        if (e == ENG_SHARDS) n = dest_views(list, n, v_shards, ROWS(v_shards), t);
        else if (e == ENG_SPECTRA) n = dest_views(list, n, v_spectra, ROWS(v_spectra), t);
        n = dest_views(list, n, v_hands, ROWS(v_hands), t);
        n = dest_views(list, n, v_shards, ROWS(v_shards), t);
        n = dest_views(list, n, v_spectra, ROWS(v_spectra), t);
        break;
    }
    case D_COLOR: n = dest_views(list, n, v_color, ROWS(v_color), t); break;
    case D_MIX: n = dest_views(list, n, v_mix, 5, t); break;
    default: for (unsigned m = 0; m < VSC_MODS; ++m) for (unsigned k = 1; k < VSC_MOD_PARAMS; ++k) n = dest_add(list, n, P_MOD(m, k)); break;
    }
    for (unsigned c = 0; c < P_SEQM; ++c) if (dev_of(t, c) == dev) n = dest_add(list, n, c);
    for (unsigned c = P_SEQM; c < VSC_NP; ++c) if (dev_of(t, c) == dev) n = dest_add(list, n, c);
    return n;
}
/* a destination's name as on its page: "BLUR"; a modulator's with its number, "2 RATE" */
static void dest_name(unsigned t, unsigned code, char *out)
{
    if (code >= VSC_GDEST) { copy_n(out, vsc_gparam_info(code - VSC_GDEST)->name, 9); return; }
    if (code >= P_MOD0 && code < P_SEQM) {
        out[0] = (char)('1' + (code - P_MOD0) / VSC_MOD_PARAMS); out[1] = ' ';
        copy_n(out + 2, vsc_tparam_info(t, code)->name, 7); return;
    }
    copy_n(out, vsc_tparam_info(t, code)->name, 9);
}
/* the device as "A PUTTY", or SPACE, PERFORM, MASTER */
static void dev_name(unsigned dev, char *out)
{
    if (dev >= D_SPACE) { copy_n(out, dev == D_SPACE ? "SPACE" : dev == D_PERFORM ? "PERFORM" : dev == D_DISSECT ? "DISSECT" : dev == D_RESON ? "RESON" : "MASTER", 8); return; }
    out[0] = letter(dev / D_TRACK_DEVICES); out[1] = ' ';
    copy_n(out + 2, dev_short[dev % D_TRACK_DEVICES], 5);
}
static void copy_n(char *d, const char *s, unsigned n) { unsigned i = 0; while (s[i] && i < n) { d[i] = s[i]; ++i; } d[i] = 0; }
/* 0.00 .. 1.00 from a fraction in hundredths */
static void unit_text(char *o, int c)
{
    c = c < 0 ? 0 : c > 100 ? 100 : c;
    o[0] = (char)('0' + c / 100); o[1] = '.'; o[2] = (char)('0' + (c / 10) % 10); o[3] = (char)('0' + c % 10); o[4] = 0;
}
static void slot_text(struct slot s, char name[10], char val[10], float *bar)
{
    *bar = -1.f; name[0] = val[0] = 0;
    if (s.kind == S_T || s.kind == S_TT) {
        unsigned t = s.kind == S_TT ? s.tk : track;
        const struct vsc_pinfo *in = vsc_tparam_info(t, s.id);
        unsigned v = vsc.t[t][s.id];
        copy_n(name, in->name, 7);
        if (s.kind == S_TT) { unsigned l = (unsigned)vsc_strlen(name); if (l > 6) l = 6; name[l] = ' '; name[l + 1] = letter(t); name[l + 2] = 0; }
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
        copy_n(name, set_names[s.id < 9 ? s.id : 0], 7);
        switch (s.id) {
        case 0: copy_n(val, vsc_host.bt_audio ? "ON" : "OFF", 7); break;
        case 1: copy_n(val, vsc_host.bt_midi ? "ON" : "OFF", 7); break;
        case 2: copy_n(val, vsc_host.ble_role ? "CENTRL" : "PERIPH", 7); break;
        case 4: copy_n(val, backlight_names[vsc_dev.backlight & 3], 7); break;
        case 5: copy_n(val, vsc_dev.knob_cc ? "110-3" : "OFF", 7); break;
        case 6: copy_n(val, vsc_dev.metro ? "ON" : "OFF", 7); break;
        case 8: copy_n(val, vsc_dev.cap_split ? "SPLIT" : "LONG", 7); break;
        case 7: num(val, vsc_host.reel_free); { unsigned l = (unsigned)vsc_strlen(val); val[l] = '%'; val[l + 1] = 0; }
                *bar = (float)vsc_host.reel_free / 100.f; break;
        default: copy_n(val, "TURN", 7); break;
        }
    } else if (s.kind == S_MAT) {
        unsigned tt; const struct vsc_route *ro = matrix_at(route_sel, &tt);
        copy_n(name, mat_names[s.id & 3], 7);
        if (!ro) { copy_n(val, s.id == 0 ? "NEW" : "--", 4); }
        else switch (s.id) {
        case 0: source_name(tt, ro->src, val); break;
        case 1: dev_name(dev_of(tt, ro->param), val); break;
        case 2: dest_name(tt, ro->param, val); break;
        default: num(val, ro->depth); *bar = ((float)ro->depth + 127.f) / 254.f; break;
        }
    } else if (s.kind == S_RBAND) {
        copy_n(name, "BAND", 7); val[0] = 'B'; val[1] = (char)('1' + rb_sel); val[2] = 0; *bar = (float)rb_sel / 7.f;
    } else if (s.kind == S_RPROF) {
        copy_n(name, "PROFILE", 8); copy_n(val, rprof_name[rb_prof % 6], 8);
    } else if (s.kind == S_STEP) {
        const struct vsc_step *st = &vsc.step[track][step_sel];
        copy_n(name, step_names[s.id < 9 ? s.id : 0], 7);
        switch (s.id) {
        case SF_SLICE: if (!st->slice) copy_n(val, "OFF", 4); else unit_text(val, (st->slice - 1) * 100 / 16);
                       *bar = (float)st->slice / 16.f; break;
        case SF_PITCH: { int d = (int)st->note - 60, i = 0; if (d >= 0) val[i++] = '+'; num(val + i, d);
                         i = (int)vsc_strlen(val); val[i++] = 'S'; val[i++] = 'T'; val[i] = 0;
                         *bar = (float)(d + 24) / 48.f; *bar = *bar < 0.f ? 0.f : *bar > 1.f ? 1.f : *bar; break; }
        case SF_LEN: unit_text(val, st->len * 100 / 8); *bar = (float)st->len / 16.f; break;
        case SF_PROB: unit_text(val, st->prob); *bar = (float)st->prob / 100.f; break;
        case SF_VELO: num(val, st->vel); *bar = (float)st->vel / 127.f; break;
        case SF_LOCK: if (st->lp[0] < VSC_NP) copy_n(val, vsc_tparam_info(track, st->lp[0])->name, 7); else copy_n(val, "--", 3); break;
        case SF_VALUE: if (st->lp[0] < VSC_NP) num(val, st->lv[0]); else copy_n(val, "--", 3); break;
        case SF_STEPS: num(val, vsc.length[track] ? vsc.length[track] : 16); break;
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
static void edit_t(unsigned t, unsigned p, int d)
{
    const struct vsc_pinfo *in = vsc_tparam_info(t, p);
    if (!in->name[0]) return;
    if (!(in->flags & VSC_PF_ENUM) && (d > 2 || d < -2)) d *= 2;
    vsc.t[t][p] = (uint8_t)clampi((int)vsc.t[t][p] + d, 0, in->max);
}
static void edit_slot(struct slot s, int d, unsigned *req, unsigned *arg)
{
    if (s.kind == S_T || s.kind == S_TT) {
        edit_t(s.kind == S_TT ? s.tk : track, s.id, d);
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
        else if (s.id == 8) { vsc_dev.cap_split = d > 0; ++vsc_dev_changed; }
        else if (s.id == 7) { reel_armed = d > 0; vsc_ui_notice(reel_armed ? "REC: ERASE THE REEL" : "REEL KEPT", 2500); }
    } else if (s.kind == S_MAT) {
        unsigned tt; struct vsc_route *ro = matrix_at(route_sel, &tt);
        if (!ro) {
            /* NEW: turning any knob makes a route, from this track's modulator to its first parameter */
            for (unsigned r = 0; r < VSC_MOD_ROUTES && !ro; ++r) if (vsc.route[track][r].param == 255) ro = &vsc.route[track][r];
            if (!ro) { vsc_ui_notice("ROUTES FULL", 1200); return; }
            tt = track; ro->src = (uint8_t)vsc_src_code(track, track, mod_sel);
            { unsigned l[VSC_NP + VSC_NG], k = dest_list(track * D_TRACK_DEVICES + D_PUTTY, l); ro->param = (uint8_t)(k ? l[0] : P_STRETCH); }
            ro->depth = 32; ro->flags = 0;
            route_sel = matrix_index(ro);
            return;
        }
        if (s.id == 0) {   /* SOURCE: the sixteen modulators, T1 WAV1 .. T4 ENV4 */
            unsigned st = vsc_src_track(tt, ro->src), i = (unsigned)clampi((int)(st * 4 + (ro->src & 3)) + d, 0, VSC_TRACKS * VSC_MODS - 1);
            ro->src = (uint8_t)vsc_src_code(tt, i / 4, i % 4);
        } else if (s.id == 1) {   /* DEVICE: T1 MATERIAL .. T4 MODS, SPACE, PERFORM, MASTER */
            unsigned cur = dev_of(tt, ro->param), l[VSC_NP + VSC_NG], k = 0;
            int nd = (int)cur;
            for (int steps = d > 0 ? d : -d; steps > 0; --steps) {   /* one device a detent, skipping empty ones */
                int at = nd;
                do at += d > 0 ? 1 : -1; while (at >= 0 && at < D_COUNT && !dest_list((unsigned)at, l));
                if (at < 0 || at >= D_COUNT) break;
                nd = at;
            }
            if (nd == (int)cur || !(k = dest_list((unsigned)nd, l))) return;
            unsigned nt = nd >= D_SPACE ? tt : (unsigned)nd / D_TRACK_DEVICES, st = vsc_src_track(tt, ro->src), m = ro->src & 3u;
            struct vsc_route *to = nt == tt ? ro : 0;
            for (unsigned r = 0; r < VSC_MOD_ROUTES && !to; ++r) if (vsc.route[nt][r].param == 255) to = &vsc.route[nt][r];
            if (!to) { vsc_ui_notice("ROUTES FULL", 1200); return; }
            struct vsc_route moved = *ro;
            moved.param = (uint8_t)l[0];
            moved.src = (uint8_t)vsc_src_code(nt, st, m);
            if (to != ro) { ro->param = 255; ro->depth = 0; }
            *to = moved;
            route_sel = matrix_index(to);
        } else if (s.id == 2) {   /* PARAM: the device's parameters in page order; before the first, the route goes */
            unsigned list[VSC_NP + VSC_NG], k = dest_list(dev_of(tt, ro->param), list), at = 0;
            for (unsigned i = 0; i < k; ++i) if (list[i] == ro->param) at = i;
            int to = (int)at + d;
            if (to < 0) { ro->param = 255; ro->depth = 0; ro->flags = 0; vsc_ui_notice("ROUTE REMOVED", 900); return; }
            ro->param = (uint8_t)list[clampi(to, 0, (int)k - 1)];
        } else { int v = clampi(ro->depth + d * 2, -127, 127); ro->depth = (int8_t)(v ? v : (d > 0 ? 1 : -1)); }
    } else if (s.kind == S_RBAND) {
        rb_sel = (unsigned)clampi((int)rb_sel + d, 0, VSC_RBANDS - 1);
    } else if (s.kind == S_RPROF) {
        rprof_apply((unsigned)clampi((int)rb_prof + d, 0, 5)); reson_here();
        say("PROFILE ", rprof_name[rb_prof], 1000);
    } else if (s.kind == S_STEP) {
        struct vsc_step *st = &vsc.step[track][step_sel];
        switch (s.id) {
        case SF_SLICE: st->slice = (uint8_t)clampi(st->slice + d, 0, 16); break;
        case SF_PITCH: st->note = (uint8_t)clampi(st->note + d, 36, 84); break;
        case SF_LEN: st->len = (uint8_t)clampi(st->len + d, 1, 16); break;
        case SF_PROB: st->prob = (uint8_t)clampi(st->prob + d * 2, 0, 100); break;
        case SF_VELO: st->vel = (uint8_t)clampi(st->vel + d, 1, 127); break;
        case SF_LOCK: { int p = st->lp[0] >= VSC_NP ? -1 : st->lp[0]; p = clampi(p + d, -1, VSC_NP - 1);
                  while (p >= 0 && p < VSC_NP && (!vsc_tparam_info(track, (unsigned)p)->name[0] || p == P_SRC || p == P_RATE || p == P_INPUT)) p += d > 0 ? 1 : -1;
                  st->lp[0] = p < 0 || p >= VSC_NP ? 255 : (uint8_t)p;
                  if (st->lp[0] < VSC_NP) st->lv[0] = vsc.t[track][st->lp[0]]; break; }
        case SF_VALUE: if (st->lp[0] < VSC_NP) st->lv[0] = (uint8_t)clampi(st->lv[0] + d, 0, vsc_tparam_info(track, st->lp[0])->max); break;
        case SF_STEPS: { int l = vsc.length[track] ? vsc.length[track] : 16; vsc.length[track] = (uint8_t)clampi(l + d, 1, VSC_STEPS); break; }
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
void vsc_ui_init(void)
{
    notice = "VISCERA PUTTY " VSC_VERSION; notice_until = 1500;
    for (unsigned t = 0; t < VSC_TRACKS; ++t) for (unsigned r = 0; r < VSC_ROBOTS; ++r) vsc_robot_param[t][r] = 255;
#ifdef VSC_BENCH
    { void vsc_bench_setup(void); vsc_bench_setup(); }
#endif
}

/* store the track's sound in the chosen slot; the FM-1 writes it to flash at once.
 * Like projects, only while stopped (a flash write stalls the sound). */
static unsigned store_sound(void)
{
    if (vsc_meter.playing) { vsc_ui_notice("STOP PLAYBACK TO SAVE", 1200); return HOST_NONE; }
    vsc_sound_store(sound_slot, track);
    vsc_ui_notice("SOUND SAVED", 900);
    return HOST_DEVICE_SAVE;
}
/* open a page; the putty pages also put the track into their mode */
static const uint8_t page_engine[3] = {ENG_HANDS, ENG_SHARDS, ENG_SPECTRA};
static void open_page(unsigned pg)
{
    if (page != pg) { page = pg; row = 0; hint_until = now_ms + 2500; }
    else row = (row + 1) % row_count();
    if (PUTTY_PAGE(pg)) vsc.t[track][P_ENGINE] = page_engine[pg];
}
static void set_track(unsigned t)
{
    if (t == track) return;
    track = t;
    /* on a putty page the page follows the new track's mode */
    if (page == PG_RESON) reson_here();
    if (PUTTY_PAGE(page)) { unsigned e = vsc.t[track][P_ENGINE]; unsigned pg = e == ENG_SHARDS ? PG_SHARDS : e == ENG_SPECTRA ? PG_SPECTRA : PG_HANDS;
                            if (pg != page) { page = pg; row = 0; } }
}
static int capture_page(void) { return PUTTY_PAGE(page) || page == PG_SOURCE; }
#define B(n) (1u << (n))
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg)
{
    unsigned req = HOST_NONE;
    now_ms = in->now_ms;
    uint32_t down = in->buttons & ~prev_buttons, up = ~in->buttons & prev_buttons;
    uint32_t kdown = in->keys & ~prev_keys, kup = ~in->keys & prev_keys;
    int lfo_held = (in->buttons & B(BTN_LFO)) != 0, arp_held = (in->buttons & B(BTN_ARP)) != 0;
    int home_held = (in->buttons & B(BTN_HOME)) != 0, save_held = (in->buttons & B(BTN_SAVE)) != 0;
    int rec_held = (in->buttons & B(BTN_REC)) != 0;
    int od_held = (in->buttons & B(BTN_OCTD)) != 0, ou_held = (in->buttons & B(BTN_OCTU)) != 0;
    if (vsc_learn_seen != learn_seen) { learn_seen = vsc_learn_seen; vsc_ui_notice("LEARNED", 900); }

    /* TEMP: both octave buttons held; everything snaps back on release */
    int both = od_held && ou_held;
    if (both && !temp_on) { temp_on = 1; oct_used = 1; vsc_memcpy(temp_t, vsc.t, sizeof temp_t); vsc_memcpy(temp_g, vsc.g, sizeof temp_g); vsc_ui_notice("TEMP", 600); }
    else if (!both && temp_on && !od_held && !ou_held) {
        temp_on = 0; vsc_memcpy(vsc.t, temp_t, sizeof temp_t); vsc_memcpy(vsc.g, temp_g, sizeof temp_g);
    }
    if (!od_held && !ou_held) {
        /* an octave button only moves the octave (or the steps' page) when it wasn't used
         * as a hand: held with a knob turned, it moves the pair */
        int steps_view = page == PG_SEQ && vsc.t[track][P_SEQM] == SEQ_STEPS;
        if ((up & B(BTN_OCTD)) && !oct_used) { if (steps_view) { if (step_page > 0) --step_page; } else if (octave > 0) --octave; }
        if ((up & B(BTN_OCTU)) && !oct_used) { if (steps_view) { if (step_page < 3) ++step_page; } else if (octave < 4) ++octave; }
        oct_used = 0;
    }

    /* bit 31 of prev_buttons marks "a held button was used as a modifier" */
    uint32_t used = prev_buttons & B(31);
    if (!(in->buttons & (B(BTN_LFO) | B(BTN_ARP) | B(BTN_SAVE) | B(BTN_HOME)))) used = 0;
    /* page buttons; pressing the button of the open page goes to its next view */
    if (down & B(BTN_FX)) open_page(PG_SHARDS);
    if (down & B(BTN_SCL)) open_page(PG_SPECTRA);
    /* ENV: the track's COLOR, then RESON (which comes to this track) */
    if (down & B(BTN_ENV)) {
        if (page == PG_COLOR) { page = PG_RESON; row = 0; hint_until = now_ms + 2500; reson_here(); }
        else { page = PG_COLOR; row = 0; }
    }
    if (down & B(BTN_SEQ)) open_page(PG_SEQ);
    /* EDIT: the track's SOURCE, then its MIX */
    if (down & B(BTN_EDIT)) { if (page == PG_SOURCE) { if (row + 1 < row_count()) ++row; else { page = PG_MIX; row = 0; } }
                              else if (page == PG_MIX) { if (row + 1 < row_count()) ++row; else { page = PG_SOURCE; row = 0; } }
                              else { page = PG_SOURCE; row = 0; hint_until = now_ms + 2500; } }
    if ((up & B(BTN_HOME)) && !(prev_buttons & B(31))) open_page(PG_HANDS);
    if ((up & B(BTN_LFO)) && !(prev_buttons & B(31))) {
        /* LFO: the matrix first, then one press per modulator, then the matrix again */
        if (page != PG_MODS) { page = PG_MODS; row = MATRIX_VIEW; }
        else { row = row == MATRIX_VIEW ? 1 : (VMOD(row) + 1) * 3 + 1; if (row >= MOD_VIEWS) row = MATRIX_VIEW; }
    }
    if ((up & B(BTN_ARP)) && !(prev_buttons & B(31))) open_page(PG_SCENE);
    /* GLO: the global things, SPACE (the shared bus), DISSECT (the listener), then
     * SETTINGS (the FM-1's own) */
    if (down & B(BTN_GLO)) { page = page == PG_SPACE ? PG_DISSECT : page == PG_DISSECT ? PG_SETTINGS : PG_SPACE; row = 0; }
    /* SAVE: where things are kept, PROJECT then SOUNDS */
    if ((up & B(BTN_SAVE)) && !(prev_buttons & B(31))) { page = page == PG_PROJECT ? PG_SOUND : PG_PROJECT; row = 0; }
    if (page == PG_MODS && row != MATRIX_VIEW) mod_sel = VMOD(row);

    /* REC on the putty pages and SOURCE: tap to start a capture and tap to end it; hold
     * to overdub; hold and turn a knob to record a robot; with PLAY, clear; with SAVE,
     * KEEP the capture on the reel. */
    if (down & B(BTN_REC)) {
        rec_down_at = now_ms; rec_mode = RM_PENDING;
        if (page == PG_MODS && row != MATRIX_VIEW && (vsc.t[track][P_MOD(VMOD(row), 0)] & 3) == MT_ADSR) {
            vsc_event(EV_ACTION, track, ACT_MOD_TRIG, VMOD(row)); rec_mode = RM_DONE;
            prev_buttons |= B(30);   /* the release ends the envelope */
        } else if (save_held && capture_page()) {
            rec_mode = RM_DONE; used = B(31);
            if (vsc.t[track][P_SRC] != SRC_CAPTURE || !vsc_meter.src_len[track]) vsc_ui_notice("KEEP: CAPTURE FIRST", 1200);
            else if (vsc_host.reel_free < 2) vsc_ui_notice("THE REEL IS FULL", 1500);
            else { *arg = track; req = HOST_KEEP; vsc_ui_notice("KEPT ON THE REEL", 1100); }
        }
    }
    if ((up & B(BTN_REC)) && (prev_buttons & B(30))) { vsc_event(EV_ACTION, track, ACT_MOD_TRIG, 0x80 | VMOD(row)); }
    if (rec_held && (down & B(BTN_PLAY)) && rec_mode != RM_DONE && capture_page()) {
        if (rec_mode == RM_DUB) vsc_event(EV_REC, track, REC_DUB_OFF, 0);
        if (rob_rec >= 0) rob_rec = -1;
        vsc_event(EV_LOOP_CLEAR, track, 0, 0); robots_clear(track);
        vsc_ui_notice("CLEARED", 1100); rec_mode = RM_DONE;
    } else if ((down & B(BTN_PLAY)) && !(save_held && (page == PG_PROJECT || page == PG_SOUND)) && !rec_held) vsc_event(EV_PLAY, track, 0, 0);
    if (rec_held && rec_mode == RM_PENDING && capture_page() && now_ms - rec_down_at > 450) {
        if (vsc.t[track][P_SRC] == SRC_CAPTURE && vsc_meter.cap_state[track] >= 2) {
            vsc_event(EV_REC, track, REC_DUB_ON, 0); rec_mode = RM_DUB; vsc_ui_notice("OVERDUB: KNOB = ROBOT", 1500);
        }
    }
    if (rec_held && rec_mode == RM_PENDING && !capture_page() && now_ms - rec_down_at > 900) {
        if (page == PG_SEQ && vsc.t[track][P_SEQM] == SEQ_CAROM) {
            rec_mode = RM_DONE; vsc_event(EV_BALL_CLEAR, track, 0, 0); vsc_ui_notice("BALLS CLEARED", 900);
        } else if (page == PG_SPACE) {
            rec_mode = RM_DONE; vsc_event(EV_ACTION, 0, ACT_SPACE_CLEAR, 0); vsc_ui_notice("SPACE CLEARED", 900);
        }
    }
    if (up & B(BTN_REC)) {
        unsigned was = rec_mode; rec_mode = RM_IDLE;
        if (was == RM_DUB) { vsc_event(EV_REC, track, REC_DUB_OFF, 0); }
        else if (was == RM_ROBOT) robot_finish();
        else if (was == RM_PENDING && capture_page()) {
            if (vsc.t[track][P_SRC] != SRC_CAPTURE) { vsc.t[track][P_SRC] = SRC_CAPTURE; vsc_ui_notice("SOURCE: CAPTURE", 900); }
            vsc_event(EV_REC, track, REC_TAP, 0);
        }
        else if (was == RM_PENDING) {
            if (page == PG_SEQ) {
                live_rec ^= 1;
                if (vsc.t[track][P_SEQM] == SEQ_CAROM) vsc_ui_notice(live_rec ? "PLAY NOTES TO ADD BALLS" : "BALL RECORD OFF", 1200);
                else vsc_ui_notice(live_rec ? "STEP RECORD ON" : "STEP RECORD OFF", 900);
            }
            else if (page == PG_PROJECT) { *arg = proj_slot; req = vsc_host.slot_used[proj_slot] ? HOST_LOAD : HOST_SAVE; }
            else if (page == PG_SOUND) {
                if (vsc_dev.sound[sound_slot].used) {
                    vsc_sound_recall(sound_slot, track);
                    say("SOUND ON TRACK ", (const char[]){letter(track), 0}, 1000);
                } else req = store_sound();
            }
            else if (page == PG_SETTINGS && row == 0) { if (vsc_host.ble_count) { *arg = set_dev; req = HOST_BLE_CONNECT; } else req = HOST_BLE_SCAN; }
            else if (page == PG_SETTINGS && row == SET_INPUT) { page = PG_SOURCE; row = 0; }
            else if (page == PG_SETTINGS && row == SET_PANEL && reel_armed) { reel_armed = 0; req = HOST_REEL_CLEAR; vsc_ui_notice("REEL ERASED", 1000); }
            else if (page == PG_SETTINGS && row == SET_LEARN) { vsc_dev.learn[learn_sel % VSC_LEARN].param = 255; ++vsc_dev_changed; vsc_ui_notice("MAPPING CLEARED", 800); }
            else if (page == PG_MODS && row == MATRIX_VIEW) {
                unsigned tt; struct vsc_route *ro = matrix_at(route_sel, &tt);
                if (ro) { ro->flags ^= 1; vsc_ui_notice(ro->flags & 1 ? "BIPOLAR: SWINGS BOTH WAYS" : "UNIPOLAR: ONE WAY", 900); }
            }
            else if (page == PG_SPACE) { vsc_event(EV_ACTION, 0, ACT_SPACE_FREEZE, 0); vsc_ui_notice(vsc_meter.space_frozen ? "SPACE FLOWING" : "SPACE FROZEN", 900); }
            else if (page == PG_MIX || page == PG_COLOR) vsc_event(EV_REC, track, REC_TAP, 0);
            else if (page == PG_RESON) { reson_here(); vsc_event(EV_ACTION, track, ACT_RESON_PING, 0); }
            else if (page == PG_DISSECT) { open_page(PG_SHARDS); vsc.t[track][P_TRIG] = TRIG_HIT; vsc_ui_notice("SHARDS ON THE HITS", 1000); }
        }
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
    if (in->enc[1]) set_track((unsigned)clampi((int)track + in->enc[1], 0, VSC_TRACKS - 1));
    if (in->enc[6]) {
        int d = in->enc[6];
        if (lfo_held) { mod_sel = (unsigned)clampi((int)mod_sel + d, 0, VSC_MODS - 1); used = B(31);
                        say("ASSIGNING MOD ", (const char[]){(char)('1' + mod_sel), 0}, 900); }
        else if (arp_held) { vsc.g[G_MSEL] = (uint8_t)clampi((int)vsc.g[G_MSEL] + d, 0, VSC_MACROS - 1); used = B(31);
                             say("MACRO ", (const char[]){(char)('1' + vsc.g[G_MSEL]), 0}, 900); }
        else switch (page) {
        case PG_SOURCE: vsc.t[track][P_SRC] = (uint8_t)clampi((int)vsc.t[track][P_SRC] + d, 0, SRC_THRU); break;
        case PG_MODS:
            if (row == MATRIX_VIEW) route_sel = (unsigned)clampi((int)route_sel + d, 0, (int)matrix_count());
            else {
                unsigned m = VMOD(row), ty = (unsigned)clampi((int)(vsc.t[track][P_MOD(m, 0)] & 3) + d, 0, 3);
                if (ty != (vsc.t[track][P_MOD(m, 0)] & 3u)) { vsc.t[track][P_MOD(m, 0)] = (uint8_t)ty; vsc_mod_defaults(track, m); }
            }
            break;
        case PG_SEQ:   /* STEPS: the step; CAROM: STEPS or CAROM */
            if (vsc.t[track][P_SEQM] == SEQ_STEPS) {
                unsigned len = vsc.length[track] ? vsc.length[track] : 16;
                step_sel = (unsigned)clampi((int)step_sel + d, 0, (int)len - 1); step_page = step_sel / 16;
            } else { vsc.t[track][P_SEQM] = SEQ_STEPS; row = 0; vsc_ui_notice("STEPS", 900); }
            break;
        case PG_PROJECT: proj_slot = (unsigned)clampi((int)proj_slot + d, 0, VSC_PROJECTS - 1); break;
        case PG_SOUND: sound_slot = (unsigned)clampi((int)sound_slot + d, 0, VSC_SOUNDS - 1); break;
        case PG_SETTINGS:
            if (row == SET_INPUT) set_track((unsigned)clampi((int)track + d, 0, VSC_TRACKS - 1));
            else if (row == SET_LEARN) learn_sel = (unsigned)clampi((int)learn_sel + d, 0, VSC_LEARN - 1);
            else set_dev = (unsigned)clampi((int)set_dev + d, 0, vsc_host.ble_count ? vsc_host.ble_count - 1 : 0);
            break;
        case PG_SCENE: vsc.g[G_TARGET] = (uint8_t)clampi((int)vsc.g[G_TARGET] + d, 0, VSC_SCENES - 1); break;
        default: set_track((unsigned)clampi((int)track + d, 0, VSC_TRACKS - 1)); break;
        }
    }
    /* KNOB 1-4: the view's four parameters. Held: LFO maps the modulator, ARP the macro,
     * SAVE arms MIDI learn, REC records a robot, OCT- moves the pair together (mirrored)
     * and OCT+ moves it apart (opposed). */
    for (unsigned e = 2; e < 6; ++e) {
        int d = in->enc[e];
        if (!d) continue;
        struct slot s = slot_at(e - 2);
        unsigned t, code = slot_code(s, &t);
        int cont = code != 255 && (code >= VSC_GDEST ? !(vsc_gparam_info(code - VSC_GDEST)->flags & VSC_PF_ENUM)
                                                    : !(vsc_tparam_info(t, code)->flags & VSC_PF_ENUM));
        if (lfo_held && cont && page != PG_MODS) {
            /* the modulator is this track's; the parameter can be another track's (MIX) */
            unsigned src = vsc_src_code(t, track, mod_sel);
            int depth = vsc_route_get(t, src, code) + d * 3;
            vsc_route_set(t, src, code, depth);
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
        } else if (rec_held && capture_page() && s.kind == S_T && cont && code < P_MOD0 && (rec_mode == RM_PENDING || rec_mode == RM_DUB || rec_mode == RM_ROBOT)) {
            if (rec_mode != RM_ROBOT) {
                if (rec_mode == RM_DUB) vsc_event(EV_REC, track, REC_DUB_OFF, 0);
                robot_start(track, code); rec_mode = RM_ROBOT;
                say("ROBOT: ", vsc_tparam_info(track, code)->name, 1500);
            }
            edit_slot(s, d, &req, arg);
        } else {
            edit_slot(s, d, &req, arg);
            if ((od_held ^ ou_held) && s.kind == S_T && code < P_MOD0) {   /* two hands */
                edit_t(VSC_PARTNER(track), code, ou_held ? -d : d);
                oct_used = 1;
            }
        }
        touch_at[e - 2] = now_ms | 1u;
    }
    mapping = (uint8_t)(lfo_held ? 1 + mod_sel : 0);
    robot_sample();
    robots_play();

    /* keys */
    for (unsigned k = 0; k < 27; ++k) {
        unsigned note = 53 + k + octave * 12 - 24;
        if (kdown & B(k)) {
            if (home_held && k >= 23) { set_track(k - 23); used = B(31); continue; }
            if (arp_held && k < VSC_SCENES) {
                if (save_held) { vsc_scene_store(k); vsc_ui_notice("SCENE STORED", 800); }
                else vsc_scene_recall(k);
                used = B(31); continue;
            }
            int carom = vsc.t[track][P_SEQM] == SEQ_CAROM;
            if (carom && live_rec) vsc_event(EV_BALL, track, note, 100);
            if (page == PG_RESON && row == RV_BAND && k < VSC_RBANDS) { rb_sel = k; continue; }
            if (page == PG_SEQ && k < 16 && !carom) {
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
            if (page == PG_SEQ && k < 16 && vsc.t[track][P_SEQM] == SEQ_STEPS) continue;
            if (page == PG_RESON && row == RV_BAND && k < VSC_RBANDS) continue;
            vsc_event(EV_NOTE_OFF, track, note, 0);
        }
    }
    apply_knob_cc(&req, arg);
    vsc_ana_view = page == PG_DISSECT;   /* the listener runs while its page is on screen */
    uint32_t keep30 = (prev_buttons & B(30)) && rec_held ? B(30) : 0;
    prev_buttons = (in->buttons & 0x3fffffffu) | used | keep30;
    prev_keys = in->keys;
    return req;
}

/* ------------------------------------------------------------- drawing
 * The screen, after the PUTTY mockups: a small header, the track and page large, the
 * picture, four knob cells, and a status line. */
#define VIS_Y 36
#define VIS_H 120
#define VY(n) (VIS_Y + (n) * 6 / 7)   /* the old 140-row pictures, in 120 */
#define VH(n) ((n) * 6 / 7)
#define KNOB_Y 160
#define FOOT_Y 220

static const char *page_title(char *buf)
{
    switch (page) {
    case PG_MODS:
        if (row == MATRIX_VIEW) return "MATRIX";
        { unsigned m = VMOD(row); copy_n(buf, mtype_name[vsc.t[track][P_MOD(m, 0)] & 3], 8);
          unsigned l = (unsigned)vsc_strlen(buf); buf[l] = ' '; buf[l + 1] = (char)('1' + m); buf[l + 2] = 0; return buf; }
    case PG_SEQ: return vsc.t[track][P_SEQM] == SEQ_CAROM ? "CAROM" : "SEQ";
    default: return page_name[page];
    }
}
static int track_page(void) { return page != PG_RESON && page != PG_SPACE && page != PG_SCENE && page != PG_PROJECT && page != PG_SETTINGS && page != PG_DISSECT; }
static int recording(void)
{
    return live_rec || rob_rec >= 0 || vsc_meter.cap_state[track] == 1 || vsc_meter.cap_state[track] == 3;
}
/* a lavender badge with dark text */
static int badge(int xr, int y, const char *s, uint16_t bg)
{
    int w = text_w(s, 1) + 6;
    rect(xr - w, y, w, 11, bg); text(xr - w + 3, y + 2, s, C_BG, 1);
    return xr - w;
}
static void draw_header(void)
{
    char b[24];
    text(4, 3, "VISCERA PUTTY", C_MID, 1);
    /* the page's views as dots, right */
    unsigned views = page == PG_MODS ? (row == MATRIX_VIEW ? 1 : 3) : row_count(), cur = page == PG_MODS && row != MATRIX_VIEW ? VPAGE(row) : row;
    if (views > 1)
        for (unsigned r = 0; r < views; ++r) {
            int x = VSC_SCREEN_W - 6 - (int)(views - r) * 8;
            uint16_t c = r == cur ? C_FG : C_LINE;
            rect(x + 1, 3, 3, 5, c); rect(x, 4, 5, 3, c);
        }
    if (temp_on) badge(VSC_SCREEN_W - 6 - (int)views * 8 - 4, 1, "TEMP", C_WARN);
    hline(4, 13, VSC_SCREEN_W - 8, C_LINE);
    /* "A / HANDS": the track and the page, large; while LFO is held, in the modulator's colour */
    char tb[16];
    const char *title = page_title(tb);
    int x = 4;
    uint16_t tc = mapping ? C_MOD[(mapping - 1) & 3] : C_INK;
    if (track_page() || (page == PG_RESON && vsc.g[G_BTRACK])) {
        b[0] = page == PG_RESON ? letter(vsc.g[G_BTRACK] - 1u) : letter(track); b[1] = ' '; b[2] = '/'; b[3] = ' '; b[4] = 0; x = text(x, 18, b, tc, 2);
    }
    text(x, 18, title, tc, 2);
    /* right: REC, and the pair's link */
    int xr = VSC_SCREEN_W - 4;
    unsigned u = VSC_PARTNER(track), lo = track & ~1u;
    if (capture_page() || page == PG_MIX) {
        unsigned bt = vsc.t[track][P_BOUNCE], bu = vsc.t[u][P_BOUNCE];
        b[0] = 0;
        if (vsc.t[track][P_ENGINE] == ENG_SPECTRA && vsc.t[track][P_CROSS] > 4) { b[0] = letter(lo); b[1] = '\\'; b[2] = letter(lo + 1); b[3] = 0; }
        else if (bt == BOUNCE_PING || bu == BOUNCE_PING) { b[0] = letter(lo); b[1] = '<'; b[2] = '>'; b[3] = letter(lo + 1); b[4] = 0; }
        else if (bt == BOUNCE_IN) { b[0] = letter(u); b[1] = '^'; b[2] = letter(track); b[3] = 0; }
        else if (bu == BOUNCE_IN) { b[0] = letter(track); b[1] = '^'; b[2] = letter(u); b[3] = 0; }
        if (b[0]) xr = badge(xr, 20, b, C_LAV) - 6;
    }
    if (recording()) {
        int on = (now_ms / 400u) & 1u;
        rect(xr - 8, 22, 7, 7, on ? C_RED : C_LAVK); rect(xr - 7, 21, 5, 9, on ? C_RED : C_LAVK); rect(xr - 9, 23, 9, 5, on ? C_RED : C_LAVK);
        text(xr - 30, 22, "REC", C_RED, 1);
    }
}
/* the SHARDS window shape (as the engine has it), for the pictures */
static float ex2(float x)
{
    if (x < -24.f) return 0.f;
    int i = (int)x; if ((float)i > x) --i;
    float f = x - (float)i, p = 1.f + f * (0.6951786f + f * (0.2261598f + f * 0.0782371f));
    union { float f; uint32_t u; } v; v.f = p; v.u += (uint32_t)i << 23; return v.f;
}
static float han(float p) { float s = p * (1.f - p) * 4.f; return s * s; }   /* near enough to hann for a picture */
static float expo_(float ph, float sharp) { return ph < 0.01f ? ph * 100.f : ex2(-(ph - 0.01f) * sharp); }
static float shard_env(float ph, float shape)
{
    if (shape < 0.3333f) { float u = shape * 3.f; return han(ph) * (1.f - u) + expo_(1.f - ph, 6.f) * u; }
    if (shape < 0.6667f) { float u = (shape - 0.3333f) * 3.f; return expo_(1.f - ph, 6.f) * (1.f - u) + expo_(ph, 6.f) * u; }
    return expo_(ph, 6.f + (shape - 0.6667f) * 72.f);
}
static void env_plot(int x0, int y0, int w, int h, float shape, uint16_t c)
{
    int prev = -1;
    for (int i = 0; i < w; ++i) {
        float v = shard_env((float)i / (float)(w - 1), shape);
        int y = y0 + h - (int)(v * (float)h);
        if (prev >= 0) { int a = prev < y ? prev : y, bb = prev < y ? y : prev; vline(x0 + i, a, bb - a + 1, c); }
        else px(x0 + i, y, c);
        prev = y;
    }
}
static void draw_knobs(void)
{
    char name[10], val[10];
    hline(1, KNOB_Y, VSC_SCREEN_W - 2, C_FRAME); hline(1, KNOB_Y + 47, VSC_SCREEN_W - 2, C_FRAME);
    for (unsigned i = 0; i <= 4; ++i) vline(i == 4 ? VSC_SCREEN_W - 2 : 1 + (int)i * 60, KNOB_Y, 48, C_FRAME);
    for (unsigned i = 0; i < 4; ++i) {
        int x = (int)i * 60 + 1, y = KNOB_Y, cx = x + 30;
        struct slot s = slot_at(i);
        float bar;
        slot_text(s, name, val, &bar);
        int touched = touch_at[i] && (int32_t)(now_ms - touch_at[i]) < 900;
        if (touched) rect(x + 1, y + 1, 58, 46, C_TOUCH);
        if (!name[0]) continue;
        unsigned t = track, code = 255;
        int mod = -1, depth = 0, robot = -1;
        unsigned mt = track;   /* the modulating track */
        if (s.kind == S_T || s.kind == S_TT || s.kind == S_G) {
            code = slot_code(s, &t);
            /* the route on this knob: the held LFO's modulator first, else the first one */
            if (code != 255) {
                unsigned want = vsc_src_code(t, track, mod_sel);
                for (unsigned r = 0; r < VSC_MOD_ROUTES; ++r) {
                    const struct vsc_route *ro = &vsc.route[t][r];
                    if (ro->param != code || !ro->depth) continue;
                    if (mod < 0 || (mapping && ro->src == want)) { mod = (int)(ro->src & 3u); mt = vsc_src_track(t, ro->src); depth = ro->depth; }
                }
                if (code < P_MOD0) robot = robot_of(t, code);
            }
        }
        int locked = s.kind == S_T && vsc_lock[track][s.id] != 255;
        int enumv = bar < 0.f && (s.kind == S_T || s.kind == S_TT || s.kind == S_G);
        text(cx - text_w(name, 1) / 2, y + 5, name, touched ? C_INK : C_FG, 1);
        if (s.kind == S_T && s.id == P_SHAPE) {
            env_plot(cx - 20, y + 16, 40, 16, bar, C_LAV);   /* SHAPE: its window, drawn */
        } else if (enumv) {
            /* a choice: a lavender badge */
            if (big_w(val) <= 46) { int w = big_w(val) + 6; rect(cx - w / 2, y + 15, w, 19, C_LAV); big(cx - w / 2 + 3, y + 17, val, C_BG); }
            else { int w = text_w(val, 1) + 6; rect(cx - w / 2, y + 18, w, 12, C_LAV); text(cx - w / 2 + 3, y + 21, val, C_BG, 1); }
        } else {
            if (big_w(val) <= 56) big(cx - big_w(val) / 2, y + 17, val, C_INK);
            else text(cx - text_w(val, 1) / 2, y + 21, val, C_INK, 1);
        }
        if (bar >= 0.f) {
            int bipolar = 0;
            if (s.kind == S_T || s.kind == S_TT) bipolar = (vsc_tparam_info(t, s.id)->flags & VSC_PF_BIPOLAR) != 0;
            else if (s.kind == S_G) bipolar = (vsc_gparam_info(s.id)->flags & VSC_PF_BIPOLAR) != 0;
            else if (s.kind == S_MAT || (s.kind == S_STEP && s.id == SF_PITCH)) bipolar = 1;
            int bx = x + 8, bw = 44, by = y + 37;
            rect(bx, by, bw, 4, C_LAVK);
            uint16_t fill = locked ? C_WARN : C_LAV;
            int at = (int)((float)bw * bar + 0.5f);
            if (bipolar) {
                int c = bx + bw / 2;
                if (bx + at >= c) rect(c, by, bx + at - c + 1, 4, fill); else rect(bx + at, by, c - bx - at, 4, fill);
                vline(c, by - 2, 8, C_FG);
            } else rect(bx, by, at, 4, fill);
            /* a modulated parameter shows where its modulator has it now; a robot, where it has it */
            if (mod >= 0 && code < VSC_NP) {
                float v = bar + (float)depth / 127.f * vsc_meter.mod_value[mt][mod];
                v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
                rect(bx + (int)((float)(bw - 2) * v), by - 2, 2, 8, C_MOD[mod]);
            }
            if (robot >= 0 && code < VSC_NP) {
                float v = bar + vsc_robot_off[t][robot] * 127.f / (float)vsc_tparam_info(t, code)->max;
                v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
                rect(bx + (int)((float)(bw - 2) * v), by - 2, 2, 8, C_INK);
            }
        }
        if (mod >= 0) rect(x + 52, y + 3, 4, 4, C_MOD[mod]);    /* the mapped marker, top right */
        if (robot >= 0) { rect(x + 4, y + 3, 4, 4, rob_rec >= 0 && (unsigned)rob_rec == (unsigned)robot && rob_rec_t == t && (now_ms / 200u) & 1u ? C_RED : C_LAV); }
    }
}
static const char *page_hint(void)
{
    switch (page) {
    case PG_HANDS: case PG_SHARDS: case PG_SPECTRA:
        return vsc_meter.src_len[track] ? "HOLD REC: DUB  REC+KNOB: ROBOT" : "REC: CAPTURE, REC AGAIN: END";
    case PG_SOURCE: return "PRESETS: SOURCE  SAVE+REC: KEEP";
    case PG_MIX: return "SELECT: PAGE  REC: CAPTURE";
    case PG_SPACE: return "REC: FREEZE  HOLD REC: CLEAR";
    case PG_DISSECT: return "REC: SHARDS ON HITS";
    case PG_RESON: return row == RV_BAND ? "PADS 1-8: BAND  REC: PING" : "REC: PING  ENV: COLOR";
    case PG_MODS: return row == MATRIX_VIEW ? "REC: BI/UNI  LFO: MODS"
                       : (vsc.t[track][P_MOD(VMOD(row), 0)] & 3) == MT_ADSR ? "PRESETS: TYPE  REC: TRIG"
                       : "PRESETS: TYPE  LFO+KNOB: MAP";
    case PG_SCENE: return "ARP+PAD: SCENE  ARP+KNOB: MAP MACRO";
    case PG_SEQ: return vsc.t[track][P_SEQM] == SEQ_CAROM ? "REC: ADD  HOLD: CLEAR" : "PADS: ON/OFF  OCT: PAGE";
    case PG_PROJECT: case PG_SOUND: return "REC: LOAD  SAVE+PLAY: SAVE";
    case PG_SETTINGS: return row == 0 ? "REC: SCAN/CONNECT" : row == SET_LEARN ? "SAVE+KNOB: LEARN  REC: CLEAR"
                           : row == SET_INPUT ? "REC: OPEN SOURCE" : row == SET_PANEL ? "KEPT THROUGH POWER-OFF" : "SELECT: PAGE";
    default: return "ALGORITHM: TRACK";
    }
}
static const char *rs_msrc(void)   /* RESON's modulator, by its name on the knob */
{
    static char b[8];
    vsc_param_label(vsc_gparam_info(G_BVMOD), G_BVMOD, vsc.g[G_BVMOD], b, 1);
    return b;
}
/* seconds as "0.73S" */
static void secs(char *o, unsigned t)
{
    float r = vsc_meter.src_rate[t] > 0.f ? vsc_meter.src_rate[t] : 22050.f;
    int c = (int)((float)vsc_meter.src_len[t] / r * 100.f + 0.5f);
    num(o, c / 100); unsigned l = (unsigned)vsc_strlen(o);
    o[l++] = '.'; o[l++] = (char)('0' + (c / 10) % 10); o[l++] = (char)('0' + c % 10); o[l++] = 'S'; o[l] = 0;
}
/* the status line: the input, what the page is about, the tempo; a notice or a hint over it */
static void draw_footer(void)
{
    int live = notice && (int32_t)(notice_until - now_ms) > 0;
    static char held[40];
    const char *msg = 0;
    if (live) msg = notice;
    else if (mapping && page != PG_MODS) {
        char b[8]; source_name(track, mod_sel, b);
        copy_n(held, "TURN A KNOB: MOD BY ", 21); copy_n(held + 20, b, 8);
        msg = held;
    } else if ((int32_t)(hint_until - now_ms) > 0) msg = page_hint();
    if (msg) {
        rect(1, FOOT_Y - 6, VSC_SCREEN_W - 2, 19, live ? C_LINE : C_BG);
        text_n(4, FOOT_Y, msg, live ? C_INK : C_LAV, 39);
        return;
    }
    char b[24];
    if (page == PG_RESON) {   /* what the page is about, in a line */
        static const char *const what[7] = {"8 BANDS . WATERY", "EDIT BAND", "CARRIER x MOD . VOCODE", "PING + DUST",
                                            "MOTION . FEEDBACK", "PARTICLES", "MODE . SENS"};
        const char *l = what[row % 7];
        text(120 - text_w(l, 1) / 2, FOOT_Y, l, C_FG, 1);
        return;
    }
    /* the track's input, lit while it has signal */
    unsigned inp = vsc.t[track][P_INPUT];
    if (vsc.t[track][P_BOUNCE] != BOUNCE_OFF) { b[0] = letter(VSC_PARTNER(track)); b[1] = 0; }
    else if (inp >= 3 && inp <= 6) { b[0] = (char)('A' + inp - 3); b[1] = 0; }
    else copy_n(b, inp == 0 ? "BT" : inp == 1 ? "USB" : inp == 2 ? "BT+USB" : "MAIN", 7);
    unsigned l = (unsigned)vsc_strlen(b); copy_n(b + l, " IN", 4);
    int sig = inp == 0 ? vsc_meter.bt_in : inp == 1 ? vsc_meter.usb_in : inp == 2 ? (vsc_meter.bt_in | vsc_meter.usb_in) : 1;
    if (vsc.t[track][P_SRC] == SRC_REEL) copy_n(b, "REEL", 5), sig = 1;
    int x = text(4, FOOT_Y, b, sig ? C_FG : C_DIM, 1);
    vline(x + 4, FOOT_Y - 1, 9, C_DIM); x += 10;
    /* the middle: the pair's lengths, or the page's own */
    if (page == PG_SEQ && vsc.t[track][P_SEQM] == SEQ_STEPS) {
        num(b, vsc.length[track] ? vsc.length[track] : 16); l = (unsigned)vsc_strlen(b); copy_n(b + l, " STEPS", 7);
        x = text(x, FOOT_Y, b, C_FG, 1); vline(x + 4, FOOT_Y - 1, 9, C_DIM); x += 10;
    } else if (page == PG_SHARDS || page == PG_SPECTRA) {
        x = text(x, FOOT_Y, page == PG_SHARDS ? "SHARDS" : "SPECTRA", C_FG, 1); vline(x + 4, FOOT_Y - 1, 9, C_DIM); x += 10;
    }
    if (track_page()) {
        unsigned lo = track & ~1u, n = page == PG_HANDS || page == PG_MIX ? 2 : 1;
        for (unsigned k = 0; k < n; ++k) {
            unsigned t = n == 2 ? lo + k : track;
            if (vsc_meter.src_len[t]) secs(b, t); else copy_n(b, "--", 3);
            l = (unsigned)vsc_strlen(b); b[l++] = ' '; b[l++] = letter(t); b[l] = 0;
            x = text(x, FOOT_Y, b, t == track ? C_FG : C_MID, 1) + 8;
        }
    }
    /* tempo, right */
    int t10 = (int)(vsc_meter.tempo * 10.f + 0.5f);
    num(b, t10 / 10); l = (unsigned)vsc_strlen(b); b[l] = '.'; b[l + 1] = (char)('0' + t10 % 10); copy_n(b + l + 2, " BPM", 5);
    uint16_t tc = vsc_meter.ext_clock && vsc.g[G_CLOCK] != CLOCK_INT ? C_WARN : C_FG;
    int bx = VSC_SCREEN_W - 4 - text_w(b, 1);
    if (bx > x) text(bx, FOOT_Y, b, tc, 1);
    if (vsc_meter.playing && ((uint32_t)(now_ms * vsc_meter.tempo / 60000.f) & 1u) == 0) rect(bx - 6, FOOT_Y + 2, 3, 3, C_RED);
}

/* the modulator's picture: 0..1 per column, in 1/255ths (RAM is short) */
static uint8_t curve_q[VSC_SCREEN_W];
#define CGET(x) ((float)curve_q[x] * (1.f / 255.f))
static void cset(int x, float v) { curve_q[x] = (uint8_t)(v <= 0.f ? 0.f : v >= 1.f ? 255.f : v * 255.f + 0.5f); }
static int adsr_node[4][2];             /* the ADSR picture's corners: x, level 0..254 */

/* ----------------------------------------------------- the putty pictures */
static unsigned col(unsigned x, unsigned x0, unsigned w) { return (x - x0) * 240u / w; }   /* screen column -> overview column */
/* a track's waveform in a row: the window bright, the playhead, a red line while it records */
static void wave_row(unsigned t, int x0, int mid, int amp, int sel)
{
    int w = VSC_SCREEN_W - 2 - x0;
    int len = (int)vsc_meter.src_len[t];
    int a = x0 + (int)vsc_meter.win_a[t] * w / 65536, bnd = x0 + (int)vsc_meter.win_b[t] * w / 65536;
    int wrap = bnd < a, cap = vsc_meter.cap_state[t];
    if (len && sel) {
        /* the window: a lavender pane, outlined */
        if (!wrap) { rect(a, mid - amp - 3, bnd - a + 1, amp * 2 + 7, C_LAVK); hline(a, mid - amp - 3, bnd - a + 1, C_LAV); hline(a, mid + amp + 3, bnd - a + 1, C_LAV); vline(a, mid - amp - 3, amp * 2 + 7, C_LAV); vline(bnd, mid - amp - 3, amp * 2 + 7, C_LAV); }
        else { rect(x0, mid - amp - 3, bnd - x0 + 1, amp * 2 + 7, C_LAVK); rect(a, mid - amp - 3, x0 + w - a, amp * 2 + 7, C_LAVK); vline(a, mid - amp - 3, amp * 2 + 7, C_LAV); vline(bnd, mid - amp - 3, amp * 2 + 7, C_LAV); }
    } else if (len) { hline(wrap ? x0 : a, mid + amp + 3, (wrap ? x0 + w : bnd) - (wrap ? x0 : a), C_LAVD); }
    for (int x = x0; x < x0 + w; ++x) {
        int h = vsc_meter.overview[t][col((unsigned)x, (unsigned)x0, (unsigned)w)] * amp / 60;
        int in = wrap ? (x >= a || x <= bnd) : (x >= a && x <= bnd);
        uint16_t c = !len ? C_LINE : sel ? (in ? C_INK : C_MID) : (in ? C_MID : C_DIM);
        if (h) vline(x, mid - h, h * 2 + 1, c); else px(x, mid, len ? C_LINE : C_PANEL);
    }
    if (len) {
        int hx = x0 + (int)vsc_meter.head[t] * w / 65536;
        vline(hx, mid - amp - 2, amp * 2 + 5, sel ? C_LAV : C_LAVD);
        if (sel) vline(hx + 1, mid - amp - 2, amp * 2 + 5, C_LAVD);
    }
    if (cap == 1 || cap == 3) {
        int rx = x0 + (int)vsc_meter.rec_head[t] * w / 65536;
        vline(rx, mid - amp - 4, amp * 2 + 9, C_RED);
    }
    if (!len) {
        unsigned src = vsc.t[t][P_SRC];
        const char *m = src == SRC_REEL ? (vsc_reel_index() ? "EMPTY SLOT" : "NO REEL: SAVE+REC KEEPS") : cap == 1 ? "LISTENING" : sel ? "EMPTY: TAP REC" : "EMPTY";
        text(x0 + w / 2 - text_w(m, 1) / 2, mid - 3, m, cap == 1 ? C_RED : C_DIM, 1);
    }
}
/* SHARDS: a cloud of dust the shards throw up, kept a few frames */
#define DUST 160
static uint8_t dust_x[DUST], dust_y[DUST], dust_a[DUST], dust_c[DUST]; static unsigned dust_w;
static uint8_t tick_x[8], tick_a[8]; static unsigned tick_w;
static uint16_t shard_seen[VSC_GRAINS];
static uint32_t rng = 0x1234567u;
static unsigned rnd8(void) { rng = rng * 1664525u + 1013904223u; return rng >> 24; }
static void shards_frame(void)
{
    unsigned t = track;
    for (unsigned i = 0; i < DUST; ++i) if (dust_a[i]) ++dust_a[i], dust_a[i] = dust_a[i] > 14 ? 0 : dust_a[i];
    for (unsigned i = 0; i < 8; ++i) if (tick_a[i]) tick_a[i] = tick_a[i] > 20 ? 0 : (uint8_t)(tick_a[i] + 1);
    float spray = (float)vsc.t[t][P_SPRAY] / 127.f, shape = (float)vsc.t[t][P_SHAPE] / 127.f;
    for (unsigned g = 0; g < VSC_GRAINS; ++g) {
        uint16_t p = vsc_meter.shard_pos[t][g];
        if (p == 0xffff) { shard_seen[g] = 0xffff; continue; }
        int x = p * 239 / 65535;
        float ph = (float)vsc_meter.shard_amp[t][g] / 255.f, env = shard_env(ph, shape);
        if (shard_seen[g] == 0xffff || vsc_meter.shard_amp[t][g] < 24) {   /* born: a tick on top */
            if (shard_seen[g] == 0xffff) { tick_x[tick_w] = (uint8_t)x; tick_a[tick_w] = 1; tick_w = (tick_w + 1) & 7; }
        }
        shard_seen[g] = p;
        unsigned n = 4 + (unsigned)(env * 10.f);
        for (unsigned k = 0; k < n; ++k) {
            int dx = (int)(rnd8() % (unsigned)(5 + (int)(spray * 18.f))) - (2 + (int)(spray * 9.f));
            int dy = (int)((float)((int)rnd8() - 128) * (0.15f + env * 0.85f) * 0.27f);
            int xx = x + dx; if (xx < 0) xx = 0; if (xx > 239) xx = 239;
            dust_x[dust_w] = (uint8_t)xx; dust_y[dust_w] = (uint8_t)(dy + 128); dust_a[dust_w] = 1; dust_c[dust_w] = (uint8_t)(g & 1);
            dust_w = (dust_w + 1) % DUST;
        }
    }
}
static void draw_shards(void)
{
    unsigned t = track;
    int mid = VIS_Y + 50;
    /* the source, faint */
    for (int x = 0; x < VSC_SCREEN_W; x += 3) { int h = vsc_meter.overview[t][x] * 30 / 60; px(x, mid + h, C_LINE); px(x, mid - h, C_LINE); }
    if (!vsc_meter.src_len[t]) text(120 - text_w("EMPTY: TAP REC", 1) / 2, mid - 3, "EMPTY: TAP REC", C_DIM, 1);
    for (unsigned i = 0; i < 8; ++i)
        if (tick_a[i]) vline(tick_x[i], VIS_Y + 2, 9, tick_a[i] < 6 ? C_INK : tick_a[i] < 12 ? C_LAV : C_LAVD);
    for (unsigned i = 0; i < DUST; ++i) {
        if (!dust_a[i]) continue;
        int a = dust_a[i];
        uint16_t c = a < 3 ? C_INK : dust_c[i] ? (a < 7 ? C_LAV : C_LAVD) : (a < 7 ? C_FG : C_DIM);
        int y = mid + (int)dust_y[i] - 128;
        px(dust_x[i], y, c);
        if (a < 5) { px(dust_x[i] + 1, y, c); px(dust_x[i], y + 1, c); }
    }
    /* the shards' window, one per beat of the picture */
    float shape = (float)vsc.t[t][P_SHAPE] / 127.f;
    int y0 = VIS_Y + 86, h = 26;
    for (int k = 0; k < 5; ++k) env_plot(6 + k * 46, y0, 44, h, shape, C_LAV);
    hline(4, y0 + h + 1, VSC_SCREEN_W - 8, C_LAVD);
    static const char *const tn[4] = {"FREE", "ON HITS", "ON NOTES", "ON CLOCK"};
    text(VSC_SCREEN_W - 4 - text_w(tn[vsc.t[t][P_TRIG] & 3], 1), VIS_Y + 2, tn[vsc.t[t][P_TRIG] & 3], C_DIM, 1);
}
/* SPECTRA: the spectrum through time, 64 columns of 48 bands at 4 bits */
#define SPEC_COLS 48
static uint8_t spec_hist[SPEC_COLS][24]; static unsigned spec_w; static uint8_t spec_track = 255;
static uint16_t spec_pal[16];
static unsigned spec_lv(unsigned v)   /* 0..255 -> 0..15, about 3 dB a step */
{
    unsigned l = 0;
    while (v > 1u) { v >>= 1; l += 2; }
    if (v) ++l;
    return l > 15u ? 15u : l;
}
static void spectra_frame(void)
{
    unsigned t = track;
    if (spec_track != t) { vsc_memset(spec_hist, 0, sizeof spec_hist); spec_track = (uint8_t)t; }
    for (unsigned b = 0; b < 24; ++b) {
        unsigned lo = spec_lv(vsc_meter.spec[t][2 * b]), hi = spec_lv(vsc_meter.spec[t][2 * b + 1]);
        spec_hist[spec_w][b] = (uint8_t)(lo | hi << 4);
    }
    spec_w = (spec_w + 1) % SPEC_COLS;
    if (!spec_pal[15]) {
        for (int i = 0; i < 16; ++i)
            spec_pal[i] = i < 1 ? C_BG : i < 6 ? mix565(C_BG, C_LAVD, i * 256 / 6) : i < 11 ? mix565(C_LAVD, C_LAV, (i - 6) * 256 / 5)
                        : mix565(C_LAV, C_INK, (i - 11) * 256 / 5);
    }
}
static void draw_spectra(void)
{
    unsigned t = track;
    int top = VIS_Y + 2, bot = VIS_Y + 116, h = bot - top;
    for (int r = 0; r < VSC_BAND; ++r) {
        int y = by0 + r;
        if (y < top || y >= bot) continue;
        int b = (bot - 1 - y) * 48 / h;   /* band, low at the bottom */
        if (((bot - 1 - y) * 48 / h) != ((bot - y) * 48 / h) && (y & 1)) continue;   /* a hair between bands */
        uint16_t *row_px = band + r * VSC_SCREEN_W;
        for (int c = 0; c < SPEC_COLS; ++c) {
            unsigned col_i = (spec_w + (unsigned)c) % SPEC_COLS;
            unsigned v = (spec_hist[col_i][b >> 1] >> ((b & 1) * 4)) & 15u;
            if (!v) continue;
            int x0 = c * VSC_SCREEN_W / SPEC_COLS, x1 = (c + 1) * VSC_SCREEN_W / SPEC_COLS;
            for (int x = x0; x < x1; ++x) row_px[x] = spec_pal[v];
        }
    }
    if (!vsc_meter.spectra_on[t] && vsc.t[t][P_ENGINE] == ENG_SPECTRA)
        text(6, VIS_Y + 50, "2 SPECTRA: HANDS", C_WARN, 1);
    else if (!vsc_meter.src_len[t]) text(78, VIS_Y + 50, "EMPTY: TAP REC", C_DIM, 1);
    /* where in the source it is */
    int hx = (int)vsc_meter.head[t] * 236 / 65536 + 2;
    hline(2, bot + 2, 236, C_LAVK); rect(hx - 1, bot + 1, 3, 3, C_LAV);
}
/* SEQ: the steps as blocks, the bounces between those that play, the source below */
static void draw_seq(void)
{
    unsigned t = track, len = vsc.length[t] ? vsc.length[t] : 16;
    int base = VIS_Y + 34, arc_y = VIS_Y + 64;
    int prev_x = -1;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned s = step_page * 16 + i;
        int x = 2 + (int)i * 15, cx = x + 6;
        const struct vsc_step *st = &vsc.step[t][s];
        if (s >= len) continue;
        int h = 8 + st->prob * 14 / 100;
        uint16_t c = !st->on ? C_PANEL : st->slice ? C_LAV : C_FG;
        rect(x, base - h, 13, h, c);
        if (st->lp[0] < VSC_NP) rect(x + 9, base - h + 2, 2, 2, st->on ? C_BG : C_MID);
        if (s == step_sel) { hline(x - 1, base + 2, 15, C_INK); hline(x - 1, base - h - 3, 15, C_INK); }
        if (vsc_meter.playing && vsc_meter.step[t] == s) rect(x, base - h - 7, 13, 2, C_RED);
        if (st->on) {
            /* a bounce from the last step that plays */
            if (prev_x >= 0) {
                int span = cx - prev_x, ah = 6 + span / 3;
                if (ah > 24) ah = 24;
                for (int xx = prev_x; xx <= cx; ++xx) {
                    float u = (float)(xx - prev_x) / (float)span;
                    px(xx, arc_y - (int)(4.f * u * (1.f - u) * (float)ah), C_FG);
                }
            }
            rect(cx - 1, arc_y - 1, 3, 3, st->slice ? C_LAV : C_FG);
            prev_x = cx;
        }
    }
    /* the source, filled lavender, with the steps' slices marked in the window */
    int y0 = VIS_Y + 80, hh = 30, wl = (int)vsc_meter.win_a[t] * 236 / 65536, wb = (int)vsc_meter.win_b[t] * 236 / 65536;
    for (int x = 2; x < 238; ++x) {
        int h = vsc_meter.overview[t][col((unsigned)x, 2u, 236u)] * hh / 60;
        if (h) vline(x, y0 + hh - h, h, C_LAVD);
        px(x, y0 + hh - h, C_LAV);
    }
    hline(2, y0 + hh, 236, C_LAV);
    if (wb > wl)
        for (unsigned i = 0; i < 16; ++i) {
            const struct vsc_step *st = &vsc.step[t][step_page * 16 + i];
            if (!st->on || !st->slice) continue;
            int sx = 2 + wl + (st->slice - 1) * (wb - wl) / 16;
            vline(sx, y0 + 4, hh - 4, step_page * 16 + i == step_sel ? C_INK : C_FG);
        }
    if (vsc_meter.src_len[t]) { int hx = 2 + (int)vsc_meter.head[t] * 236 / 65536; vline(hx, y0, hh + 1, C_INK); }
    char b[16] = "PAGE  /4"; b[5] = (char)('1' + step_page);
    text(VSC_SCREEN_W - 4 - text_w(b, 1), VIS_Y + 112, b, C_DIM, 1);
    if (live_rec) text(4, VIS_Y + 112, "RECORDING", C_RED, 1);
}
static void draw_source(void)
{
    unsigned t = track;
    wave_row(t, 4, VIS_Y + 34, 30, 1);
    static const char *const sn[3] = {"CAPTURE", "REEL", "THRU (LIVE)"};
    static const char *const st[4] = {"EMPTY", "RECORDING", "PLAYING", "OVERDUB"};
    static const char *const rn[3] = {"44K", "22K", "11K"};
    char b[32];
    unsigned src = vsc.t[t][P_SRC];
    text(4, VIS_Y + 74, sn[src % 3], C_INK, 1);
    if (src == SRC_CAPTURE) {
        unsigned cs = vsc_meter.cap_state[t] & 3;
        text(70, VIS_Y + 74, st[cs], cs == 1 || cs == 3 ? C_RED : C_FG, 1);
    }
    copy_n(b, rn[vsc.t[t][P_RATE] % 3], 4); text(150, VIS_Y + 74, b, C_MID, 1);
    if (vsc_meter.src_len[t]) { secs(b, t); text(190, VIS_Y + 74, b, C_FG, 1); }
    /* the reel: how full, how many splices */
    const struct vsc_reel *r = vsc_reel_index();
    unsigned n = r ? r->count : 0;
    copy_n(b, "REEL ", 6); num(b + 5, (int)n); { unsigned l = (unsigned)vsc_strlen(b); copy_n(b + l, n == 1 ? " SPLICE" : " SPLICES", 9); }
    text(4, VIS_Y + 92, b, C_DIM, 1);
    int used = 100 - vsc_host.reel_free;
    rect(4, VIS_Y + 104, 232, 5, C_LAVK); rect(4, VIS_Y + 104, used * 232 / 100, 5, C_LAV);
    if (r) for (unsigned i = 1; i < n && i < VSC_SPLICES; ++i) {
        uint32_t tot = r->s[n - 1].at + r->s[n - 1].frames;
        if (tot) vline(4 + (int)((uint64_t)r->s[i].at * 232u / tot * (unsigned)used / 100u), VIS_Y + 103, 7, C_BG);
    }
    if (vsc.t[t][P_BOUNCE] != BOUNCE_OFF) { char c[24] = "CAPTURES "; c[9] = letter(VSC_PARTNER(t)); c[10] = 0; copy_n(c + 10, vsc.t[t][P_BOUNCE] == BOUNCE_PING ? " (PING)" : "'S OUTPUT", 10); text(4, VIS_Y + 112, c, C_LAV, 1); }
}
/* RESON: eight bars that glow with what each band puts out (stacked, as the drawings),
 * their gains as faint stacks behind, a reflection under the water line; the band
 * being edited in a lavender pill. BAND opens a window on that band's curve; VOCODE
 * shows the carrier and the modulator meeting; PING throws sparks off the bars. */
#define RB_X(b) (14 + (int)(b) * 28)
#define RB_BASE (VIS_Y + 84)
static uint8_t spark_x[24], spark_y[24], spark_a[24]; static unsigned spark_w;
static void reson_frame(void)
{
    for (unsigned i = 0; i < 24; ++i) if (spark_a[i]) spark_a[i] = spark_a[i] > 14 ? 0 : (uint8_t)(spark_a[i] + 1);
    if (vsc_meter.rb_pings != rb_seen) {
        rb_seen = vsc_meter.rb_pings; rb_flash_at = now_ms;
        for (unsigned b = 0; b < VSC_RBANDS; ++b)
            if (vsc.g[G_BG0 + b] > 60)
                for (unsigned k = 0; k < 2; ++k) { spark_x[spark_w] = (uint8_t)(RB_X(b) + 4 + (int)(rnd8() % 14u)); spark_y[spark_w] = (uint8_t)(rnd8() % 30u); spark_a[spark_w] = 1; spark_w = (spark_w + 1) % 24; }
    }
}
static void draw_reson(void)
{
#ifdef VSC_BENCH
    return;   /* (the RESON benchmark build has no room for the drawing) */
#endif
    const int base = RB_BASE;
    unsigned t = vsc.g[G_BTRACK] ? vsc.g[G_BTRACK] - 1u : track;
    int flash = (int32_t)(now_ms - rb_flash_at) < 120;
    for (unsigned b = 0; b < VSC_RBANDS; ++b) {
        int x = RB_X(b), w = 20;
        int gh = vsc.g[G_BG0 + b] * 54 / 127, lh = vsc_meter.rb_level[b] * 54 / 255;
        if (lh > 54) lh = 54;
        for (int y = 0; y < gh; y += 3) hline(x, base - 1 - y, w, C_LAVK);                    /* the gain, faint */
        for (int y = 0; y < lh; y += 3) hline(x, base - 1 - y, w, y > lh - 5 ? C_INK : flash && gh > 40 ? C_FG : C_LAV);
        if (lh > 2) hline(x - 1, base - lh - 1, w + 2, C_INK);                                   /* the glowing cap */
        for (int y = 0; y < lh / 3; y += 3) hline(x, base + 3 + y, w, C_LAVD);                   /* the reflection */
        if (gh < 6) hline(x + 2, base - 2, w - 4, C_LAVD);
        char n[2] = {(char)('1' + b), 0};
        int sel = b == rb_sel && (row == RV_BAND || page == PG_RESON);
        if (sel && row == RV_BAND) { rect(x + 3, base + 8, 14, 11, C_LAV); text(x + 7, base + 10, n, C_BG, 1); }
        else text(x + 7, base + 10, n, b == rb_sel ? C_FG : C_DIM, 1);
    }
    hline(4, base + 1, 232, C_LAVD);
    if (row == RV_BAND) {   /* the band's window: its curve, its tuning and decay */
        int bx = 132, by = VIS_Y + 2, bw = 104, bh = 40;
        rect(bx, by, bw + 1, bh + 1, C_LAV); rect(bx + 1, by + 1, bw - 1, bh - 1, C_BG);
        char b2[12] = "BAND  "; b2[5] = (char)('1' + rb_sel); text(bx + 4, by + 3, b2, C_FG, 1);
        int sq = 400 - vsc.g[G_BWIDTH] * 3;   /* the curve, narrower for less WIDTH */
        for (int i = -28; i < 28; ++i) px(bx + 32 + i, by + bh - 4 - 24 * 784 / (784 + i * i * sq / 64), C_LAV);
        vline(bx + 32, by + 12, bh - 15, C_LAVD);
        char v[10]; int hz = (int)(vsc_reson_hz(rb_sel) + 0.5f); num(v, hz); copy_n(v + vsc_strlen(v), "HZ", 3); text(bx + 64, by + 12, v, C_INK, 1);
        int ms = (int)(vsc_reson_sec(rb_sel) * 1000.f + 0.5f); num(v, ms > 9999 ? 9999 : ms); copy_n(v + vsc_strlen(v), "MS", 3); text(bx + 64, by + 30, v, C_INK, 1);
        vline(RB_X(rb_sel) + 10, by + bh, base - 56 - by - bh > 0 ? base - 56 - by - bh : 1, C_LAV);
    } else if (row == RV_VOC) {   /* the carrier and the modulator, meeting */
        unsigned vm = vsc.g[G_BVMOD] % 6, u = vm == 2 ? VSC_PARTNER(t) : t;
        text(30, VIS_Y + 1, "CARRIER", C_DIM, 1); text(150, VIS_Y + 1, "MODULATOR", C_DIM, 1);
        for (int x = 0; x < 224; ++x) {   /* the carrier, then the modulator */
            int h = x < 104 ? vsc_meter.overview[t][x * 2] * 8 / 60 : vm == 0 || vm == 2 ? vsc_meter.overview[u][x * 2 - 210] * 8 / 60 : (int)(rnd8() % (vm == 4 ? 9u : 4u));
            if (x < 104 || x >= 120) vline(8 + x, VIS_Y + 18 - h, 2 * h + 1, C_LAVD);
        }
        text(150, VIS_Y + 30, rs_msrc(), C_LAV, 1);
        text(117, VIS_Y + 14, "X", C_INK, 1);
    }
    if (row == RV_PING || vsc.g[G_BDUST])   /* sparks off the bars on each ping */
        for (unsigned i = 0; i < 24; ++i) if (spark_a[i]) {
            int y = base - 58 - spark_y[i] / 2 + spark_a[i] * 2, x = spark_x[i];
            uint16_t c = spark_a[i] < 4 ? C_INK : spark_a[i] < 9 ? C_LAV : C_LAVD;
            px(x, y, c); if (spark_a[i] < 4) px(x, y - 1, c);
        }
    if (!vsc.g[G_BMODE]) text(96, VIS_Y + 40, "MODE OFF", C_WARN, 1);
}
static void draw_vis(void)
{
    unsigned t = track;
    switch (page) {
    case PG_HANDS: {
        /* the pair, the open track brighter: A above B, C above D */
        unsigned lo = t & ~1u;
        char l[2] = {0, 0};
        for (unsigned k = 0; k < 2; ++k) {
            unsigned u = lo + k; int mid = k ? VIS_Y + 92 : VIS_Y + 36, amp = u == t ? 28 : 18;
            l[0] = letter(u); text(2, mid - 7, l, u == t ? C_INK : C_DIM, 2);
            wave_row(u, 18, mid, amp, u == t);
        }
        break;
    }
    case PG_SHARDS: draw_shards(); break;
    case PG_RESON: draw_reson(); break;
    case PG_SPECTRA: draw_spectra(); break;
    case PG_SOURCE: draw_source(); break;
    case PG_SEQ: if (vsc.t[track][P_SEQM] == SEQ_STEPS) { draw_seq(); break; }
{
        int x0 = 20, y0 = VY(4), w = 200, h = 128;
        rect(x0, y0, w + 1, h + 1, C_LINE); rect(x0 + 1, y0 + 1, w - 1, h - 1, C_BG);
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
        if (!count) text(x0 + 14, y0 + h / 2 - 3, live_rec ? "PLAY NOTES" : "NO STEPS: REC, PLAY", C_DIM, 1);
        else if (!vsc_meter.playing) text(x0 + 4, y0 + h + 4, "PRESS PLAY", C_DIM, 1);
        if (live_rec) text(x0 + w - 54, y0 + h + 4, "RECORDING", C_WARN, 1);
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
        int cx = 120, cy = VY(72);
        float R = 46.f + lvl * 22.f, sx = 1.f + comp * 0.35f, sy = 1.f - comp * 0.45f;
        unsigned frame = now_ms / 70;
        int redux = vsc.t[t][P_CMODE] != 0 && crush > 0.02f;
        for (int yy = VY(4); yy < VY(142); yy += step) {
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
    case PG_DISSECT: {
        /* the listener: its three bands, the last few seconds of level with each hit a
         * bright stroke, the pitch it hears (note, Hz, how sure) and its partials */
        static const char *const bn[3] = {"LOW", "MID", "HIGH"};
        const float bv[3] = {vsc_meter.ana_low, vsc_meter.ana_mid, vsc_meter.ana_high};
        if (!vsc_meter.ana_on) text(4, VY(2), "LISTENER OFF", C_DIM, 1);
        else { char b[16]; copy_n(b, "HITS ", 6); num(b + 5, (int)(vsc_meter.ana_hits % 100000u)); text(4, VY(2), b, C_MID, 1); }
        { int f = (int)(vsc_meter.ana_hit * 8.f); if (f > 0) rect(226, VY(1), 10, 10, f > 4 ? C_INK : C_FG); else rect(226, VY(1), 10, 10, C_LINE); }
        for (int b = 0; b < 3; ++b) {
            int y = VY(16) + b * 10;
            text(4, y, bn[b], C_DIM, 1);
            rect(34, y + 1, 200, 5, C_PANEL);
            float v = bv[b] > 1.f ? 1.f : bv[b];
            rect(34, y + 1, (int)(v * 200.f), 5, C_FG);
        }
        int base = VY(98);
        for (int xx = 0; xx < VSC_SCREEN_W; ++xx) {
            unsigned i = (hist_w + (unsigned)(xx * 96 / VSC_SCREEN_W)) % 96;
            int h = dis_lev[i] * 40 / 255;
            if (h) vline(xx, base - h, h, C_LINE);
            if (dis_hit[i] && (xx * 96 / VSC_SCREEN_W) != ((xx - 1) * 96 / VSC_SCREEN_W)) {
                int hh = dis_hit[i] * 44 / 255;
                vline(xx, base - hh, hh, C_INK); vline(xx + 1, base - hh, hh, C_FG);
            }
        }
        hline(0, base, VSC_SCREEN_W, C_LINE);
        float hz = vsc_meter.ana_pitch, conf = vsc_meter.ana_conf;
        if (hz > 0.f && conf > 0.05f) {
            /* the note: 69 + 12 log2(hz / 440) */
            union { float f; uint32_t u; } v; v.f = hz / 440.f;
            float l2 = (float)((int)(v.u >> 23) - 127) + ((float)(v.u & 0x7fffff) / 8388608.f) * 0.98f;
            int note = (int)(69.f + 12.f * l2 + (69.f + 12.f * l2 >= 0.f ? 0.5f : -0.5f));
            static const char *const nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            char b[12]; copy_n(b, nn[((note % 12) + 12) % 12], 3);
            unsigned l = (unsigned)vsc_strlen(b); num(b + l, note / 12 - 1);
            text(4, VY(106), b, conf > 0.5f ? C_INK : C_DIM, 2);
            char hzs[12]; num(hzs, (int)(hz + 0.5f)); l = (unsigned)vsc_strlen(hzs); copy_n(hzs + l, " HZ", 4);
            text(4, VY(126), hzs, C_MID, 1);
            rect(4, VY(138), 100, 3, C_PANEL); rect(4, VY(138), (int)(conf * 100.f), 3, conf > 0.5f ? C_FG : C_DIM);
        } else text(4, VY(110), "NO PITCH", C_DIM, 1);
        for (int h = 0; h < 8; ++h) {
            float a = vsc_meter.ana_harm[h] * 3.f; if (a > 1.f) a = 1.f;
            int hh = (int)(a * 28.f), xx = 122 + h * 14;
            rect(xx, VY(140) - 28, 10, 28, C_PANEL);
            if (hh) rect(xx, VY(140) - hh, 10, hh, h ? C_FG : C_INK);
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
        int cx = 66, cy = VY(72);
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
            px(176 + dx + drift / 2, VY(72) + dy - drift / 3, (i & 3) ? C_MID : C_FG);
        }
        if (frozen) text(4, VY(4), "FROZEN", C_WARN, 1);
        break;
    }
    case PG_MODS:
        if (row == MATRIX_VIEW) {
            /* the matrix: every route on every track, each a modulator (its colour) moving a
             * parameter; the bar is the AMOUNT, the bright tick where it has the parameter now */
            unsigned count = matrix_count(), first = route_sel > 7 ? route_sel - 7 : 0;
            for (unsigned i = first; i <= count && i < first + 9; ++i) {
                int y = VY(4) + (int)(i - first) * 15;
                int sel = i == route_sel;
                if (sel) rect(0, y - 3, VSC_SCREEN_W, 13, C_PANEL);
                unsigned tt; const struct vsc_route *ro = matrix_at(i, &tt);
                if (!ro) { text(4, y, "+ NEW ROUTE", sel ? C_INK : C_LINE, 1); continue; }
                unsigned st = vsc_src_track(tt, ro->src), m = ro->src & 3u;
                char b[16];
                rect(4, y, 3, 7, C_MOD[m]);
                source_name(tt, ro->src, b);
                text(10, y, b, sel ? C_INK : C_MID, 1);
                text(54, y, ">", C_LINE, 1);
                if (ro->param >= VSC_GDEST) { dev_name(dev_of(tt, ro->param), b); b[2] = 0; text(62, y, b[0] == 'P' ? "PF" : b[0] == 'M' ? "MS" : "SP", C_DIM, 1); }
                else { b[0] = 'T'; b[1] = (char)('1' + tt); b[2] = 0; text(62, y, b, C_DIM, 1); }
                dest_name(tt, ro->param, b);
                text_n(78, y, b, sel ? C_INK : C_FG, 10);
                /* amount, centred, and the live position */
                int cx = 196, half = 38, w = ro->depth * half / 127;
                hline(cx - half, y + 3, half * 2 + 1, C_LINE);
                vline(cx, y, 7, C_LINE);
                if (w > 0) rect(cx, y + 2, w, 3, C_MID); else if (w < 0) rect(cx + w, y + 2, -w, 3, C_MID);
                float mv = vsc_meter.mod_value[st][m];
                if (ro->flags & 1) mv = mv * 2.f - 1.f;
                int lx = cx + (int)((float)w * mv);
                rect(lx - 1, y, 2, 7, C_MOD[m]);
            }
            if (!count) {
                text(4, VY(40), "NO ROUTES: HOLD LFO", C_DIM, 1);
                text(4, VY(52), "+ TURN A KNOB", C_DIM, 1);
            }
            break;
        }
        {
            /* the modulator's shape, filled up to where it is now, as on the S-4 */
            unsigned m = VMOD(row), ty = vsc.t[track][P_MOD(m, 0)] & 3;
            int base = VY(140), h = 100;
            float ph = vsc_meter.mod_phase[track][m];
            int phx = ty == MT_WAVE || ty == MT_RANDOM ? (int)(ph * 239.f) : 239;
            int prev = -1;
            for (int xx = 0; xx < VSC_SCREEN_W; ++xx) {
                float v = CGET(xx);
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
            if (ty == MT_FOLLOW) for (int xx = 0; xx < VSC_SCREEN_W; xx += 2) vline(xx, base - (int)(CGET(xx) * (float)h), (int)(CGET(xx) * (float)h), C_LINE);
            /* the four slots along the top: type and live value */
            for (unsigned k = 0; k < VSC_MODS; ++k) {
                int x0 = 4 + (int)k * 59;
                char lab[10]; copy_n(lab, mtype_name[vsc.t[track][P_MOD(k, 0)] & 3], 6);
                text(x0, VY(2), lab, k == m ? C_MOD[k] : C_DIM, 1);
                rect(x0, VY(11), (int)(vsc_meter.mod_value[track][k] * 50.f), 2, k == m ? C_MOD[k] : C_LINE);
            }
        }
        break;
    case PG_MIX:
        for (unsigned k = 0; k < VSC_TRACKS; ++k) {
            int x = 8 + (int)k * 58;
            char lab[4] = {letter(k), 0, 0, 0};
            text(x, VIS_Y + 2, lab, k == track ? C_INK : C_DIM, 2);
            static const char *const eng[3] = {"HANDS", "SHARDS", "SPECTR"};
            text(x + 16, VIS_Y + 6, eng[vsc.t[k][P_ENGINE] % 3], (k & 1) ? C_LAV : C_MID, 1);
            int hgt = (int)(vsc_meter.peak[k] * 80.f); if (hgt > 80) hgt = 80;
            rect(x, VIS_Y + 30, 10, 80, C_PANEL);
            rect(x, VIS_Y + 110 - hgt, 10, hgt, vsc.t[k][P_MUTE] ? C_DIM : vsc_meter.peak[k] > 0.98f ? C_RED : C_FG);
            int lv = vsc.t[k][P_LEVEL] * 80 / 127;
            hline(x + 13, VIS_Y + 110 - lv, 8, C_LAV);
            if (vsc.t[k][P_MUTE]) text(x + 14, VIS_Y + 30, "M", C_WARN, 1);
        }
        break;
    case PG_SCENE:
        for (unsigned s = 0; s < VSC_SCENES; ++s) {
            int x = 6 + (int)(s % 4) * 58, y = VY(4) + (int)(s / 4) * 34;
            uint16_t c = vsc.scene[s].used ? (s == vsc.g[G_TARGET] ? C_INK : C_MID) : C_LINE;
            rect(x, y, 50, 26, C_PANEL);
            char lab[4] = {'S', (char)('1' + s), 0, 0};
            text(x + 4, y + 9, lab, c, 1);
            if (s == vsc.g[G_TARGET]) hline(x, y + 25, 50, C_FG);
        }
        for (unsigned m = 0; m < VSC_MACROS; ++m) {
            int x0 = m < 4 ? 6 : 124, y = VY(80) + (int)(m & 3) * 14;
            char lab[4] = {'M', (char)('1' + m), 0, 0};
            text(x0, y, lab, m == vsc.g[G_MSEL] ? C_INK : C_DIM, 1);
            rect(x0 + 16, y + 2, 92, 3, C_PANEL);
            rect(x0 + 16, y + 2, vsc.g[G_M0 + m] * 92 / 127, 3, C_FG);
        }
        break;
    case PG_PROJECT:
        for (unsigned s = 0; s < VSC_PROJECTS; ++s) {
            int y = VIS_Y + 2 + (int)s * 14;
            char lab[16] = {(char)('1' + s), ' ', 0};
            if (s == proj_slot) rect(0, y - 3, VSC_SCREEN_W, 14, C_PANEL);
            text(6, y, lab, s == proj_slot ? C_INK : C_DIM, 1);
            text_n(24, y, vsc_host.slot_used[s] ? vsc_host.slot_name[s] : "EMPTY", vsc_host.slot_used[s] ? C_FG : C_LINE, 35);
        }
        if (!vsc_host.storage_ok) text(6, VIS_Y + 114, "STORAGE NOT AVAILABLE", C_WARN, 1);
        break;
    case PG_SOUND: {
        /* eight at a time; a sound is SOURCE to MIX and the modulators of one track */
        static const char *const src_n[] = {"CAPTURE", "REEL", "THRU"};
        static const char *const eng_n[] = {"HANDS", "SHARDS", "SPECTRA"};
        unsigned first = sound_slot / 8u * 8u;
        for (unsigned s = first; s < first + 8u; ++s) {
            int y = VIS_Y + 2 + (int)(s - first) * 14;
            const struct vsc_sound *so = &vsc_dev.sound[s];
            char lab[4] = {(char)('0' + (s + 1) / 10), (char)('0' + (s + 1) % 10), 0};
            if (s == sound_slot) rect(0, y - 3, VSC_SCREEN_W, 13, C_PANEL);
            text(6, y, lab, s == sound_slot ? C_INK : C_DIM, 1);
            if (!so->used) { text(30, y, "EMPTY", C_LINE, 1); continue; }
            text(30, y, eng_n[so->p[P_ENGINE] % 3], C_FG, 1);
            text(90, y, src_n[so->p[P_SRC] % 3], C_DIM, 1);
        }
        char tx[24] = "REC LOADS ONTO TRACK "; tx[21] = letter(track); tx[22] = 0;
        text(6, VIS_Y + 114, tx, C_DIM, 1);
        break;
    }
    case PG_SETTINGS: if (row == SET_INPUT) {
        /* AUDIO IN: every way sound gets in, in one place. The FM-1 has no analogue input. */
        const char *bt = !vsc_host.bt_audio ? "OFF" : vsc_host.bt_audio_playing ? "PLAYING"
                       : vsc_host.bt_audio_connected ? "CONNECTED" : "WAITING";
        text(6, VIS_Y + 2, "BLUETOOTH", C_DIM, 1); text(80, VIS_Y + 2, bt, vsc_host.bt_audio ? C_FG : C_LINE, 1);
        text(6, VIS_Y + 14, "USB", C_DIM, 1);
        text(80, VIS_Y + 14, vsc_host.usb_connected ? "CONNECTED" : "NOT CONNECTED", vsc_host.usb_connected ? C_FG : C_LINE, 1);
        /* level meters: left over right, for each input */
        for (unsigned s = 0; s < 2; ++s) {
            int y = VIS_Y + 32 + (int)s * 16;
            text(6, y, s ? "USB" : "BT", C_DIM, 1);
            for (unsigned c = 0; c < 2; ++c) {
                float v = vsc_meter.in_peak[s][c]; if (v > 1.f) v = 1.f;
                int w = (int)(v * 190.f), yy = y + (int)c * 5 - 1;
                rect(40, yy, 190, 3, C_PANEL);
                rect(40, yy, w, 3, v > 0.95f ? C_RED : C_LAV);
            }
        }
        char b[32] = "TRACK "; b[6] = letter(track); b[7] = ' '; b[8] = 0;
        static const char *const sn[3] = {"CAPTURES", "PLAYS THE REEL", "PLAYS LIVE"};
        copy_n(b + 8, sn[vsc.t[track][P_SRC] % 3], 20);
        text(6, VIS_Y + 72, b, C_FG, 1);
        text(6, VIS_Y + 88, "REC ON PUTTY CATCHES IT", C_DIM, 1);
        break;
    } else if (row == SET_CLOCK) {
        text(6, VY(4), "CLOCK", C_DIM, 1);
        text(80, VY(4), vsc_meter.ext_clock ? "MIDI CLOCK ARRIVING" : "NO MIDI CLOCK", vsc_meter.ext_clock ? C_FG : C_LINE, 1);
        text(6, VY(20), "FOLLOWING", C_DIM, 1);
        text(80, VY(20), vsc.g[G_CLOCK] == CLOCK_INT || (vsc.g[G_CLOCK] == CLOCK_AUTO && !vsc_meter.ext_clock) ? "INTERNAL" : "MIDI", C_FG, 1);
        text(6, VY(44), "CH 1-4: A-D, 16: ALL", C_DIM, 1);
        text(6, VY(56), "PROGRAM CHANGE: SCENES", C_DIM, 1);
        break;
    } else if (row == SET_PANEL) {
        /* PANEL: the FM-1's own settings, the version and the load */
        text(6, VY(4), "VISCERA " VSC_VERSION, C_FG, 1);
        text(6, VY(18), "KNOB CC: CC 110-113", C_DIM, 1);
#ifdef __wasm__
        if (vsc_host.cpu == 255) { text(6, VY(100), "CPU: SHOWN ON THE FM-1", C_DIM, 1); break; }   /* the emulator */
#endif
        char b[16] = "CPU "; num(b + 4, vsc_host.cpu); unsigned l = (unsigned)vsc_strlen(b); b[l] = '%'; b[l + 1] = 0;
        text(6, VY(100), b, vsc_host.cpu > 85 ? C_WARN : C_FG, 1);
        rect(60, VY(102), 170, 3, C_PANEL);
        { unsigned w = vsc_host.cpu > 100 ? 170u : vsc_host.cpu * 170u / 100u;
          rect(60, VY(102), (int)w, 3, vsc_host.cpu > 85 ? C_WARN : C_FG); }
        if (vsc_host.cpu_peak) { int px0 = 60 + vsc_host.cpu_peak * 170 / 100; vline(px0 > 229 ? 229 : px0, VY(99), 9, C_INK); }
        /* where it goes: each track, SPACE, the rest (inputs, sequencer, master) */
        static const char *const part_n[VSC_PARTS + 1] = {"T1", "T2", "T3", "T4", "SP", "+"};
        for (unsigned i = 0; i <= VSC_PARTS; ++i) {
            char pb[8]; copy_n(pb, part_n[i], 3); unsigned pl = (unsigned)vsc_strlen(pb);
            pb[pl] = ' '; num(pb + pl + 1, vsc_host.part[i]);
            text(6 + (int)i * 39, VY(116), pb, vsc_host.part[i] > 30 ? C_WARN : C_DIM, 1);
        }
        text(6, VY(130), vsc_host.relief >= 2 ? "GUARD: EASED 2" : vsc_host.relief ? "GUARD: EASED 1" : "GUARD: OFF",
             vsc_host.relief ? C_WARN : C_LINE, 1);
        break;
    } else if (row == SET_LEARN) {
        int any = 0;
        unsigned first = learn_sel > 8 ? learn_sel - 8 : 0;
        for (unsigned l = first; l < first + 9 && l < VSC_LEARN; ++l) {
            const struct vsc_learn *L = &vsc_dev.learn[l];
            int y = VY(3) + (int)(l - first) * 15;
            if (l == learn_sel) rect(0, y - 3, VSC_SCREEN_W, 13, C_PANEL);
            if (L->param == 255) { text(6, y, "--", C_LINE, 1); continue; }
            any = 1;
            char b[12] = "CH"; num(b + 2, L->ch + 1); text(6, y, b, C_MID, 1);
            char c[12] = "CC"; num(c + 2, L->cc); text(44, y, c, C_FG, 1);
            char tk[4] = {'T', (char)('1' + (L->track & 3)), 0, 0};
            if (L->param < VSC_NP) text(90, y, tk, C_DIM, 1);
            char dn[10]; vsc_mod_dest_name(L->param, dn); text_n(112, y, dn, C_FG, 9);
        }
        if (!any && VY(120) >= by0) text(6, VY(128), "SAVE+KNOB, THEN SEND A CC", C_DIM, 1);
        break;
    } else {
        static const char *const states[] = {"OFF", "IDLE", "WAITING", "SCANNING", "CONNECTING", "CHECKING", "CONNECTED", "READY", "DISCONNECTING"};
        text(6, VY(4), "BLUETOOTH AUDIO IN", C_DIM, 1);
        text(150, VY(4), vsc_host.bt_audio ? (vsc_host.bt_audio_playing ? "PLAYING" : vsc_host.bt_audio_connected ? "LINKED" : "ON") : "OFF", C_FG, 1);
        text(6, VY(20), "BLUETOOTH MIDI", C_DIM, 1);
        text(150, VY(20), vsc_host.bt_midi ? states[vsc_host.ble_state % 9] : "OFF", C_FG, 1);
        text(6, VY(36), "MIDI ROLE", C_DIM, 1);
        text(150, VY(36), vsc_host.ble_role ? "CENTRAL" : "PERIPHERAL", C_FG, 1);
        text(6, VY(52), "USB", C_DIM, 1);
        text(150, VY(52), vsc_host.usb_connected ? "CONNECTED" : "--", C_FG, 1);
        for (unsigned i = 0; i < vsc_host.ble_count && i < 4; ++i)
            text_n(12, VY(74) + (int)i * 14, vsc_host.ble_name[i], i == set_dev ? C_INK : C_DIM, 37);
        if (!vsc_host.ble_count && vsc_host.ble_role) text(12, VY(74), "REC: SCAN FOR MIDI DEVICES", C_DIM, 1);
        break;
    }
    default: break;
    }
}

static float fexp2(float x)
{
    int i = (int)x; if ((float)i > x) --i;
    float f = x - (float)i, p = 1.f + f * (0.6951786f + f * (0.2261598f + f * 0.0782371f));
    union { float f; uint32_t u; } v; v.f = p; v.u += (uint32_t)i << 23; return v.f;
}
/* the selected modulator's shape, for its picture */
static void compute_mod_curve(void)
{
    unsigned m = VMOD(row), ty = vsc.t[track][P_MOD(m, 0)] & 3;
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
            cset(x, v < 0.f ? 0.f : v > 1.f ? 1.f : v);
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
            cset(x, v * (b[8] / 127.f));
        }
        int amt = b[8] * 2;
        adsr_node[0][0] = 0; adsr_node[0][1] = 0;
        adsr_node[1][0] = xa; adsr_node[1][1] = amt;
        adsr_node[2][0] = xd; adsr_node[2][1] = (int)(S * (float)amt);
        adsr_node[3][0] = xh; adsr_node[3][1] = (int)(S * (float)amt);
    } else {
        /* RANDOM and FOLLOW: what they have done lately */
        for (int x = 0; x < VSC_SCREEN_W; ++x) cset(x, (float)mod_hist[m][(hist_w + (unsigned)(x * 96 / VSC_SCREEN_W)) % 96] * (1.f / 255.f));
    }
}
void vsc_ui_frame(uint32_t now)
{
    now_ms = now;
    if (page == PG_DISSECT) {
        float lev = vsc_meter.ana_low + vsc_meter.ana_mid + vsc_meter.ana_high;
        dis_lev[hist_w] = (uint8_t)(lev > 1.f ? 255.f : lev * 255.f);
        uint32_t h = vsc_meter.ana_hits;
        dis_hit[hist_w] = h != dis_hits_seen ? (uint8_t)(64.f + 191.f * (vsc_meter.ana_hit > 1.f ? 1.f : vsc_meter.ana_hit)) : 0;
        dis_hits_seen = h;
    }
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        mod_hist[m][hist_w] = (uint8_t)(vsc_meter.mod_value[track][m] * 255.f);
    }
    hist_w = (hist_w + 1) % 96;
    if (page == PG_SHARDS) shards_frame();
    if (page == PG_RESON) reson_frame();
    if (page == PG_SPECTRA) spectra_frame();
    if (page == PG_MODS && row != MATRIX_VIEW) compute_mod_curve();
}
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels)
{
    band = pixels; by0 = (int)y0;
    for (unsigned i = 0; i < VSC_SCREEN_W * VSC_BAND; ++i) pixels[i] = C_BG;
    draw_header();
    draw_vis();
    draw_knobs();
    draw_footer();
}

/* ------------------------------------------------------------- panel LEDs */
void vsc_ui_leds(uint8_t out[41])
{
    uint8_t glow = vsc_dev.backlight ? 1 : 0;
    for (unsigned i = 0; i < 41; ++i) out[i] = glow;
    /* the open page's button */
    static const uint8_t page_btn[PG_COUNT] = {BTN_HOME, BTN_FX, BTN_SCL, BTN_EDIT, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_GLO,
                                               BTN_ARP, BTN_SEQ, BTN_SAVE, BTN_GLO, BTN_SAVE, BTN_GLO, BTN_ENV};
    out[page_btn[page]] = 2;
    if (vsc_meter.playing) out[BTN_PLAY] = 2;
    if (recording()) out[BTN_REC] = (now_ms / 250u) & 1u ? 2 : 0;
    if (octave < 2) out[BTN_OCTD] = 2;
    if (octave > 2) out[BTN_OCTU] = 2;
    if (temp_on) out[BTN_OCTD] = out[BTN_OCTU] = 2;
    for (unsigned b = 0; b < 14; ++b) if (prev_buttons & (1u << b)) out[b] = 2;
    /* keys: what's held, the steps of the pattern on SEQ, scenes with ARP, tracks with HOME */
    if (page == PG_SEQ && vsc.t[track][P_SEQM] == SEQ_STEPS) {
        unsigned len = vsc.length[track] ? vsc.length[track] : 16;
        for (unsigned k = 0; k < 16; ++k) {
            unsigned st = step_page * 16 + k;
            out[14 + k] = st >= len ? 0 : vsc.step[track][st].on ? 2 : glow;
            if (vsc_meter.playing && vsc_meter.step[track] == st) out[14 + k] = vsc.step[track][st].on ? 0 : 2;
        }
    }
    if (page == PG_RESON) for (unsigned k = 0; k < VSC_RBANDS; ++k) out[14 + k] = vsc.g[G_BG0 + k] > 60 || k == rb_sel ? 2 : glow;
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
