/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: an overdubbing four-track tape looper for the M-VAVE FM-1, with a
 * resonator to print through. Portable core (C99, single-precision float, no C library). */
#ifndef VSC_H
#define VSC_H
#include <stdint.h>
#include <stddef.h>
#include "vsc_config.h"

/* ---- Parameters -------------------------------------------------------
 * Every parameter is a uint8_t from 0 to its max. Continuous ones run 0..127 and a
 * robot can move them; enums cannot. One flat list: the tracks' strips are in it. */
#define VSC_TP 8                              /* parameters per track strip */
enum vsc_param {
    /* TAPE: varispeed (pitch and time together), the decoupled pitch shift, the loop
     * length for the first take (0: free), the tape's level, the TAPE SPEED switch */
    P_SPEED, P_PITCH, P_LOOP, P_TLEVEL, P_TAPESPD,
    /* INPUT: where it comes from, its trim and the preamp's drive, monitoring, and
     * whether a pass adds to the tape (DUB) or replaces it */
    P_ISRC, P_ITRIM, P_IDRIVE, P_IMON, P_IREC,
    /* the strips: TRIM (into the channel's preamp), PAN, LEVEL, MUTE, ARM, the track's own
     * SPEED and PITCH, and DUB (how much of what is there a DUB pass keeps) */
    P_STRIP,
    /* BOUNCE: which tracks (bits), to which (AUTO: the first free), through RESON,
     * the drive, the level, and whether the sources are cleared after */
    P_BSRC = P_STRIP + VSC_TRACKS * VSC_TP, P_BDST, P_BRESON, P_BDRIVE, P_BLEVEL, P_BCLEAR,
    /* RESON: where it sits (OFF, a track, INPUT), its profile, wet, decay, the bands'
     * width, the whole bank's shift, dust, and the eight bands' gains */
    P_RTGT, P_RPROF, P_RMIX, P_RDECAY, P_RWIDTH, P_RSHIFT, P_RDUST,
    P_RG0, P_RG1, P_RG2, P_RG3, P_RG4, P_RG5, P_RG6, P_RG7,
    /* MASTER: a four-band EQ (each band FREQ, GAIN, Q, TYPE; the first and last can shelve or
     * cut) and the bus compressor (after Ravel's COMP: threshold, ratio, attack, release,
     * makeup, mix, what it listens to, on) */
    P_EQ0,
    P_CTHR = P_EQ0 + 16, P_CRATIO, P_CATK, P_CREL, P_CMAKE, P_CMIX, P_CKEY, P_CON,
    /* SLICE: the track sliced, how many slices, whether they snap to transients, where a
     * take goes (AUTO: a free track) */
    P_SLSRC, P_SLN, P_SLSNAP, P_SLDST,
    /* ROBOTS: two, each a target, a rate, a shape, its skew and depth */
    P_ROB0,
    VSC_NP = P_ROB0 + VSC_ROBOTS * 5
};
enum { S_TRIM, S_PAN, S_LEVEL, S_MUTE, S_ARM, S_SPEED, S_PITCH, S_DUB };
enum { E_FREQ, E_GAIN, E_Q, E_TYPE };
#define P_EQ(b, k) (P_EQ0 + (b) * 4 + (k))
enum { EQ_BELL, EQ_SHELF, EQ_CUT };
#define P_TRK(t, k) (P_STRIP + (t) * VSC_TP + (k))
enum { R_TARGET, R_RATE, R_SHAPE, R_SKEW, R_DEPTH };
#define P_ROB(r, k) (P_ROB0 + (r) * 5 + (k))
enum { TS_HI, TS_MID, TS_LO };                       /* 22.05, 16, 11.025 kHz */
enum { IN_BT, IN_USB, IN_BOTH };
enum { MON_OFF, MON_ON, MON_AUTO };                  /* AUTO: while a track is armed */
enum { IREC_DUB, IREC_REPLACE };
enum { RT_OFF, RT_T1, RT_T2, RT_T3, RT_T4, RT_INPUT };
enum { RP_LIQUID, RP_GLASS, RP_THROAT, RP_DUST, RP_RADIO, RP_DROPS, RP_COUNT };
enum { RS_SINE, RS_TRI, RS_SAW, RS_SQUARE, RS_SH, RS_DRIFT, RS_GESTURE, RS_COUNT };
#define VSC_RBANDS 8

#define VSC_PF_ENUM    1
#define VSC_PF_BIPOLAR 2
struct vsc_pinfo {
    char name[7];
    uint8_t max, def, flags;
};
const struct vsc_pinfo *vsc_param_info(unsigned p);
/* Writes a short value label (up to 7 chars) into out. */
void vsc_param_label(unsigned p, unsigned value, char out[8]);
/* A robot's target: an index into this list of parameters (continuous ones) */
#define VSC_RTARGETS 24
unsigned vsc_robot_target(unsigned i);   /* the parameter, or VSC_NP for none */
void vsc_target_name(unsigned i, char out[8]);

/* The panel's LEDs as the UI wants them: 0..13 buttons, 14..40 the 27 keys;
 * 0 off, 1 the backlight's glow, 2 lit. */
void vsc_ui_leds(uint8_t out[41]);
/* CC 110-113 from MIDI: turn KNOB 1-4 of the page on screen (MIDI task). */
void vsc_ui_knob_cc(unsigned knob, unsigned value);

/* ---- Project data (saved and recalled as one block) -------------------- */
/* The tape itself stays in RAM; a project is the desk: every setting, the robots and
 * their recorded gestures. */
struct vsc_project {
    uint32_t magic, version, bytes, crc;
    char name[12];
    uint8_t p[VSC_NP];
    uint8_t pad[(4u - (VSC_NP & 3u)) & 3u];
    int8_t gest[VSC_ROBOTS][VSC_GEST];        /* a recorded knob gesture, offsets of the target */
    uint16_t gest_ms[VSC_ROBOTS];             /* how long it lasts */
    /* MOTION: knob moves recorded over the loop (SEL page), one lane a parameter, 64 points
     * round the loop, played back each pass in place of the knob */
    struct vsc_motion { uint8_t param, on, pad[2]; uint8_t v[VSC_MOTION_PTS]; } motion[VSC_MOTIONS];
};
#define VSC_MAGIC 0x34435356u /* "VSC4" */
#define VSC_FORMAT 10         /* 9: per-track speed, pitch, dub; the master EQ and compressor */
/* MIDI learn: a CC (or a note, for an action) on a channel. kind 0: CC to a parameter;
 * 1: a note to an action. */
struct vsc_learn { uint8_t ch, cc, kind, param; };
enum { ACT_REC, ACT_PLAY, ACT_ARM1, ACT_ARM2, ACT_ARM3, ACT_ARM4, ACT_BOUNCE, ACT_COUNT };
/* Settings that belong to the FM-1, not to a project: kept through power-off. The
 * platform stores them when vsc_dev_changed moves. */
enum { BACKLIGHT_OFF, BACKLIGHT_LOW, BACKLIGHT_MID, BACKLIGHT_HIGH };
struct vsc_device {
    uint32_t magic, version, bytes, crc;
    uint8_t backlight, knob_cc;
    uint8_t start_blank, last_slot;   /* START WITH: 0 the last project saved or loaded (last_slot, 1..8; 0 none), 1 a blank one */
    struct vsc_learn learn[VSC_LEARN];
};
#define VSC_DEV_MAGIC 0x44435356u   /* 'VSCD' */
#define VSC_DEV_VERSION 4           /* 4: the 4-track */
extern struct vsc_device vsc_dev;
extern volatile uint32_t vsc_dev_changed;   /* bumped on every change */
void vsc_device_defaults(void);
int vsc_device_valid(const struct vsc_device *d);
void vsc_device_seal(void);
/* Learned controls let go and wait to be picked up again (after a project change). */
void vsc_learn_release(void);

/* CPU: the platform's clock (both unset: no readout), and the guard's relief level.
 * Relief 1: RESON uses only its louder bands; 2: its quietest are off too, and no
 * droplets. */
#define VSC_PARTS 3   /* tape and mixer, RESON, the rest */
extern uint32_t (*vsc_clock_now)(void);
extern uint32_t (*vsc_clock_since)(uint32_t since);
extern volatile uint8_t vsc_relief;

/* ---- Engine (audio context) ------------------------------------------- */
struct vsc_inputs {
    const float *bt, *usb;     /* VSC_BLOCK interleaved stereo frames, or NULL */
};
void vsc_init(void);
/* Render VSC_BLOCK stereo frames into out (interleaved, -1..1). */
void vsc_render(const struct vsc_inputs *in, float *out);

/* Events from control or MIDI into the audio context (lock-free queue). */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_PLAY, EV_STOP, EV_REC, EV_CLEAR, EV_ERASE, EV_BOUNCE,
       EV_ARM, EV_CC, EV_PANIC, EV_LOOP, EV_INTRO, EV_SLICE, EV_SLMODE, EV_SLREC };
/* SLICE: EV_SLMODE a: the source track + 1 while the page is open (it is muted; the slices
 * play through its strip), 0 off. EV_SLICE a: play slice a. EV_SLREC a: 1 record what the
 * slices play into a free track (P_SLDST), 0 stop. The UI finds the slices (vsc_slice_at). */
extern uint32_t vsc_slice_at[17];   /* the slices' starts, then the last one's end (tape samples) */
extern volatile uint8_t vsc_slice_n;
/* the tape's level in frames of 256 samples from..to (for finding transients; control task) */
unsigned vsc_tape_env(unsigned t, uint32_t from, uint32_t to, uint8_t *env, unsigned max);
/* MOTION: lanes the UI is writing now (bits): they don't play back while they're written */
extern volatile uint8_t vsc_motion_hold;
/* SOLO (the mixer's keys): soloed tracks, bits; with any, the rest are silent */
extern volatile uint8_t vsc_solo;
float vsc_motion_value(unsigned lane);   /* a lane's value where the loop is now */   /* EV_INTRO a: 1 play the start-up music, 0 stop it */
/* EV_PLAY a: 0 toggle, 1 start. EV_CLEAR: track a. EV_ARM: track a toggled.
 * EV_LOOP: the loop length moves by a (signed) twentieths of a second. */
void vsc_event(unsigned type, unsigned a, unsigned b);
void vsc_midi_event(unsigned type, unsigned a, unsigned b);
/* MIDI learn: the UI arms it with a parameter (kind 0) or an action (kind 1); the next
 * CC (or note) to arrive takes it. vsc_learn_seen counts completed learns. */
extern volatile uint8_t vsc_learn_armed, vsc_learn_kind, vsc_learn_param;
extern volatile uint8_t vsc_learn_seen;
/* A complete MIDI message from a port (0 MIDI jack, 1 USB, 2 Bluetooth). MIDI task only. */
void vsc_midi_in(unsigned port, const uint8_t *m, unsigned n, uint32_t now_ms);

/* ---- State shared with the UI ----------------------------------------- */
enum { TR_STOP, TR_PLAY, TR_FIRST };     /* the transport: stopped, playing, the first take */
enum { BE_NONE, BE_EMPTY, BE_NOROOM, BE_NOLOOP };   /* a bounce: nothing to bounce, no free track, no loop yet */
/* the bounce's sources and destination as the engine will take them (BE_* or 0) */
int vsc_bounce_plan(unsigned *src_mask, unsigned *dst);     /* the transport: stopped, playing, the first take */
struct vsc_meter {
    float cpu;
    /* VU (a 300 ms average) and peaks, 0..1 linear: the four tracks, the input, the mix */
    float vu[VSC_TRACKS], vu_in, vu_l, vu_r;
    float pk[VSC_TRACKS], pk_in;
    uint8_t sat[VSC_TRACKS], sat_in;          /* the preamp went past its knee (fades) */
    uint8_t transport, recording;            /* recording: bits of the tracks being written */
    uint8_t has[VSC_TRACKS];                 /* the track holds something */
    uint8_t bounce;                          /* 1 while a bounce prints */
    uint8_t bounce_dst;                      /* where the last one went */
    uint8_t bounce_pct;
    uint8_t bounce_err;                      /* why the last one didn't start: BE_* */
    uint8_t bt_in, usb_in;                   /* audio arriving */
    uint32_t pos, loop, cap;                 /* the tape: where in the loop (samples), the loop's length, the capacity */
    uint32_t lstart, lend, take;             /* the loop's START and END, and the take they sit in (samples) */
    float rate;                              /* the tape's sample rate now (Hz, with SPEED) */
    float robot[VSC_ROBOTS];                 /* each robot's output, -1..1 */
    uint8_t overview[VSC_TRACKS][60];        /* each track's shape over the loop, 0..15 */
    uint32_t part_ticks[VSC_PARTS];          /* the last block's time per part (platform clock ticks) */
    /* RESON: each band's output level (0..255), the pings so far, droplets alive */
    uint8_t rb_level[VSC_RBANDS];
    uint32_t rb_pings;
    uint8_t rb_drops, playing;
    uint8_t sl_rec, sl_dst, sl_err, sl_on;   /* SLICE: recording, where to, why it couldn't (BE_*), voices playing */
    uint32_t tpos[VSC_TRACKS];               /* each track's own place on the tape (its SPEED) */
    float comp_gr, comp_in;                  /* the compressor: reduction now (dB), the level it hears (dB) */
};
extern struct vsc_project vsc;   /* the live project */
extern struct vsc_meter vsc_meter;
/* the master EQ's bands as they are now (b0 b1 b2 a1 a2), and which do something: for the picture */
extern float vsc_eq_coef[4][5];
extern uint8_t vsc_eq_live;

/* ---- Control helpers --------------------------------------------------- */
void vsc_project_defaults(struct vsc_project *p);
uint32_t vsc_crc32(const void *data, size_t bytes);
int vsc_project_valid(const struct vsc_project *p);
/* RESON's profiles: the values a profile sets (the knobs' too) */
void vsc_reson_profile(unsigned n);
float vsc_reson_hz(unsigned band);
/* The tape's rate at a TAPE SPEED setting (Hz) */
float vsc_tape_hz(unsigned speed);

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
    uint8_t part[VSC_PARTS + 1];      /* per part, then the platform's own (percent) */
    uint8_t relief;                  /* the CPU guard's relief level (0..2) */
    uint8_t slot_used[VSC_PROJECTS];
    char slot_name[VSC_PROJECTS][12];
    uint8_t storage_ok, battery;
};
extern struct vsc_host_status vsc_host;
void vsc_ui_init(void);
/* the start-up animation and its music (any button skips it); the platform calls it once the engine runs */
void vsc_ui_intro(void);
/* Feed one control snapshot. Returns a HOST_* request (and its argument). */
unsigned vsc_ui_update(const struct vsc_ui_input *in, unsigned *arg);
void vsc_ui_notice(const char *text, uint32_t ms);
/* Paint rows y0..y0+VSC_BAND-1 into a VSC_W*VSC_BAND RGB565 buffer. */
void vsc_ui_paint_band(unsigned y0, uint16_t *pixels);
/* Called once before painting a frame's bands. */
void vsc_ui_frame(uint32_t now_ms);

#endif
