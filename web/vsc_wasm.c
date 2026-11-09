/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera in the browser: the platform layer for the same core the FM-1 runs.
 * Built with clang --target=wasm32, no C library. The page's AudioWorklet calls
 * render once per 128 frames; the page asks for the screen and sends controls. */
#include "../core/vsc.h"

#define W_EXPORT(name) __attribute__((export_name(#name)))

/* ---- the small part of the C library the compiler may call ---- */
void *memset(void *d, int v, unsigned long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, unsigned long n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

/* ---- sample bank: 16 slots of 32 kHz mono PCM, like the FM-1's ---- */
#define BANK_FRAMES (32000 * 40)     /* 40 s shared, more than the FM-1's 17 s */
static int16_t bank_pcm[BANK_FRAMES];
static uint32_t bank_off[VSC_BANK_SLOTS], bank_len[VSC_BANK_SLOTS];
static char bank_names[VSC_BANK_SLOTS][16];
static uint32_t bank_used;
unsigned vsc_bank_frames(unsigned s) { return s < VSC_BANK_SLOTS ? bank_len[s] : 0; }
const char *vsc_bank_name(unsigned s) { return s < VSC_BANK_SLOTS ? bank_names[s] : ""; }
const int16_t *vsc_bank_chunk(unsigned s, unsigned c)
{
    if (s >= VSC_BANK_SLOTS || (uint32_t)c * VSC_CHUNK >= bank_len[s]) return 0;
    /* the last chunk may be short: the core only reads frames it was told exist */
    return bank_pcm + bank_off[s] + (uint32_t)c * VSC_CHUNK;
}
/* The page writes PCM here, then calls bank_commit. */
static int16_t bank_stage[32000 * 12];
static char name_stage[16];
W_EXPORT(bank_stage) int16_t *w_bank_stage(void) { return bank_stage; }
W_EXPORT(bank_stage_frames) unsigned w_bank_stage_frames(void) { return sizeof bank_stage / sizeof bank_stage[0]; }
W_EXPORT(bank_name) char *w_bank_name(void) { return name_stage; }
/* Puts frames of the staged PCM into a slot. Slots are packed in the order
 * they arrive; replacing a slot re-packs the bank. Returns 0 or -1 if full. */
W_EXPORT(bank_commit) int w_bank_commit(unsigned slot, unsigned frames)
{
    if (slot >= VSC_BANK_SLOTS || frames > sizeof bank_stage / sizeof bank_stage[0]) return -1;
    vsc_event(EV_STOP, 0, 0, 0);
    /* drop the slot's old audio by re-packing the others */
    if (bank_len[slot]) {
        uint32_t off = bank_off[slot], len = bank_len[slot];
        memmove(bank_pcm + off, bank_pcm + off + len, (bank_used - off - len) * sizeof(int16_t));
        for (unsigned i = 0; i < VSC_BANK_SLOTS; ++i) if (bank_len[i] && bank_off[i] > off) bank_off[i] -= len;
        bank_used -= len; bank_len[slot] = 0;
    }
    if (bank_used + frames > BANK_FRAMES) return -1;
    memcpy(bank_pcm + bank_used, bank_stage, frames * sizeof(int16_t));
    bank_off[slot] = bank_used; bank_len[slot] = frames; bank_used += frames;
    for (unsigned i = 0; i < 16; ++i) bank_names[slot][i] = name_stage[i];
    bank_names[slot][15] = 0;
    return 0;
}

/* ---- audio ---- */
static float in_bt[VSC_BLOCK * 2], out_buf[VSC_BLOCK * 2];
static uint8_t have_input;
W_EXPORT(input_buffer) float *w_input(void) { return in_bt; }
W_EXPORT(output_buffer) float *w_output(void) { return out_buf; }
W_EXPORT(init) void w_init(void) { vsc_init(); vsc_project_defaults(&vsc); vsc_ui_init(); vsc_host.cpu = 255; }
/* has_input: the page's live input (standing in for Bluetooth/USB audio) */
W_EXPORT(render) void w_render(int has_input)
{
    struct vsc_inputs in = {has_input ? in_bt : 0, 0};
    have_input = (uint8_t)has_input;
    /* the page's file or microphone stands in for Bluetooth audio */
    vsc_host.bt_audio = 1; vsc_host.bt_audio_connected = vsc_host.bt_audio_playing = (uint8_t)has_input;
    vsc_render(&in, out_buf);
    if (vsc.g[G_INMON] && has_input)
        for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) out_buf[i] += in_bt[i];
}

/* ---- screen and controls ---- */
static uint16_t screen[VSC_SCREEN_W * VSC_SCREEN_H];
W_EXPORT(screen) uint16_t *w_screen(void) { return screen; }
W_EXPORT(paint) void w_paint(unsigned now_ms)
{
    vsc_ui_frame(now_ms);
    for (unsigned y = 0; y < VSC_SCREEN_H; y += VSC_BAND) vsc_ui_paint_band(y, screen + y * VSC_SCREEN_W);
}
static struct vsc_ui_input ui;
static unsigned host_req, host_arg;
W_EXPORT(control) unsigned w_control(unsigned keys, unsigned buttons, int e0, int e1, int e2, int e3, int e4, int e5, int e6, unsigned now)
{
    ui.keys = keys; ui.buttons = buttons; ui.now_ms = now;
    ui.enc[0] = (int16_t)e0; ui.enc[1] = (int16_t)e1; ui.enc[2] = (int16_t)e2; ui.enc[3] = (int16_t)e3;
    ui.enc[4] = (int16_t)e4; ui.enc[5] = (int16_t)e5; ui.enc[6] = (int16_t)e6;
    host_req = vsc_ui_update(&ui, &host_arg);
    return host_req;
}
W_EXPORT(host_arg) unsigned w_host_arg(void) { return host_arg; }
/* status the UI shows on SETTINGS and the header */
W_EXPORT(set_host) void w_set_host(unsigned bt_audio, unsigned playing, unsigned usb)
{
    vsc_host.bt_audio = (uint8_t)bt_audio; vsc_host.bt_audio_connected = (uint8_t)bt_audio;
    vsc_host.bt_audio_playing = (uint8_t)playing; vsc_host.usb_connected = (uint8_t)usb;
    vsc_host.storage_ok = 1;
}
/* projects live in the page's browser storage: the page copies them in and out */
W_EXPORT(project) struct vsc_project *w_project(void) { return &vsc; }
W_EXPORT(project_bytes) unsigned w_project_bytes(void) { return sizeof vsc; }
W_EXPORT(project_seal) void w_project_seal(unsigned slot)
{
    vsc.magic = VSC_MAGIC; vsc.version = VSC_FORMAT; vsc.bytes = sizeof vsc;
    char name[12] = "PROJECT  ";
    name[8] = (char)('1' + slot); name[9] = 0;
    for (unsigned i = 0; i < 12; ++i) vsc.name[i] = name[i];
    vsc.crc = vsc_crc32(&vsc.name, sizeof vsc - offsetof(struct vsc_project, name));
}
W_EXPORT(project_check) int w_project_check(void) { return vsc_project_valid(&vsc); }
W_EXPORT(stop_all) void w_stop_all(void) { vsc_event(EV_STOP, 0, 0, 0); vsc_event(EV_PANIC, 0, 0, 0); }
W_EXPORT(slot_info) void w_slot_info(unsigned slot, unsigned used, const char *name)
{
    if (slot >= VSC_PROJECTS) return;
    vsc_host.slot_used[slot] = (uint8_t)used;
    for (unsigned i = 0; i < 11; ++i) vsc_host.slot_name[slot][i] = used && name ? name[i] : 0;
    vsc_host.slot_name[slot][11] = 0;
}
W_EXPORT(name_buffer) char *w_name_buffer(void) { return name_stage; }
static uint32_t midi_ms;
W_EXPORT(midi) void w_midi(unsigned status, unsigned a, unsigned b)
{
    uint8_t m[3] = {(uint8_t)status, (uint8_t)a, (uint8_t)b};
    unsigned n = status >= 0xf8 ? 1u : (status & 0xe0u) == 0xc0u ? 2u : 3u;
    midi_ms += 1;   /* the page sends clock in order; one port */
    vsc_midi_in(0, m, n, midi_ms);
}

/* ---- direct access, for the page's tests and tools ---- */
W_EXPORT(set_param) void w_set_param(unsigned t, unsigned p, unsigned v) { if (t < VSC_TRACKS && p < VSC_NP) vsc.t[t][p] = (uint8_t)v; }
W_EXPORT(get_param) unsigned w_get_param(unsigned t, unsigned p) { return t < VSC_TRACKS && p < VSC_NP ? vsc.t[t][p] : 0; }
W_EXPORT(set_global) void w_set_global(unsigned g, unsigned v) { if (g < VSC_NG) vsc.g[g] = (uint8_t)v; }
W_EXPORT(get_global) unsigned w_get_global(unsigned g) { return g < VSC_NG ? vsc.g[g] : 0; }
W_EXPORT(set_route) void w_set_route(unsigned t, unsigned r, unsigned src, unsigned param, int depth, unsigned flags)
{
    if (t >= VSC_TRACKS || r >= VSC_MOD_ROUTES) return;
    vsc.route[t][r].src = (uint8_t)src; vsc.route[t][r].param = (uint8_t)param;
    vsc.route[t][r].depth = (int8_t)depth; vsc.route[t][r].flags = (uint8_t)flags;
}
W_EXPORT(mod_value) float w_mod_value(unsigned t, unsigned m) { return t < VSC_TRACKS && m < VSC_MODS ? vsc_meter.mod_value[t][m] : 0.f; }
W_EXPORT(track_peak) float w_track_peak(unsigned t) { return t < VSC_TRACKS ? vsc_meter.peak[t] : 0.f; }
W_EXPORT(tempo_now) float w_tempo_now(void) { return vsc_meter.tempo; }
W_EXPORT(playing) unsigned w_playing(void) { return vsc_meter.playing; }
W_EXPORT(learn_entry) unsigned w_learn_entry(unsigned l) { return l < VSC_LEARN ? (unsigned)vsc_dev.learn[l].ch | (unsigned)vsc_dev.learn[l].cc << 8 | (unsigned)vsc_dev.learn[l].track << 16 | (unsigned)vsc_dev.learn[l].param << 24 : 0; }
W_EXPORT(np) unsigned w_np(void) { return VSC_NP; }

/* ---- the panel's LEDs and the FM-1's own settings ---- */
static uint8_t leds[41];
W_EXPORT(leds) uint8_t *w_leds(void) { vsc_ui_leds(leds); return leds; }
W_EXPORT(device) struct vsc_device *w_device(void) { return &vsc_dev; }
W_EXPORT(device_bytes) unsigned w_device_bytes(void) { return sizeof vsc_dev; }
W_EXPORT(device_seal) void w_device_seal(void) { vsc_device_seal(); }
W_EXPORT(device_check) int w_device_check(void) { return vsc_device_valid(&vsc_dev); }
W_EXPORT(device_changed) unsigned w_device_changed(void) { return vsc_dev_changed; }
W_EXPORT(cpu_set) void w_cpu_set(unsigned cpu) { vsc_host.cpu = (uint8_t)cpu; }
