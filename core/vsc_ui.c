/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: screen and controls. Portable: the platform hands in a control
 * snapshot and asks for 16-row bands of RGB565 pixels. Control context only. */
#include "vsc.h"

struct vsc_host_status vsc_host;

/* ---------------------------------------------------------------- colour */
#define RGB(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))
static const uint16_t C_BG = RGB(18, 17, 16), C_PANEL = RGB(30, 29, 27), C_LINE = RGB(52, 50, 46),
    C_DIM = RGB(111, 107, 100), C_MID = RGB(169, 163, 151), C_FG = RGB(230, 223, 209),
    C_INK = RGB(243, 239, 231), C_WARN = RGB(196, 120, 96);

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

/* ------------------------------------------------------------- pages */
enum { PG_MIX, PG_MAT, PG_GRAIN, PG_FILTER, PG_COLOR, PG_MODS, PG_SPACE, PG_SCENE, PG_STEPS, PG_PROJECT, PG_SETTINGS, PG_COUNT };
static const char *const page_name[PG_COUNT] = {"MIX", "MATERIAL", "GRAIN", "FILTER", "COLOR", "MODS", "SPACE",
                                                "SCENE", "STEPS", "PROJECT", "SETTINGS"};
enum { S_NONE, S_T, S_G, S_TEMPO, S_STEP };
struct slot { uint8_t kind, id; };
#define T(p) {S_T, p}
#define G(p) {S_G, p}
#define N {S_NONE, 0}
static const struct slot rows_mat[3][6] = {
    {T(P_MODE), T(P_SLOT), T(P_START), T(P_LEN), T(P_SPEED), T(P_PITCH)},
    {T(P_LOOPM), T(P_INPUT), T(P_DUB), T(P_SIZE), T(P_QUAL), T(P_GAIN)},
    {T(P_ATK), T(P_DEC), T(P_SUS), T(P_REL), T(P_GLIDE), T(P_VELO)}};
static const struct slot rows_grain[2][6] = {
    {T(P_GMIX), T(P_GSIZE), T(P_GDENS), T(P_GPITCH), T(P_GSPRAY), T(P_GSHAPE)},
    {T(P_GTIME), T(P_GREV), T(P_GFREEZE), T(P_GDETUNE), T(P_GQUANT), T(P_GFDBK)}};
static const struct slot rows_filter[1][6] = {{T(P_FTYPE), T(P_FCUT), T(P_FRES), T(P_FSPREAD), T(P_FSCALE), T(P_FMIX)}};
static const struct slot rows_color[2][6] = {
    {T(P_DRIVE), T(P_COMP), T(P_BITS), T(P_RATE), T(P_NOISE), T(P_TONE)},
    {T(P_NTYPE), T(P_CMIX), N, N, N, N}};
static const struct slot rows_mix[1][6] = {{T(P_LEVEL), T(P_PAN), T(P_SEND), T(P_MUTE), G(G_MAIN), G(G_INMON)}};
static const struct slot rows_space[2][6] = {
    {G(G_DTIME), G(G_DFEED), G(G_DTONE), G(G_DLONG), G(G_DLEVEL), G(G_SWING)},
    {G(G_RSIZE), G(G_RDAMP), G(G_RLEVEL), G(G_CLOCK), {S_TEMPO, 0}, N}};
static const struct slot rows_scene[2][6] = {
    {G(G_M0), G(G_M1), G(G_M2), G(G_M3), G(G_TARGET), G(G_MORPH)},
    {G(G_MSEL), N, N, N, N, N}};
static const struct slot rows_steps[2][6] = {
    {{S_STEP, 0}, {S_STEP, 1}, {S_STEP, 2}, {S_STEP, 3}, {S_STEP, 4}, {S_STEP, 5}},
    {T(P_SEQM), N, N, N, N, N}};
static const struct slot rows_carom[2][6] = {
    {T(P_SEQM), T(P_RSPEED), T(P_RGRAV), T(P_RLOSS), T(P_RSPREAD), T(P_RQUANT)},
    {T(P_RCOUNT), T(P_RWALLS), T(P_RJITTER), T(P_RGATE), N, N}};
static const char *const mod_name[VSC_MODS] = {"LFO", "RANDOM", "FOLLOW", "ENV"};

static unsigned track, page = PG_MIX, row, mod_sel, step_sel, step_page, octave;
static uint32_t prev_buttons, prev_keys, rec_down_at;
static uint8_t temp_on, rec_long, live_rec;
static uint8_t temp_t[VSC_TRACKS][VSC_NP], temp_g[VSC_NG];
static const char *notice; static uint32_t notice_until, now_ms;
static unsigned proj_slot, set_dev;
static float mod_hist[VSC_MODS][96]; static unsigned hist_w;

void vsc_ui_notice(const char *t, uint32_t ms) { notice = t; notice_until = now_ms + ms; }

static unsigned row_count(void)
{
    switch (page) {
    case PG_MAT: return 3; case PG_GRAIN: case PG_COLOR: case PG_SPACE: case PG_SCENE: case PG_STEPS: return 2;
    default: return 1;
    }
}
static struct slot slot_at(unsigned i)
{
    struct slot none = {S_NONE, 0};
    unsigned r = row % row_count();
    switch (page) {
    case PG_MAT: return rows_mat[r][i];
    case PG_GRAIN: return rows_grain[r][i];
    case PG_FILTER: return rows_filter[0][i];
    case PG_COLOR: return rows_color[r][i];
    case PG_MIX: return rows_mix[0][i];
    case PG_SPACE: return rows_space[r][i];
    case PG_SCENE: return rows_scene[r][i];
    case PG_STEPS: return vsc.t[track][P_SEQM] == SEQ_CAROM ? rows_carom[r][i] : rows_steps[r][i];
    case PG_MODS: if (i < 4) { struct slot s = {S_T, (uint8_t)(P_M0A + mod_sel * 4 + i)}; return s; } return none;
    default: return none;
    }
}
static const char *const step_names[6] = {"NOTE", "VELO", "LOCK", "VALUE", "LENGTH", "TRIG"};
static void slot_text(struct slot s, char name[8], char val[8], float *bar)
{
    *bar = -1.f; name[0] = val[0] = 0;
    if (s.kind == S_T) {
        const struct vsc_pinfo *in = vsc_tparam_info(track, s.id);
        unsigned v = vsc.t[track][s.id];
        vsc_memcpy(name, in->name, 7); name[7] = 0;
        vsc_param_label(in, s.id, v, val, 0);
        if (!(in->flags & VSC_PF_ENUM)) *bar = (float)v / (float)in->max;
    } else if (s.kind == S_G) {
        const struct vsc_pinfo *in = vsc_gparam_info(s.id);
        unsigned v = vsc.g[s.id];
        vsc_memcpy(name, in->name, 7); name[7] = 0;
        vsc_param_label(in, s.id, v, val, 1);
        if (!(in->flags & VSC_PF_ENUM)) *bar = (float)v / (float)in->max;
    } else if (s.kind == S_TEMPO) {
        vsc_memcpy(name, "TEMPO", 6); num(val, vsc.tempo);
    } else if (s.kind == S_STEP) {
        const struct vsc_step *st = &vsc.step[track][step_sel];
        vsc_memcpy(name, step_names[s.id], vsc_strlen(step_names[s.id]) + 1);
        switch (s.id) {
        case 0: num(val, st->note); break;
        case 1: num(val, st->vel); break;
        case 2: if (st->lp[0] < VSC_NP) { vsc_memcpy(val, vsc_tparam_info(track, st->lp[0])->name, 7); val[7] = 0; }
                else vsc_memcpy(val, "--", 3); break;
        case 3: if (st->lp[0] < VSC_NP) num(val, st->lv[0]); else vsc_memcpy(val, "--", 3); break;
        case 4: num(val, vsc.length[track] ? vsc.length[track] : 16); break;
        default: vsc_memcpy(val, st->on ? "ON" : "OFF", 4); break;
        }
    }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void edit_slot(struct slot s, int d)
{
    if (s.kind == S_T) {
        const struct vsc_pinfo *in = vsc_tparam_info(track, s.id);
        if (!in->name[0]) return;
        if (!(in->flags & VSC_PF_ENUM) && (d > 2 || d < -2)) d *= 2;
        vsc.t[track][s.id] = (uint8_t)clampi((int)vsc.t[track][s.id] + d, 0, in->max);
    } else if (s.kind == S_G) {
        const struct vsc_pinfo *in = vsc_gparam_info(s.id);
        vsc.g[s.id] = (uint8_t)clampi((int)vsc.g[s.id] + d, 0, in->max);
    } else if (s.kind == S_TEMPO) {
        vsc.tempo = (uint16_t)clampi((int)vsc.tempo + d, 30, 300);
    } else if (s.kind == S_STEP) {
        struct vsc_step *st = &vsc.step[track][step_sel];
        switch (s.id) {
        case 0: st->note = (uint8_t)clampi(st->note + d, 0, 127); break;
        case 1: st->vel = (uint8_t)clampi(st->vel + d, 1, 127); break;
        case 2: { int p = st->lp[0] >= VSC_NP ? -1 : st->lp[0]; p = clampi(p + d, -1, VSC_NP - 1);
                  while (p >= 0 && !vsc_tparam_info(track, (unsigned)p)->name[0]) p += d > 0 ? 1 : -1;
                  st->lp[0] = p < 0 || p >= VSC_NP ? 255 : (uint8_t)p;
                  if (st->lp[0] < VSC_NP) st->lv[0] = vsc.t[track][st->lp[0]]; break; }
        case 3: if (st->lp[0] < VSC_NP) st->lv[0] = (uint8_t)clampi(st->lv[0] + d, 0, vsc_tparam_info(track, st->lp[0])->max); break;
        case 4: { int l = vsc.length[track] ? vsc.length[track] : 16; vsc.length[track] = (uint8_t)clampi(l + d, 1, VSC_STEPS); break; }
        default: st->on = d > 0; break;
        }
    }
}

/* ------------------------------------------------------------- input */
void vsc_ui_init(void) { notice = "VISCERA " VSC_VERSION; notice_until = 1500; }

#define B(n) (1u << (n))
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg)
{
    unsigned req = HOST_NONE;
    now_ms = in->now_ms;
    uint32_t down = in->buttons & ~prev_buttons, up = ~in->buttons & prev_buttons;
    uint32_t kdown = in->keys & ~prev_keys, kup = ~in->keys & prev_keys;
    int lfo_held = (in->buttons & B(BTN_LFO)) != 0, arp_held = (in->buttons & B(BTN_ARP)) != 0;
    int home_held = (in->buttons & B(BTN_HOME)) != 0, save_held = (in->buttons & B(BTN_SAVE)) != 0;

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

    /* page buttons: press again to step through a page's sub-views */
    struct { unsigned btn, pg; } map[] = {{BTN_HOME, PG_MIX}, {BTN_EDIT, PG_MAT}, {BTN_FX, PG_GRAIN}, {BTN_SCL, PG_FILTER},
        {BTN_ENV, PG_COLOR}, {BTN_GLO, PG_SPACE}, {BTN_SEQ, PG_STEPS}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (down & B(map[i].btn)) { if (page != map[i].pg) row = 0; page = map[i].pg; }
    if (up & B(BTN_LFO) && !(prev_buttons & B(31))) { if (page == PG_MODS) mod_sel = (mod_sel + 1) % VSC_MODS; page = PG_MODS; }
    if (up & B(BTN_ARP) && !(prev_buttons & B(31))) { page = PG_SCENE; }
    if (up & B(BTN_SAVE) && !(prev_buttons & B(31))) { page = page == PG_PROJECT ? PG_SETTINGS : PG_PROJECT; }
    /* bit 31 of prev_buttons marks "a held button was used as a modifier" */
    uint32_t used = prev_buttons & B(31);
    if (!(in->buttons & (B(BTN_LFO) | B(BTN_ARP) | B(BTN_SAVE)))) used = 0;

    /* transport */
    if (down & B(BTN_PLAY)) vsc_event(EV_PLAY, track, 0, 0);
    if (down & B(BTN_REC)) { rec_down_at = now_ms; rec_long = 0; }
    if ((in->buttons & B(BTN_REC)) && !rec_long && now_ms - rec_down_at > 900) {
        if (page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_CAROM) {
            rec_long = 1; vsc_event(EV_BALL_CLEAR, track, 0, 0); vsc_ui_notice("BALLS CLEARED", 900);
        } else if (vsc.t[track][P_MODE] == MODE_LOOP) {
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
        else if (page == PG_SETTINGS) { if (vsc_host.ble_count) { *arg = set_dev; req = HOST_BLE_CONNECT; } else req = HOST_BLE_SCAN; }
        else if (vsc.t[track][P_MODE] == MODE_LOOP) vsc_event(EV_REC, track, 0, 0);
        else vsc_ui_notice("REC: LOOP TRACKS ONLY", 900);
    }
    if (page == PG_PROJECT && save_held && (down & B(BTN_PLAY))) { *arg = proj_slot; req = HOST_SAVE; used = B(31); }

    /* encoders */
    for (unsigned e = 0; e < 7; ++e) {
        int d = in->enc[e];
        if (!d) continue;
        if (e == 6) {
            if (page == PG_MODS) mod_sel = (unsigned)clampi((int)mod_sel + d, 0, VSC_MODS - 1);
            else if (page == PG_MIX || page == PG_PROJECT || page == PG_SETTINGS) track = (unsigned)clampi((int)track + d, 0, VSC_TRACKS - 1);
            else row = (unsigned)clampi((int)row + d, 0, (int)row_count() - 1);
            continue;
        }
        if (page == PG_PROJECT) { if (e == 0) proj_slot = (unsigned)clampi((int)proj_slot + d, 0, VSC_PROJECTS - 1); continue; }
        if (page == PG_SETTINGS) {
            if (e == 0) { *arg = !vsc_host.bt_audio; req = HOST_BT_AUDIO; }
            else if (e == 1) { *arg = !vsc_host.bt_midi; req = HOST_BT_MIDI; }
            else if (e == 2) { *arg = !vsc_host.ble_role; req = HOST_BLE_ROLE; }
            else if (e == 3) set_dev = (unsigned)clampi((int)set_dev + d, 0, vsc_host.ble_count ? vsc_host.ble_count - 1 : 0);
            else if (e == 4) req = HOST_BLE_FORGET;
            continue;
        }
        struct slot s = slot_at(e);
        if (lfo_held && s.kind == S_T && !(vsc_tparam_info(track, s.id)->flags & VSC_PF_ENUM)) {
            vsc_route_set(track, mod_sel, s.id, vsc_route_get(track, mod_sel, s.id) + d * 3);
            used = B(31);
        } else if (arp_held && s.kind == S_T && !(vsc_tparam_info(track, s.id)->flags & VSC_PF_ENUM)) {
            unsigned m = vsc.g[G_MSEL];
            vsc_macro_set(m, track, s.id, vsc_macro_get(m, track, s.id) + d * 3);
            used = B(31);
        } else edit_slot(s, d);
    }

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
    prev_buttons = (in->buttons & 0x7fffffffu) | used;
    prev_keys = in->keys;
    return req;
}

/* ------------------------------------------------------------- drawing */
#define VIS_Y 20
#define VIS_H 140
#define KNOB_Y 176
static void frame_box(void) { hline(0, VIS_Y - 2, VSC_SCREEN_W, C_LINE); hline(0, KNOB_Y - 4, VSC_SCREEN_W, C_LINE); }

static void draw_header(void)
{
    char b[24];
    rect(0, 0, VSC_SCREEN_W, 16, C_BG);
    b[0] = 'T'; b[1] = (char)('1' + track); b[2] = 0;
    rect(4, 3, 15, 11, C_FG); text(6, 5, b, C_BG, 1);
    const char *title = page == PG_MODS ? mod_name[mod_sel]
        : page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_CAROM ? "CAROM" : page_name[page];
    text(24, 5, title, C_FG, 1);
    if (row_count() > 1) {
        for (unsigned r = 0; r < row_count(); ++r) rect(24 + text_w(title, 1) + 6 + (int)r * 5, 8, 3, 3, r == row ? C_FG : C_LINE);
    }
    num(b, vsc.tempo);
    int x = VSC_SCREEN_W - 4 - text_w(b, 1);
    text(x, 5, b, C_MID, 1);
    if (vsc_meter.playing) { for (int i = 0; i < 5; ++i) vline(x - 10 + i, 6 + i / 2, 7 - (i / 2) * 2, C_FG); }
    else rect(x - 10, 6, 6, 6, C_DIM);
    int ix = x - 34;
    text(ix, 5, "BT", vsc_meter.bt_in ? C_FG : C_LINE, 1);
    text(ix - 22, 5, "USB", vsc_meter.usb_in ? C_FG : C_LINE, 1);
    if (temp_on) text(ix - 52, 5, "TEMP", C_WARN, 1);
}
static void draw_knobs(void)
{
    char name[8], val[8];
    for (unsigned i = 0; i < 6; ++i) {
        int x = (int)i * 40;
        struct slot s = slot_at(i);
        float bar;
        slot_text(s, name, val, &bar);
        if (!name[0]) continue;
        int mod = 0;
        if (s.kind == S_T) for (unsigned m = 0; m < VSC_MODS; ++m) mod |= vsc_route_get(track, m, s.id) != 0;
        int locked = s.kind == S_T && vsc_lock[track][s.id] != 255;
        /* a knob's column is 40 px: six characters, never into the next column */
        text_n(x + 3, KNOB_Y, name, mod ? C_FG : C_DIM, 6);
        text_n(x + 3, KNOB_Y + 14, val, locked ? C_WARN : C_INK, 6);
        if (bar >= 0.f) {
            rect(x + 3, KNOB_Y + 30, 34, 3, C_LINE);
            rect(x + 3, KNOB_Y + 30, (int)(34.f * bar + 0.5f), 3, C_FG);
        }
        if (i) vline(x, KNOB_Y - 2, 40, C_LINE);   /* each knob keeps to its own column */
    }
    /* key and button hints along the bottom */
    const char *hint = page == PG_STEPS && vsc.t[track][P_SEQM] == SEQ_CAROM ? "REC: ADD BALLS  HOLD REC: CLEAR"
        : page == PG_STEPS ? "KEYS: STEPS  OCT: PAGE  REC: RECORD"
        : page == PG_PROJECT ? "E1: SLOT  REC: LOAD/SAVE  SAVE+PLAY: SAVE"
        : page == PG_SETTINGS ? "E1-3 AUDIO/MIDI/ROLE E4 DEV E5 UNPAIR"
        : page == PG_MODS ? "HOLD LFO + TURN: AMOUNT  E7: MODULATOR"
        : page == PG_SCENE ? "ARP+KEY: RECALL  ARP+SAVE+KEY: STORE"
        : "E7: ROW   HOLD HOME + TOP KEYS: TRACK";
    if (notice && (int32_t)(notice_until - now_ms) > 0) hint = notice;
    text_n(4, 228, hint, notice && (int32_t)(notice_until - now_ms) > 0 ? C_INK : C_DIM, 39);
}

/* visualisations, computed once per frame and painted band by band */
static uint8_t wave[VSC_SCREEN_W];      /* 0..60 amplitude per column */
static float curve[VSC_SCREEN_W];       /* 0..1 per column */
static void draw_wave_from_loop(int y, int h)
{
    for (int x = 0; x < VSC_SCREEN_W; ++x) {
        int a = wave[x] * h / 120;
        if (a) vline(x, y + h / 2 - a, a * 2, (x & 1) ? C_MID : C_FG);
        else px(x, y + h / 2, C_LINE);
    }
}
static void draw_vis(void)
{
    unsigned t = track;
    switch (page) {
    case PG_MAT: case PG_GRAIN: {
        draw_wave_from_loop(VIS_Y + 6, 90);
        uint32_t len = vsc_meter.pos_len[t];
        if (page == PG_MAT && len) {
            float s = (float)vsc.t[t][P_START] / 127.f, l = (float)vsc.t[t][P_LEN] / 127.f;
            int a = (int)(s * 239.f), b = a + (int)(l * (239.f - (float)a));
            hline(a, VIS_Y + 104, b - a + 1, C_FG); vline(a, VIS_Y + 100, 9, C_FG); vline(b, VIS_Y + 100, 9, C_FG);
            int ph = (int)((float)vsc_meter.pos_play[t] / (float)len * 239.f);
            vline(ph, VIS_Y + 2, 98, C_INK);
        }
        if (page == PG_GRAIN) {
            int w = (int)((float)vsc_meter.grain_write[t] / (float)VSC_GRAIN_FRAMES * 239.f);
            vline(w, VIS_Y + 2, 98, C_WARN);
            for (unsigned g = 0; g < VSC_GRAINS; ++g) {
                if (vsc_meter.grain_pos[t][g] == 0xffff) continue;
                int x = (int)vsc_meter.grain_pos[t][g] * 239 / 65535;
                int a = vsc_meter.grain_amp[t][g];
                int hgt = 6 + (a < 128 ? a : 255 - a) / 6;
                rect(x - 1, VIS_Y + 100 + 18 - hgt, 3, hgt, C_INK);
            }
        }
        const char *st = vsc.t[t][P_MODE] == MODE_LOOP
            ? (const char *const[]){"EMPTY: PRESS REC", "RECORDING", "PLAYING", "OVERDUB"}[vsc_meter.loop_state[t] & 3]
            : vsc.t[t][P_MODE] == MODE_THRU ? "LIVE INPUT" : vsc.t[t][P_SLOT] < VSC_BANK_SLOTS
            ? (vsc_bank_frames(vsc.t[t][P_SLOT]) ? vsc_bank_name(vsc.t[t][P_SLOT]) : "EMPTY SLOT") : "LOOP";
        text_n(4, VIS_Y + 124, st, C_MID, 39);
        break;
    }
    case PG_FILTER: case PG_COLOR: case PG_SPACE: {
        int y0 = VIS_Y + 4, h = 110;
        for (int x = 0; x < VSC_SCREEN_W; ++x) {
            int y = y0 + h - (int)(curve[x] * (float)h);
            vline(x, y, 2, C_FG);
            if ((x & 3) == 0) vline(x, y + 2, y0 + h - y - 2 > 0 ? y0 + h - y - 2 : 0, C_PANEL);
        }
        if (page != PG_FILTER) {
            for (int x = 0; x < 128; ++x) {
                int v = (int)(vsc_scope[0][x] * 20.f);
                px(56 + x, VIS_Y + 128 - v, C_MID);
            }
        }
        break;
    }
    case PG_MODS:
        for (unsigned m = 0; m < VSC_MODS; ++m) {
            int y0 = VIS_Y + (int)m * 35, mid = y0 + 16;
            text(4, y0 + 2, mod_name[m], m == mod_sel ? C_INK : C_DIM, 1);
            hline(56, mid, 180, C_PANEL);
            for (int i = 0; i < 96; ++i) {
                float v = mod_hist[m][(hist_w + (unsigned)i) % 96];
                int y = mid - (int)(v * 14.f);
                rect(56 + i * 2 - 1 + 1, y, 2, 2, m == mod_sel ? C_FG : C_DIM);
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
            int y = VIS_Y + 80 + (int)m * 14;
            char lab[4] = {'M', (char)('1' + m), 0, 0};
            text(6, y, lab, m == vsc.g[G_MSEL] ? C_INK : C_DIM, 1);
            rect(26, y + 2, 200, 3, C_PANEL);
            rect(26, y + 2, vsc.g[G_M0 + m] * 200 / 127, 3, C_FG);
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
    case PG_SETTINGS: {
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
    /* PLAY / KEYS from the bank, THRU: a coarse picture from the main scope */
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
    if (page == PG_FILTER) {
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
        float drive = (float)vsc.t[t][P_DRIVE] / 127.f, bits = (float)vsc.t[t][P_BITS] / 127.f;
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
void vsc_ui_frame(uint32_t now)
{
    now_ms = now;
    for (unsigned m = 0; m < VSC_MODS; ++m) {
        float v = vsc_meter.mod_value[track][m];
        mod_hist[m][hist_w] = m >= 2 ? v * 2.f - 1.f : v;
    }
    hist_w = (hist_w + 1) % 96;
    compute_wave();
    compute_curve();
}
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels)
{
    band = pixels; by0 = (int)y0;
    for (unsigned i = 0; i < VSC_SCREEN_W * VSC_BAND; ++i) pixels[i] = C_BG;
    draw_header();
    frame_box();
    draw_vis();
    draw_knobs();
}
