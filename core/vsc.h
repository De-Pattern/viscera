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
    /* MATERIAL */
    P_MODE, P_SLOT, P_START, P_LEN, P_SPEED, P_PITCH,
    P_LOOPM, P_INPUT, P_DUB, P_SIZE, P_QUAL, P_GAIN,
    P_ATK, P_DEC, P_SUS, P_REL, P_GLIDE, P_VELO,
    /* GRAIN */
    P_GMIX, P_GSIZE, P_GDENS, P_GPITCH, P_GSPRAY, P_GSHAPE,
    P_GTIME, P_GREV, P_GFREEZE, P_GDETUNE, P_GQUANT, P_GFDBK,
    /* FILTER */
    P_FTYPE, P_FCUT, P_FRES, P_FSPREAD, P_FSCALE, P_FMIX,
    /* COLOR */
    P_DRIVE, P_COMP, P_BITS, P_RATE, P_NOISE, P_TONE,
    P_NTYPE, P_CMIX, P_CRES1, P_CRES2, P_CRES3, P_CRES4,
    /* MIX */
    P_LEVEL, P_PAN, P_SEND, P_MUTE, P_MRES1, P_MRES2,
    /* MODS: four parameters for each of the four modulators */
    P_M0A, P_M0B, P_M0C, P_M0D,
    P_M1A, P_M1B, P_M1C, P_M1D,
    P_M2A, P_M2B, P_M2C, P_M2D,
    P_M3A, P_M3B, P_M3C, P_M3D,
    /* SEQUENCER: STEPS or REBOUND, and REBOUND's physics */
    P_SEQM, P_RSPEED, P_RGRAV, P_RLOSS, P_RSPREAD, P_RQUANT,
    P_RCOUNT, P_RWALLS, P_RJITTER, P_RGATE,
    VSC_NP
};
enum vsc_gparam {
    G_SWING, G_DTIME, G_DFEED, G_DTONE, G_DLONG, G_DLEVEL,
    G_RSIZE, G_RDAMP, G_RLEVEL, G_MAIN, G_CLOCK, G_INMON,
    G_MORPH, G_TARGET, G_MSEL, G_M0, G_M1, G_M2, G_M3,
    VSC_NG
};
enum { MODE_PLAY, MODE_LOOP, MODE_KEYS, MODE_THRU };
enum { IN_BT, IN_USB, IN_BOTH, IN_T1, IN_T2, IN_T3, IN_T4, IN_MAIN, IN_COUNT };
enum { FT_LP, FT_BP, FT_HP, FT_PEAKS, FT_OFF };
enum { MOD_LFO, MOD_RANDOM, MOD_FOLLOW, MOD_ENV };
enum { SEQ_STEPS, SEQ_REBOUND };
#define VSC_BALLS 8

#define VSC_PF_ENUM    1
#define VSC_PF_BIPOLAR 2
#define VSC_PF_GLOBAL  4
struct vsc_pinfo {
    char name[7];
    uint8_t max, def, flags;
    const char *const *names;  /* enum labels, or NULL */
};
const struct vsc_pinfo *vsc_tparam_info(unsigned track, unsigned p);
const struct vsc_pinfo *vsc_gparam_info(unsigned p);
/* Writes a short value label (up to 7 chars) into out. */
void vsc_param_label(const struct vsc_pinfo *info, unsigned p, unsigned value, char out[8], int global);

/* ---- Project data (saved and recalled as one block) -------------------- */
struct vsc_step { uint8_t on, note, vel, lp[2], lv[2], pad; };
struct vsc_route { uint8_t src, param; int8_t depth; uint8_t pad; };
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
       EV_PANIC, EV_LOOP_CLEAR, EV_BALL, EV_BALL_CLEAR };
void vsc_event(unsigned type, unsigned track, unsigned a, unsigned b);
/* Same, from the MIDI routing task (its own queue). */
void vsc_midi_event(unsigned type, unsigned track, unsigned a, unsigned b);

/* ---- State shared with the UI ----------------------------------------- */
struct vsc_meter {
    float peak[VSC_TRACKS], main_peak, cpu;
    uint32_t pos_play[VSC_TRACKS];   /* material read position in source frames */
    uint32_t pos_len[VSC_TRACKS];    /* source length in frames */
    uint32_t grain_write[VSC_TRACKS];
    uint16_t grain_pos[VSC_TRACKS][VSC_GRAINS]; /* 0..65535 position in ring, 0xffff idle */
    uint8_t grain_amp[VSC_TRACKS][VSC_GRAINS];
    float mod_value[VSC_TRACKS][VSC_MODS];
    uint8_t loop_state[VSC_TRACKS];  /* 0 empty, 1 recording, 2 playing, 3 overdub */
    uint8_t step[VSC_TRACKS];
    /* REBOUND: ball positions 0..255, a flash that fades after each wall hit */
    uint8_t ball_x[VSC_TRACKS][VSC_BALLS], ball_y[VSC_TRACKS][VSC_BALLS], ball_hit[VSC_TRACKS][VSC_BALLS];
    uint8_t ball_on[VSC_TRACKS][VSC_BALLS];
    uint8_t playing, ext_clock, bt_in, usb_in;
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
/* Sets or clears a modulation route; depth -127..127, 0 removes it. */
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
