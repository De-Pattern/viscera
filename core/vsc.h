/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: a four-track sculpting sampler for the M-VAVE FM-1.
 * Portable core (C99, single-precision float, no C library). */
#ifndef VSC_H
#define VSC_H
#include <stdint.h>
#include <stddef.h>
#include "vsc_config.h"

/* ---- Parameters -------------------------------------------------------
 * Every parameter is a uint8_t from 0 to its max. Continuous parameters run
 * 0..127 and can be modulated; enums cannot. */
enum vsc_tparam {
    /* MATERIAL: PLAY (from the flash bank), LOOP (tape), KEYS (polyphonic), THRU */
    P_MODE, P_SLOT, P_START, P_LEN, P_SPEED, P_PITCH,
    P_LOOPM, P_INPUT, P_DUB, P_SIZE, P_QUAL, P_GAIN,
    P_ATK, P_DEC, P_SUS, P_REL, P_GLIDE, P_VELO,
    /* GRAIN */
    P_GMIX, P_GSIZE, P_GDENS, P_GPITCH, P_GSPRAY, P_GSHAPE,
    P_GTIME, P_GREV, P_GFREEZE, P_GDETUNE, P_GQUANT, P_GFDBK, P_GWMODE,
    /* FILTER: RESON (the resonator bank) or a classic filter */
    P_FTYPE, P_FCUT, P_FRES, P_FSPREAD, P_FSCALE, P_FMIX, P_FWMODE,
    P_KDECAY, P_KPITCH, P_KSLOPE, P_KTONE, P_KSCALE, P_KWAVES,
    P_KWRATE, P_KNOISE, P_KNRATE, P_KTILT, P_KDETUN, P_KDMODE,
    /* COLOR: noise -> drive -> crush -> compress, then tilt */
    P_DRIVE, P_COMP, P_CRUSH, P_CMODE, P_TILT, P_XOVER,
    P_NOISE, P_NDECAY, P_NTONE, P_NTYPE, P_CMIX, P_CWMODE,
    /* MIX */
    P_LEVEL, P_PAN, P_SEND, P_MUTE, P_DJF,
    /* MODS: four slots, each a TYPE and nine parameters whose meaning follows the type */
    P_MOD0,
    /* SEQUENCER: STEPS or CAROM, and CAROM's physics */
    P_SEQM = P_MOD0 + 40, P_RSPEED, P_RGRAV, P_RLOSS, P_RSPREAD, P_RQUANT,
    P_RCOUNT, P_RWALLS, P_RJITTER, P_RGATE,
    VSC_NP
};
#define VSC_MOD_PARAMS 10
#define P_MOD(m, k) (P_MOD0 + (m) * VSC_MOD_PARAMS + (k))
enum vsc_gparam {
    G_SWING, G_DTIME, G_DFEED, G_DTONE, G_DLONG, G_DLEVEL,
    G_RSIZE, G_RDAMP, G_RLEVEL, G_MAIN, G_CLOCK, G_INMON,
    G_MORPH, G_TARGET, G_MSEL, G_MCOMP,
    G_M0, G_M1, G_M2, G_M3, G_M4, G_M5, G_M6, G_M7,
    VSC_NG
};
enum { MODE_PLAY, MODE_LOOP, MODE_KEYS, MODE_THRU };
enum { IN_BT, IN_USB, IN_BOTH, IN_T1, IN_T2, IN_T3, IN_T4, IN_MAIN, IN_COUNT };
enum { FT_LP, FT_BP, FT_HP, FT_PEAKS, FT_RING, FT_OFF };
enum { RING_SUSTAIN, RING_CHOKE };
#define VSC_RING_BANDS 10
enum { MT_WAVE, MT_RANDOM, MT_FOLLOW, MT_ADSR };
enum { WET_MIX, WET_SEND };
enum { CLOCK_AUTO, CLOCK_INT, CLOCK_MIDI };
enum { SEQ_STEPS, SEQ_CAROM };
#define VSC_BALLS 8

#define VSC_PF_ENUM    1
#define VSC_PF_BIPOLAR 2
#define VSC_PF_GLOBAL  4
struct vsc_pinfo {
    char name[7];
    uint8_t max, def, flags;
    const char *const *names;  /* enum labels, or NULL */
};
/* A track parameter's description. Modulator parameters depend on the slot's TYPE. */
const struct vsc_pinfo *vsc_tparam_info(unsigned track, unsigned p);
/* The description of modulator parameter k (1..9) for a modulator type. */
const struct vsc_pinfo *vsc_mod_info(unsigned type, unsigned k);
/* Puts a modulator slot's nine parameters back to its type's defaults. */
void vsc_mod_defaults(unsigned track, unsigned slot);
const struct vsc_pinfo *vsc_gparam_info(unsigned p);
/* Writes a short value label (up to 7 chars) into out. */
void vsc_param_label(const struct vsc_pinfo *info, unsigned p, unsigned value, char out[8], int global);
/* The panel's LEDs as the UI wants them: 0..13 buttons, 14..40 the 27 keys;
 * 0 off, 1 the backlight's glow, 2 lit. */
void vsc_ui_leds(uint8_t out[41]);
/* CC 110-113 from MIDI: turn KNOB 1-4 of the page on screen (MIDI task). */
void vsc_ui_knob_cc(unsigned knob, unsigned value);
/* Every destination the mod matrix offers, in menu order (track and global codes). */
unsigned vsc_mod_dest_count(void);
unsigned vsc_mod_dest(unsigned i);
/* A destination's name with its section, e.g. "G.SIZE" (up to 9 chars). */
void vsc_mod_dest_name(unsigned code, char out[10]);

/* ---- Project data (saved and recalled as one block) -------------------- */
struct vsc_step { uint8_t on, note, vel, lp[2], lv[2], pad; };
/* A modulation route: param < VSC_NP is a track parameter, VSC_GDEST + g a global one,
 * 255 an empty route. */
struct vsc_route { uint8_t src, param; int8_t depth; uint8_t flags; };   /* flags bit 0: bipolar */
/* MIDI learn: a CC on a channel drives a parameter (code as for routes). */
struct vsc_learn { uint8_t ch, cc, track, param; };
#define VSC_LEARN 16
#define VSC_GDEST 128
#define VSC_FORMAT 4   /* project layout version: bump when the parameters change */
struct vsc_mroute { uint8_t track, param; int8_t depth; uint8_t pad; };
struct vsc_scene { uint8_t used, pad[3]; uint8_t t[VSC_TRACKS][VSC_NP]; uint8_t macro[VSC_MACROS]; };
struct vsc_project {
    uint32_t magic, version, bytes, crc;
    char name[12];
    uint16_t tempo, pad0;
    uint8_t t[VSC_TRACKS][VSC_NP];
    uint8_t g[VSC_NG];
    uint8_t length[VSC_TRACKS];
    struct vsc_route route[VSC_TRACKS][VSC_MOD_ROUTES];
    struct vsc_mroute macro[VSC_MACROS][VSC_MACRO_ROUTES];
    struct vsc_step step[VSC_TRACKS][VSC_STEPS];
    struct vsc_scene scene[VSC_SCENES];
};
/* Settings that belong to the FM-1, not to a project: kept through power-off and
 * whatever project loads. The platform stores them when vsc_dev_changed moves. */
enum { BACKLIGHT_OFF, BACKLIGHT_LOW, BACKLIGHT_MID, BACKLIGHT_HIGH };
struct vsc_device {
    uint32_t magic, version, bytes, crc;
    uint8_t backlight, knob_cc, metro, pad;
    struct vsc_learn learn[VSC_LEARN];
};
#define VSC_DEV_MAGIC 0x44435356u   /* 'VSCD' */
extern struct vsc_device vsc_dev;
extern volatile uint32_t vsc_dev_changed;   /* bumped on every change */
void vsc_device_defaults(void);
int vsc_device_valid(const struct vsc_device *d);
void vsc_device_seal(void);
/* Learned controls let go and wait to be picked up again (after a project or scene change). */
void vsc_learn_release(void);
#define VSC_MAGIC 0x31435356u /* "VSC1" */

/* ---- Platform services (implemented per target) ----------------------- */
/* Sample bank: number of frames in a slot (0 = empty), its name, and one
 * decoded chunk of VSC_CHUNK int16 frames. vsc_bank_chunk is called from the
 * audio context and must not block. */
unsigned vsc_bank_frames(unsigned slot);
const char *vsc_bank_name(unsigned slot);
const int16_t *vsc_bank_chunk(unsigned slot, unsigned chunk);

/* ---- Engine (audio context) ------------------------------------------- */
struct vsc_inputs {
    const float *bt, *usb;     /* VSC_BLOCK interleaved stereo frames, or NULL */
};
void vsc_init(void);
/* Render VSC_BLOCK stereo frames into out (interleaved, -1..1). */
void vsc_render(const struct vsc_inputs *in, float *out);

/* Events from control or MIDI into the audio context (lock-free queue). */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_PLAY, EV_STOP, EV_REC, EV_CLOCK, EV_START, EV_CONT,
       EV_PANIC, EV_LOOP_CLEAR, EV_BALL, EV_BALL_CLEAR, EV_CC, EV_PC, EV_ACTION };
/* EV_ACTION a: */
enum { ACT_SPACE_FREEZE, ACT_SPACE_CLEAR, ACT_MOD_TRIG, ACT_TAPE_REWIND, ACT_TAPE_REVERSE };
/* MIDI learn: the UI arms it with a parameter (track, code); the next CC to arrive
 * takes it. vsc_learn_seen counts completed learns, for the UI's notice. */
extern volatile uint8_t vsc_learn_armed, vsc_learn_track, vsc_learn_param;
extern volatile uint8_t vsc_learn_seen;
void vsc_event(unsigned type, unsigned track, unsigned a, unsigned b);
/* Same, from the MIDI routing task (its own queue). */
void vsc_midi_event(unsigned type, unsigned track, unsigned a, unsigned b);
/* A complete MIDI message from a port (0 MIDI jack, 1 USB, 2 Bluetooth). MIDI task only. */
void vsc_midi_in(unsigned port, const uint8_t *m, unsigned n, uint32_t now_ms);

/* ---- State shared with the UI ----------------------------------------- */
struct vsc_meter {
    float peak[VSC_TRACKS], main_peak, cpu;
    uint32_t pos_play[VSC_TRACKS];   /* material read position in source frames */
    uint32_t pos_len[VSC_TRACKS];    /* source length in frames */
    uint32_t grain_write[VSC_TRACKS];
    uint16_t grain_pos[VSC_TRACKS][VSC_GRAINS]; /* 0..65535 position in ring, 0xffff idle */
    uint8_t grain_amp[VSC_TRACKS][VSC_GRAINS];
    float mod_value[VSC_TRACKS][VSC_MODS];
    float mod_phase[VSC_TRACKS][VSC_MODS];   /* WAVE/RANDOM: where in the cycle (0..1) */
    uint8_t loop_state[VSC_TRACKS];  /* 0 empty, 1 recording, 2 playing, 3 overdub */
    uint8_t step[VSC_TRACKS];
    /* CAROM: ball positions 0..255, a flash that fades after each wall hit */
    uint8_t ball_x[VSC_TRACKS][VSC_BALLS], ball_y[VSC_TRACKS][VSC_BALLS], ball_hit[VSC_TRACKS][VSC_BALLS];
    uint8_t ball_on[VSC_TRACKS][VSC_BALLS];
    uint8_t playing, ext_clock, bt_in, usb_in, space_frozen, reverse[VSC_TRACKS];
    uint8_t overview[VSC_TRACKS][240];   /* PLAY/KEYS: the sample's shape, 0..60 per column, filled by the audio side */
    uint8_t overview_slot[VSC_TRACKS];   /* which slot it shows (255: none yet) */
    float tempo;                     /* the tempo in use: internal, or measured from MIDI clock */
    /* RING: each band's place on the screen's frequency axis (0..239) and its level */
    uint8_t ring_x[VSC_TRACKS][VSC_RING_BANDS], ring_amp[VSC_TRACKS][VSC_RING_BANDS];
};
extern struct vsc_project vsc;   /* the live project */
extern struct vsc_meter vsc_meter;
extern uint8_t vsc_lock[VSC_TRACKS][VSC_NP]; /* 255 = no lock */
extern float vsc_scope[2][128];              /* last main output block, for the UI */

/* Loop memory, for the UI's overview (8-bit mu-law). */
const uint8_t *vsc_loop_memory(unsigned track, uint32_t *frames, uint32_t *capacity);
const uint8_t *vsc_grain_ring(unsigned track);
uint32_t vsc_loop_base(unsigned track);   /* track's offset into the loop memory */
float vsc_ulaw(uint8_t v);

/* ---- Control helpers --------------------------------------------------- */
void vsc_project_defaults(struct vsc_project *p);
void vsc_track_defaults(struct vsc_project *p, unsigned track);
void vsc_scene_store(unsigned scene);
void vsc_scene_recall(unsigned scene);
uint32_t vsc_crc32(const void *data, size_t bytes);
int vsc_project_valid(const struct vsc_project *p);
/* Sets or clears a modulation route; depth -127..127, 0 removes it. param is a
 * track parameter, or VSC_GDEST + a global one. */
void vsc_route_set(unsigned track, unsigned src, unsigned param, int depth);
int vsc_route_get(unsigned track, unsigned src, unsigned param);
void vsc_macro_set(unsigned macro, unsigned track, unsigned param, int depth);
int vsc_macro_get(unsigned macro, unsigned track, unsigned param);

/* ---- Tiny libc replacements (the audio path may not call the C library) */
void vsc_memset(void *d, int v, size_t n);
void vsc_memcpy(void *d, const void *s, size_t n);
size_t vsc_strlen(const char *s);

/* ---- UI (control context) ---------------------------------------------- */
#define VSC_SCREEN_W 240
#define VSC_SCREEN_H 240
#define VSC_BAND 16
enum { BTN_OCTD, BTN_OCTU, BTN_FX, BTN_SCL, BTN_ENV, BTN_LFO, BTN_EDIT, BTN_GLO,
       BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC, BTN_COUNT };
struct vsc_ui_input {
    uint32_t keys, buttons;   /* keys bit 0 = F3 ... bit 26 = G5 */
    int16_t enc[7];
    uint32_t now_ms;
};
/* Host actions the UI asks the platform to perform. */
enum { HOST_NONE, HOST_SAVE, HOST_LOAD, HOST_BT_AUDIO, HOST_BT_MIDI, HOST_BLE_ROLE,
       HOST_BLE_SCAN, HOST_BLE_CONNECT, HOST_BLE_FORGET, HOST_BLE_DISCONNECT };
struct vsc_host_status {
    uint8_t bt_audio, bt_midi, bt_audio_connected, bt_audio_playing, usb_connected;
    uint8_t ble_role, ble_state, ble_count;
    char ble_name[8][16];
    uint8_t cpu, cpu_peak;            /* render load, percent */
    uint8_t slot_used[VSC_PROJECTS];
    char slot_name[VSC_PROJECTS][12];
    uint8_t storage_ok, battery;
};
extern struct vsc_host_status vsc_host;
void vsc_ui_init(void);
/* Feed one control snapshot. Returns a HOST_* request (and its argument). */
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg);
void vsc_ui_notice(const char *text, uint32_t ms);
/* Paint rows y0..y0+VSC_BAND-1 into a VSC_W*VSC_BAND RGB565 buffer. */
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels);
/* Called once before painting a frame's bands. */
void vsc_ui_frame(uint32_t now_ms);

#endif
