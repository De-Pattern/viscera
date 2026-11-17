/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track in the browser: the platform layer for the same core the FM-1 runs.
 * Built with clang --target=wasm32, no C library. The page's AudioWorklet calls
 * render once per 128 frames; the page asks for the screen and sends controls. */
#include "../core/vsc.h"

#define W_EXPORT(name) __attribute__((export_name(#name)))

/* ---- the small part of the C library the compiler may call ---- */
void *memset(void *d, int v, unsigned long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }

/* ---- audio: the page's input stands in for Bluetooth audio (and USB, for the tests) ---- */
static float in_bt[VSC_BLOCK * 2], in_usb[VSC_BLOCK * 2], out_buf[VSC_BLOCK * 2];
W_EXPORT(input_buffer) float *w_input(void) { return in_bt; }
W_EXPORT(usb_buffer) float *w_usb(void) { return in_usb; }
W_EXPORT(output_buffer) float *w_output(void) { return out_buf; }
W_EXPORT(init) void w_init(void) { vsc_init(); vsc_project_defaults(&vsc); vsc_ui_init(); vsc_host.cpu = 255; vsc_host.bt_audio = 1; vsc_host.battery = 3; }   /* (a full battery, to show the glyph) */
W_EXPORT(intro) void w_intro(unsigned now) { (void)vsc_ui_update(&(struct vsc_ui_input){0, 0, {0}, now}, &(unsigned){0}); vsc_ui_intro(); }
/* has_input: bit 0 the page's live input (as Bluetooth), bit 1 the USB buffer */
W_EXPORT(render) void w_render(int has_input)
{
    struct vsc_inputs in = {(has_input & 1) ? in_bt : 0, (has_input & 2) ? in_usb : 0};
    vsc_host.bt_audio_connected = vsc_host.bt_audio_playing = (uint8_t)(has_input & 1);
    vsc_render(&in, out_buf);
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
    /* Bluetooth audio here is the page's input: it is always on */
    if (host_req == HOST_BT_AUDIO) { vsc_host.bt_audio = 1; host_req = HOST_NONE; }
    return host_req;
}
W_EXPORT(host_arg) unsigned w_host_arg(void) { return host_arg; }
/* status the UI shows on INPUT and SETTINGS */
W_EXPORT(set_host) void w_set_host(unsigned bt_audio, unsigned playing, unsigned usb)
{
    vsc_host.bt_audio = 1; vsc_host.bt_audio_connected = (uint8_t)bt_audio;
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
W_EXPORT(project_check) int w_project_check(void) { return vsc_project_valid(&vsc) || vsc_project_upgrade(&vsc); }
W_EXPORT(param_live) float w_param_live(unsigned p) { return vsc_param_live(p); }
W_EXPORT(route) struct vsc_route *w_route(void) { return vsc.route; }
W_EXPORT(stop_all) void w_stop_all(void) { vsc_event(EV_STOP, 0, 0); vsc_event(EV_PANIC, 0, 0); }
W_EXPORT(slot_info) void w_slot_info(unsigned slot, unsigned used, const char *name)
{
    if (slot >= VSC_PROJECTS) return;
    vsc_host.slot_used[slot] = (uint8_t)used;
    for (unsigned i = 0; i < 11; ++i) vsc_host.slot_name[slot][i] = used && name ? name[i] : 0;
    vsc_host.slot_name[slot][11] = 0;
}
static char name_stage[16];
W_EXPORT(name_buffer) char *w_name_buffer(void) { return name_stage; }
static uint32_t midi_ms;
W_EXPORT(midi) void w_midi(unsigned status, unsigned a, unsigned b)
{
    uint8_t m[3] = {(uint8_t)status, (uint8_t)a, (uint8_t)b};
    unsigned n = status >= 0xf8 ? 1u : (status & 0xe0u) == 0xc0u ? 2u : 3u;
    midi_ms += 1;
    vsc_midi_in(0, m, n, midi_ms);
}

/* ---- direct access, for the tests and tools ---- */
W_EXPORT(np) unsigned w_np(void) { return VSC_NP; }
W_EXPORT(set_param) void w_set_param(unsigned p, unsigned v) { if (p < VSC_NP) vsc.p[p] = (uint8_t)v; }
W_EXPORT(get_param) unsigned w_get_param(unsigned p) { return p < VSC_NP ? vsc.p[p] : 0; }
W_EXPORT(event) void w_event(unsigned type, unsigned a, unsigned b) { vsc_event(type, a, b); }
W_EXPORT(transport) unsigned w_transport(void) { return vsc_meter.transport; }
W_EXPORT(recording) unsigned w_recording(void) { return vsc_meter.recording; }
W_EXPORT(has) unsigned w_has(unsigned t) { return t < VSC_TRACKS ? vsc_meter.has[t] : 0; }
W_EXPORT(pos) unsigned w_pos(void) { return vsc_meter.pos; }
W_EXPORT(loop) unsigned w_loop(void) { return vsc_meter.loop; }
W_EXPORT(cap) unsigned w_cap(void) { return vsc_meter.cap; }
W_EXPORT(lstart) unsigned w_lstart(void) { return vsc_meter.lstart; }
W_EXPORT(lend) unsigned w_lend(void) { return vsc_meter.lend; }
W_EXPORT(take) unsigned w_take(void) { return vsc_meter.take; }
W_EXPORT(rate) float w_rate(void) { return vsc_meter.rate; }
W_EXPORT(vu) float w_vu(unsigned t) { return t < VSC_TRACKS ? vsc_meter.vu[t] : t == 4 ? vsc_meter.vu_in : vsc_meter.vu_l; }
W_EXPORT(peak) float w_peak(unsigned t) { return t < VSC_TRACKS ? vsc_meter.pk[t] : vsc_meter.pk_in; }
W_EXPORT(sat) unsigned w_sat(unsigned t) { return t < VSC_TRACKS ? vsc_meter.sat[t] : vsc_meter.sat_in; }
W_EXPORT(bounce) unsigned w_bounce(void) { return vsc_meter.bounce | (unsigned)vsc_meter.bounce_err << 8 | (unsigned)vsc_meter.bounce_dst << 16; }
W_EXPORT(robot) float w_robot(unsigned r) { return r < VSC_ROBOTS ? vsc_meter.robot[r] : 0.f; }
W_EXPORT(reson_pings) unsigned w_reson_pings(void) { return vsc_meter.rb_pings; }
W_EXPORT(reson_level) unsigned w_reson_level(unsigned b) { return b < VSC_RBANDS ? vsc_meter.rb_level[b] : 0; }
W_EXPORT(learn_entry) unsigned w_learn_entry(unsigned l) { return l < VSC_LEARN ? (unsigned)vsc_dev.learn[l].ch | (unsigned)vsc_dev.learn[l].cc << 8 | (unsigned)vsc_dev.learn[l].kind << 16 | (unsigned)vsc_dev.learn[l].param << 24 : 0; }
W_EXPORT(gest) signed char *w_gest(unsigned r) { return r < VSC_ROBOTS ? vsc.gest[r] : 0; }

/* ---- the panel's LEDs and the FM-1's own settings ---- */
static uint8_t leds[41];
W_EXPORT(leds) uint8_t *w_leds(void) { vsc_ui_leds(leds); return leds; }
W_EXPORT(device) struct vsc_device *w_device(void) { return &vsc_dev; }
W_EXPORT(device_bytes) unsigned w_device_bytes(void) { return sizeof vsc_dev; }
W_EXPORT(device_seal) void w_device_seal(void) { vsc_device_seal(); }
W_EXPORT(device_check) int w_device_check(void) { return vsc_device_valid(&vsc_dev); }
W_EXPORT(device_changed) unsigned w_device_changed(void) { return vsc_dev_changed; }
W_EXPORT(cpu_set) void w_cpu_set(unsigned cpu) { vsc_host.cpu = (uint8_t)cpu; }
