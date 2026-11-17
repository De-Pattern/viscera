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
static uint8_t safe_mode;   /* OCT- and OCT+ held at power-on, or two failed starts: no engine, updates still work */
static uint8_t boot_pending;            /* this start hasn't yet been marked good */
static uint8_t started;                 /* the first control pass has run start() */
static uint32_t boot_at;
#define BOOT_FAILED_STARTS 3u           /* starts that never ran well before safe mode takes over */
#define BOOT_GOOD_MS 4000u              /* running this long counts as a good start */
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

static uint32_t dev_saved_gen, dev_seen_gen, dev_changed_at;
static void host(unsigned req, unsigned arg)
{
    int r;
    switch (req) {
    case HOST_BT_AUDIO: configure_bt(arg ? bt_features | FM1_BT_AUDIO : bt_features & ~FM1_BT_AUDIO); break;
    case HOST_BT_MIDI: configure_bt(arg ? bt_features | FM1_BT_MIDI : bt_features & ~FM1_BT_MIDI); break;
    case HOST_BLE_ROLE: (void)fm1_ble_set_role(arg ? FM1_BLE_CENTRAL : FM1_BLE_PERIPHERAL); break;
    case HOST_BLE_SCAN: if (fm1_ble_scan() == 0) vsc_ui_notice("SCANNING", 1500); break;
    case HOST_BLE_CONNECT: (void)fm1_ble_connect(arg); break;
    case HOST_BLE_FORGET: if (fm1_ble_forget_bonds() == 0) vsc_ui_notice("UNPAIRED", 1500); break;
    case HOST_BLE_DISCONNECT: (void)fm1_ble_disconnect(); break;
    case HOST_SAVE:
        r = vsc_fm1_save(arg);
        vsc_ui_notice(r == 0 ? "SAVED" : r == -2 ? "STOP TO SAVE" : "SAVE FAILED", 1500);
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
    /* The panel and the display first, as M-VAVE's own start-up does: the scanner
     * (keys, buttons, encoders, LEDs) and the LCD (its controller set up and switched
     * on). Without these the screen stays dark and nothing on the panel answers. */
    fm1_controls_init();
    for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
    fm1_screen_init();
    boot_at = fm1_millis();

    /* Nothing here may wait: the library calls this from its start-up, where an RTOS sleep
     * (fm1_device_delay) never comes back, and the FM-1 then never starts (no screen, no USB).
     * Safe mode (OCT- and OCT+ held) is read on the first control passes instead, once the
     * scanner has had time to see the buttons. */
    safe_mode = 0;
    /* the screen can draw from here; the engine starts on the first control pass,
     * after the start guard (flash is for the control task only) */
    vsc_project_defaults(&vsc);
    vsc_ui_init();
    for (unsigned i = 0; i < VSC_SCREEN_H / VSC_BAND; ++i) band_hash[i] = 0;
    next_band = 0;
    started = 0;
}

/* First control pass: the FM-1's settings, the start guard, then the engine (or safe
 * mode, after two starts that never ran well). */
static void run_engine(void);
static int start_tries;   /* starts since the last good one (the guard's count) */
static void start(void)
{
    started = 1;
    vsc_fm1_device_load();                       /* the settings first: the guard may rewrite their sector */
    int tries = vsc_fm1_boot_begin();
    start_tries = tries;
    boot_pending = tries >= 0;
    if (tries >= (int)BOOT_FAILED_STARTS) safe_mode = 1;
    if (safe_mode) { vsc_ui_notice("SAFE MODE  PLAY: START", 0x7fffffffu); return; }
    run_engine();
}
/* START WITH LAST: the project last saved or loaded opens, unless the last start never ran
 * well (a crash or a hang: a project it can't open can never stop it starting); then it
 * starts blank and says so */
static char open_msg[32];
static void open_last(void)
{
    unsigned s = vsc_dev.last_slot;
    if (vsc_dev.start_blank || !s || s > VSC_PROJECTS) return;
    if (start_tries != 0) { vsc_ui_notice("AFTER A RESET: NEW PROJECT", 6000); return; }
    int r = vsc_fm1_load(s - 1u);
    if (r == 0) {
        const char *a = "OPENED ";
        unsigned i = 0;
        while (*a) open_msg[i++] = *a++;
        open_msg[i++] = (char)('0' + s); open_msg[i++] = ' ';
        for (unsigned k = 0; k < 11 && vsc.name[k]; ++k) open_msg[i++] = vsc.name[k];
        open_msg[i] = 0;
        vsc_ui_notice(open_msg, 6000);
    } else if (r == -4) vsc_ui_notice("LAST PROJECT DAMAGED: NEW", 6000);
}
/* the engine and the rest; from safe mode too, when PLAY is pressed */
static void run_engine(void)
{
    safe_mode = 0;
    vsc_ui_notice("", 0);
    vsc_fm1_start();
    vsc_ui_init();
    configure_bt(FM1_BT_AUDIO | FM1_BT_MIDI);
    refresh_slots();
    vsc_ui_intro();
    open_last();
}

/* A start that has run for 4 s is a good one: mark it (one byte of flash, no erase).
 * In safe mode too, so the next power-on tries Viscera again. */
static void boot_check(uint32_t now)
{
    if (!boot_pending || now - boot_at < BOOT_GOOD_MS) return;
    boot_pending = 0;
    (void)vsc_fm1_boot_good();
}

/* MIDI Machine Control from the transport layer: 1 stop, 2 play. */
void groove_screen_mmc(unsigned command)
{
    if (command == 1) vsc_event(EV_STOP, 0, 0);
    else if (command == 2 && !vsc_meter.playing) vsc_event(EV_PLAY, 1, 0);
}

/* The panel's LEDs: lit ones on; the rest glow dimly with the backlight, which the panel
 * scan lights itself one frame in a few (as Ravel's), so it is steady however busy this loop
 * is. fm1_led_set and the dim mask are touched only when a LED changes. */
static uint8_t led_now[41], led_want[41], led_dim[41], back_set = 255;
static void leds(uint32_t now)
{
    (void)now;
    vsc_ui_leds(led_want);
    static const uint8_t div[4] = {0, 6, 3, 2};   /* OFF, LOW, MID, HIGH */
    uint8_t b = vsc_dev.backlight & 3u;
    if (b != back_set) { back_set = b; vsc_board_backlight(div[b]); }
    for (unsigned i = 0; i < 41; ++i) {
        uint8_t on = led_want[i] == 2, dim = led_want[i] == 1;
        if (on != led_now[i]) { led_now[i] = on; fm1_led_set(i, on); }
        if (dim != led_dim[i]) { led_dim[i] = dim; vsc_board_led_dim(i, dim); }
    }
}
/* the FM-1's own settings go to flash a second after the last change, while stopped */
static void device_settings(uint32_t now)
{
    uint32_t g = vsc_dev_changed;
    if (g != dev_seen_gen) { dev_seen_gen = g; dev_changed_at = now; }
    if (g == dev_saved_gen || vsc_meter.playing || now - dev_changed_at < 1000u) return;
    dev_saved_gen = g;
    if (vsc_fm1_device_save() != 0) vsc_ui_notice("NOT SAVED", 1200);
}

void groove_screen_control(void)
{
    struct fm1_controls c;
    fm1_controls_read(&c);
    uint32_t now = fm1_millis();
    if (!started) {                           /* the first 150 ms: OCT- and OCT+ held is safe mode */
        if ((c.buttons & 3u) == 3u) safe_mode = 1;   /* buttons 0 and 1: OCT- and OCT+ */
        if (now - boot_at < 150u) { fm1_control_request(); return; }
        start();
    }
    if (safe_mode) {
        /* only the screen: the engine hasn't started, so nothing can stall. PLAY starts it. */
        boot_check(now);
        if (c.buttons & (1u << BTN_PLAY)) { run_engine(); return; }
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
    boot_check(now);

    if (now - status_at >= 250u) {
        status_at = now; refresh_status();
        {   /* the battery: a slow average of its divider (10 bit), in Ravel's bars (531 / 561 / 591);
             * on a USB host's power the reading says little, so a bolt */
            static int32_t raw;
            int32_t b = (int32_t)c.battery;
            if (b > 0) raw = raw ? raw + (b - raw) / 8 : b;
            vsc_host.battery = vsc_host.usb_connected ? 4u : !raw ? 255u : raw >= 591 ? 3u : raw >= 561 ? 2u : raw >= 531 ? 1u : 0u;
        }
        { unsigned pk = 0, cpu = vsc_fm1_cpu(&pk); vsc_host.cpu = (uint8_t)(cpu > 254u ? 254u : cpu); vsc_host.cpu_peak = (uint8_t)(pk > 255u ? 255u : pk); }
        vsc_fm1_parts(vsc_host.part);
        { static uint8_t shown; uint8_t r = vsc_relief; vsc_host.relief = r;
          if (r > shown) vsc_ui_notice("CPU HIGH: EASED", 1500);
          shown = r; }
        if (vsc_fm1_overload) vsc_ui_notice("CPU OVERLOAD: PAUSED", 1200);
    }
    if (now - slots_at >= 3000u) { slots_at = now; refresh_slots(); }

    /* screen: one band per call while the DMA is free; a new frame every ~33 ms, every
     * ~100 ms while the guard eases or the audio takes over 60% (it shares the core) */
    if (fm1_screen_pending() != 0) return;
    if (next_band == 0) {
        if (now - frame_at < (vsc_relief || vsc_host.cpu > 60u ? 100u : 33u)) return;
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
