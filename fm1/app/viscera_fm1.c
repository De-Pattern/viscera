/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track on the FM-1: the bridge between the portable core and M-VAVE's
 * FM1 Open SDK. Audio render and inputs, MIDI in, and project storage. */
#include "fm1.h"
#include "fm1_audio_io.h"
#include "fm1_device.h"
#include "fm1_audio.h"
#include "instrument.h"
#include "viscera/vsc.h"
#include "viscera_fm1.h"

/* the control task's functions: size over speed (flash is short) */
#define CTRL __attribute__((minsize))

/* ------------------------------------------------------------ audio */
static volatile uint8_t ready;
static uint8_t over_count, hard_count; static uint16_t rest_blocks, calm_blocks;
volatile uint8_t vsc_fm1_overload;   /* set while the CPU guard has silenced the engine (its last resort) */
/* The inputs are read raw into the buffers they are then converted in place in, and the
 * engine renders into the output block itself: 2.5 KB less RAM, which the library's
 * start-up needs as heap. may_alias: the same memory is read as one type and written
 * as another. */
typedef float __attribute__((may_alias)) af32;
typedef int32_t __attribute__((may_alias)) ai32;
typedef int16_t __attribute__((may_alias)) ai16;
static float in_bt[VSC_BLOCK * 2], in_usb[VSC_BLOCK * 2];
static uint32_t render_peak_ticks, render_ticks, block_period, block_prev;
/* the audio timer, read directly (fm1_device_ticks lives in flash; nothing the audio
 * interrupt calls may) */
static inline __attribute__((always_inline)) uint32_t ticks_between(uint32_t start, uint32_t end)
{
    return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1u - start;
}

/* the engine's clock for the per-part CPU readout (called from the audio interrupt) */
static FM1_AUDIO_CODE uint32_t clock_now(void) { return FM1_AUDIO_TIMER_COUNT; }
static FM1_AUDIO_CODE uint32_t clock_since(uint32_t since) { return ticks_between(since, FM1_AUDIO_TIMER_COUNT); }

CTRL void vsc_fm1_start(void)
{
    vsc_init();
    vsc_clock_now = clock_now;
    vsc_clock_since = clock_since;
    vsc_project_defaults(&vsc);
    vsc_fm1_device_load();
    ready = 1;
}

/* Called from the audio interrupt in place of the stock engine. Fills one
 * block of signed Q23 stereo. */
FM1_AUDIO_CODE int vsc_fm1_render(int32_t *out)
{
    if (!ready || rest_blocks) {
        if (rest_blocks && !--rest_blocks) vsc_fm1_overload = 0;
        for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) out[i] = 0;
        return 0;
    }
    uint32_t t0 = FM1_AUDIO_TIMER_COUNT;
    if (block_prev) block_period = ticks_between(block_prev, t0);   /* one block, in the timer's ticks */
    block_prev = t0 | 1u;
    struct vsc_inputs in = {0, 0};
    uint16_t gains[2];
    {   /* USB: Q23 in int32, converted where it lies (same size) */
        ai32 *raw = (ai32 *)in_usb; af32 *f = (af32 *)in_usb;
        if (fm1_usb_audio_read((int32_t *)raw, VSC_BLOCK, gains) == VSC_BLOCK) {
            for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) { int32_t v = raw[i]; f[i] = (float)v * (1.f / 8388608.f); }
            in.usb = in_usb;
        }
    }
    {   /* Bluetooth: int16 in the first half, widened from the end down (each read
         * happens before its bytes are overwritten) */
        ai16 *raw = (ai16 *)in_bt; af32 *f = (af32 *)in_bt;
        if (fm1_bluetooth_audio_read((int16_t *)raw, VSC_BLOCK) == VSC_BLOCK) {
            for (unsigned i = VSC_BLOCK * 2; i-- > 0;) { int16_t v = raw[i]; f[i] = (float)v * (1.f / 32768.f); }
            in.bt = in_bt;
        }
    }
    af32 *mix = (af32 *)out;   /* rendered into the output block, converted to Q23 in place */
    vsc_render(&in, (float *)mix);   /* (the engine monitors the input itself) */
    for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) {
        float v = mix[i];
        if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
        ((ai32 *)out)[i] = (int32_t)(v * 8388607.f);
    }
    uint32_t used = ticks_between(t0, FM1_AUDIO_TIMER_COUNT);
    render_ticks = used;
    if (used > render_peak_ticks) render_peak_ticks = used;
    /* CPU guard, in steps as Ravel's: above 75% of a block two blocks running, ease
     * (early: the FM-1 has proved slower than the emulator these shares come from)
     * (relief 1: RESON only its louder bands and no analysis bank; 2: fewer bands still,
     * no droplets). Still over at relief 2 for three such pairs: rest for about a second
     * (the audio interrupt must never starve the USB and control tasks, which carry
     * the update protocol). Below 50% for about 3 s: one step back. (No division
     * here: the compiler's divide helpers live in flash; 32-bit: a block is far
     * below 2^28 ticks.) */
#ifdef VSC_BENCH
    if (0) {
#else
    if (block_period && used < 0x10000000u && used * 4u > block_period * 3u) {
#endif
        calm_blocks = 0;
        if (++over_count >= 2) {
            over_count = 0;
            if (vsc_relief < 2) vsc_relief = (uint8_t)(vsc_relief + 1);
            else if (++hard_count >= 3) { hard_count = 0; rest_blocks = 350; vsc_fm1_overload = 1; }
        }
    } else {
        over_count = 0;
        if (block_period && used * 2u < block_period) {
            hard_count = 0;
            if (vsc_relief && ++calm_blocks >= 1000u) { calm_blocks = 0; vsc_relief = (uint8_t)(vsc_relief - 1); }
        } else calm_blocks = 0;
    }
    return 0;
}
/* Render time as a share of one block (0..100+), and its peak since the last read. */
CTRL unsigned vsc_fm1_cpu(unsigned *peak)
{
    /* (control task) as a share of the measured block period */
    uint32_t period = block_period ? block_period : 1u;
    unsigned now = (unsigned)((uint64_t)render_ticks * 100u / period);
    if (peak) { *peak = (unsigned)((uint64_t)render_peak_ticks * 100u / period); render_peak_ticks = 0; }
    return now;
}

/* (control task) each part's share of a block: the tape and desk, RESON, the rest; then
 * the platform's own (the inputs, the conversion) */
CTRL void vsc_fm1_parts(uint8_t *pct)
{
    uint32_t period = block_period ? block_period : 1u, sum = 0;
    for (unsigned i = 0; i < VSC_PARTS; ++i) {
        uint32_t v = vsc_meter.part_ticks[i];
        sum += v;
        uint32_t p = (uint32_t)((uint64_t)v * 100u / period);
        pct[i] = (uint8_t)(p > 255u ? 255u : p);
    }
    uint32_t rest = render_ticks > sum ? render_ticks - sum : 0;
    uint32_t p = (uint32_t)((uint64_t)rest * 100u / period);
    pct[VSC_PARTS] = (uint8_t)(p > 255u ? 255u : p);
}

/* ------------------------------------------------------- sample bank */
/* samples.c tells us when it binds a bank; the 4-track doesn't play it */
CTRL void vsc_fm1_bank_entry(unsigned slot, const uint8_t *data, uint32_t frames, const char *name)
{
    (void)slot; (void)data; (void)frames; (void)name;
}

/* ---------------------------------------------------------------- MIDI */
/* Routing task: queue notes and clock for the audio context. Channels 1-4
 * play tracks 1-4. */
CTRL void vsc_fm1_midi(unsigned source, const uint8_t *m, size_t n)
{
    vsc_midi_in(source, m, (unsigned)n, fm1_millis());
}

/* ------------------------------------------------------------ storage */
/* Eight 8 KB project slots in the top 64 KB of the sample-bank area, below
 * the DX7 library sectors. samples.c refuses banks that would reach them. */
#define SLOT_BYTES 8192u
static uint32_t slot_offset(unsigned slot) { return VSC_STORE_OFFSET + slot * SLOT_BYTES; }
_Static_assert(sizeof(struct vsc_project) <= SLOT_BYTES, "a project must fit its slot");

CTRL int vsc_fm1_storage_ok(void)
{
    if (fm1_flash_size() < GROOVE_FLASH_BYTES) return 0;
    /* A bank written by the stock firmware may run into our area. */
    uint32_t header[3];
    if (fm1_flash_read(GROOVE_SAMPLE_OFFSET, header, sizeof header)) return 0;
    if (header[0] == 0x53314d46u && header[2] > VSC_SAMPLE_LIMIT) return 0;
    return 1;
}
/* this format, or format 10 (2.4's: no LFO matrix), which loads with an empty one */
static CTRL int head_ok(const uint32_t *head)
{
    return head[0] == VSC_MAGIC && ((head[1] == VSC_FORMAT && head[2] == sizeof(struct vsc_project)) ||
                                    (head[1] == 10u && head[2] == VSC_FORMAT_10_BYTES) ||
                                    (head[1] == 11u && head[2] == VSC_FORMAT_11_BYTES) || (head[1] == 12u && head[2] == VSC_FORMAT_12_BYTES) ||
                                    (head[1] == 13u && head[2] == VSC_FORMAT_13_BYTES));
}
CTRL int vsc_fm1_slot_info(unsigned slot, char name[12])
{
    uint32_t head[4];
    if (fm1_flash_read(slot_offset(slot), head, sizeof head)) return 0;
    /* this format only: 0.x projects were made for the old engine */
    if (!head_ok(head)) return 0;
    if (fm1_flash_read(slot_offset(slot) + 16u, name, 12)) return 0;
    return 1;
}
/* Saving writes flash, which silences audio briefly: only while stopped.
 * No staging copy: RAM is short, so the live project is stamped and written. */
CTRL int vsc_fm1_save(unsigned slot)
{
    if (slot >= VSC_PROJECTS || !vsc_fm1_storage_ok()) return -1;
    if (vsc_meter.playing) return -2;
    vsc.magic = VSC_MAGIC; vsc.version = VSC_FORMAT; vsc.bytes = sizeof vsc;
    char name[12] = "PROJECT  ";
    name[8] = (char)('1' + slot); name[9] = 0;
    vsc_memcpy(vsc.name, name, 12);
    vsc.crc = vsc_crc32(&vsc.name, sizeof vsc - offsetof(struct vsc_project, name));
    uint32_t base = slot_offset(slot);
    for (uint32_t o = 0; o < SLOT_BYTES; o += FM1_FLASH_SECTOR_BYTES)
        if (fm1_flash_erase(base + o)) return -3;
    if (fm1_flash_write(base, &vsc, sizeof vsc)) return -3;
    if (vsc_dev.last_slot != slot + 1u) { vsc_dev.last_slot = (uint8_t)(slot + 1u); ++vsc_dev_changed; }   /* START WITH LAST */
    return 0;
}
/* Loading checks the stored project's CRC straight from flash, then copies it. */
CTRL int vsc_fm1_load(unsigned slot)
{
    if (slot >= VSC_PROJECTS) return -1;
    uint32_t base = slot_offset(slot), head[4];
    if (fm1_flash_read(base, head, sizeof head)) return -3;
    /* (1.0's projects, format 6, load in the emulator only: flash for the FM-1's CPU guards) */
    if (!head_ok(head)) return -4;
    uint8_t buf[256];
    uint32_t c = 0xffffffffu, at = offsetof(struct vsc_project, name), end = head[2];
    while (at < end) {
        uint32_t n = end - at < sizeof buf ? end - at : sizeof buf;
        if (fm1_flash_read(base + at, buf, n)) return -3;
        for (uint32_t i = 0; i < n; ++i) {
            c ^= buf[i];
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
        at += n;
    }
    if (~c != head[3]) return -4;
    vsc_event(EV_STOP, 0, 0);
    vsc_event(EV_PANIC, 0, 0);
    if (fm1_flash_read(base, &vsc, end)) return -3;
    if (head[1] != VSC_FORMAT && !vsc_project_upgrade(&vsc)) { vsc_project_defaults(&vsc); return -4; }
    vsc_learn_release();
    if (vsc_dev.last_slot != slot + 1u) { vsc_dev.last_slot = (uint8_t)(slot + 1u); ++vsc_dev_changed; }   /* START WITH LAST */
    return 0;
}

/* The FM-1's own settings, in the sector after the eight project slots. */
#define DEVICE_OFFSET (VSC_STORE_OFFSET + 8u * SLOT_BYTES)
#define BOOT_AT (DEVICE_OFFSET + FM1_FLASH_SECTOR_BYTES - 16u)   /* the start guard: the sector's last 16 bytes */
_Static_assert(sizeof(struct vsc_device) <= FM1_FLASH_SECTOR_BYTES - 16u, "device settings fit one sector, before the start guard");
_Static_assert(8u * SLOT_BYTES + FM1_FLASH_SECTOR_BYTES <= VSC_STORE_BYTES, "the settings sector is inside the store");
CTRL void vsc_fm1_device_load(void)
{
    if (!vsc_fm1_storage_ok() || fm1_flash_read(DEVICE_OFFSET, &vsc_dev, sizeof vsc_dev) ||
        (!vsc_device_valid(&vsc_dev) && !vsc_device_upgrade(&vsc_dev)))   /* (2.x's settings carry over) */
        vsc_device_defaults();
}
/* The start guard, in the sector's last 16 bytes: a stamp of this version, then one
 * byte a start, programmed without an erase (flash bits only go from 1 to 0): 0x00 when
 * it starts, 0x5A over the next free byte once it has run well. The count is the
 * starts since the last good one. Another version's stamp (a new install), anything
 * unexpected or a full area starts afresh (an erase that rewrites the settings, before
 * the audio starts). Returns that count before this start, or -1 if the flash can't be
 * used (then there is no guard). */
static unsigned boot_pos = 16u;
CTRL int vsc_fm1_boot_begin(void)
{
    uint8_t b[16], st = 0;
    if (!vsc_fm1_storage_ok() || fm1_flash_read(BOOT_AT, b, sizeof b)) return -1;
    for (const char *v = VSC_VERSION; *v; ++v) st = (uint8_t)(st * 31u + (uint8_t)*v);
    st |= 0x80u; if (st == 0xffu) st = 0xfeu;   /* never a start, a good mark or erased */
    unsigned n = 1, tries = 0, ok = b[0] == st;
    for (; ok && n < sizeof b && b[n] != 0xffu; ++n) { if (!b[n]) ++tries; else if (b[n] == 0x5au) tries = 0; else ok = 0; }
    for (unsigned i = n; i < sizeof b; ++i) if (b[i] != 0xffu) ok = 0;
    if (!ok || n >= sizeof b) {
        if (vsc_fm1_device_save() || fm1_flash_write(BOOT_AT, &st, 1)) return -1;
        n = 1; if (!ok) tries = 0;
    }
    uint8_t z = 0;
    (void)fm1_flash_write(BOOT_AT + n, &z, 1);
    boot_pos = n + 1u;
    return (int)tries;
}
/* a good start: one byte, no erase */
int vsc_fm1_boot_good(void)
{
    uint8_t g = 0x5au;
    return boot_pos < 16u ? fm1_flash_write(BOOT_AT + boot_pos, &g, 1) : 0;
}
CTRL int vsc_fm1_device_save(void)
{
    if (!vsc_fm1_storage_ok()) return -1;
    vsc_device_seal();
    if (fm1_flash_erase(DEVICE_OFFSET)) return -3;
    if (fm1_flash_write(DEVICE_OFFSET, &vsc_dev, sizeof vsc_dev)) return -3;
    return 0;
}
