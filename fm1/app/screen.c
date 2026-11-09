/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: replaces the stock screen.c. The SDK calls groove_screen_init once
 * and groove_screen_control from its control task; this file reads the panel,
 * runs the Viscera UI, carries out its requests (Bluetooth, storage) and sends
 * the screen to the LCD band by band. */
#include "fm1.h"
#include "fm1_device.h"
#include "fm1_engine_control.h"
#include "instrument.h"
#include "viscera/vsc.h"
#include "viscera_fm1.h"

static uint16_t pixels[VSC_SCREEN_W * VSC_BAND];
static uint32_t band_hash[VSC_SCREEN_H / VSC_BAND];
static unsigned next_band, bt_features, status_at, slots_at;
static uint8_t safe_mode;   /* OCT- and OCT+ held at power-on: no engine, no UI, updates still work */
static uint32_t frame_at;

static uint32_t hash(const uint16_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void refresh_slots(void)
{
    vsc_host.storage_ok = (uint8_t)vsc_fm1_storage_ok();
    for (unsigned s = 0; s < VSC_PROJECTS; ++s) {
        char name[12];
        vsc_host.slot_used[s] = (uint8_t)vsc_fm1_slot_info(s, name);
        for (unsigned i = 0; i < 12; ++i) vsc_host.slot_name[s][i] = vsc_host.slot_used[s] ? name[i] : 0;
        vsc_host.slot_name[s][11] = 0;
    }
}
static void refresh_status(void)
{
    struct fm1_bluetooth_status bt;
    struct fm1_ble_status ble;
    fm1_bluetooth_get_status(&bt);
    fm1_ble_get_status(&ble);
    vsc_host.bt_audio = (bt_features & FM1_BT_AUDIO) != 0;
    vsc_host.bt_midi = (bt_features & FM1_BT_MIDI) != 0;
    vsc_host.bt_audio_connected = bt.audio_connected;
    vsc_host.bt_audio_playing = bt.audio_playing;
    vsc_host.usb_connected = (uint8_t)fm1_device_usb_connected();
    vsc_host.ble_role = ble.role;
    vsc_host.ble_state = ble.state;
    vsc_host.ble_count = ble.count > 8 ? 8 : ble.count;
    for (unsigned i = 0; i < vsc_host.ble_count; ++i) {
        struct fm1_ble_device d;
        if (fm1_ble_get_device(i, &d)) { vsc_host.ble_name[i][0] = 0; continue; }
        for (unsigned k = 0; k < 15; ++k) vsc_host.ble_name[i][k] = d.name[k];
        vsc_host.ble_name[i][15] = 0;
    }
}
static void configure_bt(unsigned features)
{
    if (fm1_bluetooth_configure(features) == 0) bt_features = features;
}

static void host(unsigned req, unsigned arg)
{
    int r;
    switch (req) {
    case HOST_BT_AUDIO: configure_bt(arg ? bt_features | FM1_BT_AUDIO : bt_features & ~FM1_BT_AUDIO); break;
    case HOST_BT_MIDI: configure_bt(arg ? bt_features | FM1_BT_MIDI : bt_features & ~FM1_BT_MIDI); break;
    case HOST_BLE_ROLE: (void)fm1_ble_set_role(arg ? FM1_BLE_CENTRAL : FM1_BLE_PERIPHERAL); break;
    case HOST_BLE_SCAN: if (fm1_ble_scan() == 0) vsc_ui_notice("SCANNING FOR 15 S", 1500); break;
    case HOST_BLE_CONNECT: (void)fm1_ble_connect(arg); break;
    case HOST_BLE_FORGET: if (fm1_ble_forget_bonds() == 0) vsc_ui_notice("PAIRINGS FORGOTTEN", 1500); break;
    case HOST_BLE_DISCONNECT: (void)fm1_ble_disconnect(); break;
    case HOST_SAVE:
        r = vsc_fm1_save(arg);
        vsc_ui_notice(r == 0 ? "SAVED" : r == -2 ? "STOP PLAYBACK TO SAVE" : "SAVE FAILED", 1500);
        refresh_slots();
        break;
    case HOST_LOAD:
        r = vsc_fm1_load(arg);
        vsc_ui_notice(r == 0 ? "LOADED" : "LOAD FAILED", 1500);
        break;
    default: break;
    }
}

void groove_screen_init(void)
{
    struct fm1_controls c;
    /* the panel scanner may need a moment after power-on: sample for ~60 ms */
    safe_mode = 0;
    for (unsigned i = 0; i < 6 && !safe_mode; ++i) {
        fm1_controls_read(&c);
        safe_mode = (c.buttons & 3u) == 3u;       /* buttons 0 and 1: OCT- and OCT+ */
        if (!safe_mode) fm1_device_delay(10);
    }
    if (safe_mode) {
        vsc_project_defaults(&vsc);               /* for the screen only: the engine never starts */
        vsc_ui_init();
        vsc_ui_notice("SAFE MODE: CONNECT TO REINSTALL", 0x7fffffffu);
        return;
    }
    vsc_fm1_start();
    vsc_ui_init();
    configure_bt(FM1_BT_AUDIO | FM1_BT_MIDI);
    for (unsigned i = 0; i < VSC_SCREEN_H / VSC_BAND; ++i) band_hash[i] = 0;
    next_band = 0;
    refresh_slots();
}

/* MIDI Machine Control from the transport layer: 1 stop, 2 play. */
void groove_screen_mmc(unsigned command)
{
    if (command == 1) vsc_event(EV_STOP, 0, 0, 0);
    else if (command == 2 && !vsc_meter.playing) vsc_event(EV_PLAY, 0, 0, 0);
}

/* The panel's LEDs: lit ones on, the backlight's glow as a short on-time every few
 * calls. The glow only runs while this loop is fast enough not to flicker; otherwise
 * unlit keys stay dark. fm1_led_set is called only when a LED changes. */
static uint8_t led_now[41], led_want[41];
static uint32_t led_calls, led_ms0;
static uint8_t glow_ok;
static void leds(uint32_t now)
{
    if (now - led_ms0 >= 1000u) { glow_ok = led_calls >= 1500u; led_calls = 0; led_ms0 = now; }   /* >= 1.5 kHz */
    ++led_calls;
    vsc_ui_leds(led_want);
    static const uint8_t duty[4] = {0, 1, 2, 4};               /* of 16 calls */
    unsigned phase = led_calls & 15u, d = duty[vsc_dev.backlight & 3];
    for (unsigned i = 0; i < 41; ++i) {
        uint8_t on = led_want[i] == 2 || (led_want[i] == 1 && glow_ok && phase < d);
        if (on != led_now[i]) { led_now[i] = on; fm1_led_set(i, on); }
    }
}
/* the FM-1's own settings go to flash a second after the last change, while stopped */
static uint32_t dev_saved_gen, dev_seen_gen, dev_changed_at;
static void device_settings(uint32_t now)
{
    uint32_t g = vsc_dev_changed;
    if (g != dev_seen_gen) { dev_seen_gen = g; dev_changed_at = now; }
    if (g == dev_saved_gen || vsc_meter.playing || now - dev_changed_at < 1000u) return;
    dev_saved_gen = g;
    if (vsc_fm1_device_save() != 0) vsc_ui_notice("SETTINGS NOT SAVED", 1200);
}

void groove_screen_control(void)
{
    struct fm1_controls c;
    fm1_controls_read(&c);
    uint32_t now = fm1_millis();
    if (safe_mode) {
        /* only the screen: the engine never starts, so nothing can stall */
        if (fm1_screen_pending() != 0) return;
        if (next_band == 0) { if (now - frame_at < 250u) return; frame_at = now; }
        vsc_ui_paint_band(next_band * VSC_BAND, pixels);
        (void)fm1_screen_submit(0, next_band * VSC_BAND, VSC_SCREEN_W, VSC_BAND, pixels, VSC_SCREEN_W * VSC_BAND);
        next_band = (next_band + 1) % (VSC_SCREEN_H / VSC_BAND);
        return;
    }
    (void)groove_master_update(c.knob);   /* the MASTER knob sets the output level */

    struct vsc_ui_input in;
    in.keys = c.keys; in.buttons = c.buttons; in.now_ms = now;
    for (unsigned i = 0; i < 7; ++i) in.enc[i] = c.encoders[i];
    unsigned arg = 0, req = vsc_ui_update(&in, &arg);
    if (req) host(req, arg);
    leds(now);
    device_settings(now);

    if (now - status_at >= 250u) {
        status_at = now; refresh_status(); vsc_host.battery = (uint8_t)(c.battery >> 2);
        { unsigned pk = 0, cpu = vsc_fm1_cpu(&pk); vsc_host.cpu = (uint8_t)(cpu > 255u ? 255u : cpu); vsc_host.cpu_peak = (uint8_t)(pk > 255u ? 255u : pk); }
        if (vsc_fm1_overload) vsc_ui_notice("CPU OVERLOAD: SOUND PAUSED", 1200);
    }
    if (now - slots_at >= 3000u) { slots_at = now; refresh_slots(); }

    /* screen: one band per call while the DMA is free; a new frame every ~33 ms */
    if (fm1_screen_pending() != 0) return;
    if (next_band == 0) {
        if (now - frame_at < 33u) return;
        frame_at = now;
        vsc_ui_frame(now);
    }
    unsigned y = next_band * VSC_BAND;
    vsc_ui_paint_band(y, pixels);
    uint32_t h = hash(pixels, VSC_SCREEN_W * VSC_BAND);
    if (h != band_hash[next_band]) {
        if (fm1_screen_submit(0, y, VSC_SCREEN_W, VSC_BAND, pixels, VSC_SCREEN_W * VSC_BAND) == 0)
            band_hash[next_band] = h;
        else return;   /* busy: try this band again next call */
    }
    next_band = (next_band + 1) % (VSC_SCREEN_H / VSC_BAND);
    fm1_control_request();
}
