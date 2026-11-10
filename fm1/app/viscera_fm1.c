/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera on the FM-1: the bridge between the portable core and M-VAVE's
 * FM1 Open SDK. Audio render and inputs, the flash sample bank (SEA5 chunks
 * decoded into a small RAM cache), MIDI in, and project storage. */
#include "fm1.h"
#include "fm1_audio_io.h"
#include "fm1_device.h"
#include "fm1_sea.h"
#include "fm1_audio.h"
#include "instrument.h"
#include "viscera/vsc.h"
#include "viscera_fm1.h"

#include "vsc_sea_table.inc"

/* ------------------------------------------------------------ audio */
static volatile uint8_t ready;
static uint8_t over_count, hard_count; static uint16_t rest_blocks, calm_blocks;
volatile uint8_t vsc_fm1_overload;   /* set while the CPU guard has silenced the engine (its last resort) */
static float in_bt[VSC_BLOCK * 2], in_usb[VSC_BLOCK * 2], mix[VSC_BLOCK * 2];
static int32_t usb_raw[VSC_BLOCK * 2];
static int16_t bt_raw[VSC_BLOCK * 2];
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

void vsc_fm1_start(void)
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
    if (fm1_usb_audio_read(usb_raw, VSC_BLOCK, gains) == VSC_BLOCK) {
        for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) in_usb[i] = (float)usb_raw[i] * (1.f / 8388608.f);
        in.usb = in_usb;
    }
    if (fm1_bluetooth_audio_read(bt_raw, VSC_BLOCK) == VSC_BLOCK) {
        for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) in_bt[i] = (float)bt_raw[i] * (1.f / 32768.f);
        in.bt = in_bt;
    }
    vsc_render(&in, mix);
    int mon = vsc.g[G_INMON];
    for (unsigned i = 0; i < VSC_BLOCK * 2; ++i) {
        float v = mix[i];
        if (mon) { if (in.bt) v += in_bt[i]; if (in.usb) v += in_usb[i]; }
        if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
        out[i] = (int32_t)(v * 8388607.f);
    }
    uint32_t used = ticks_between(t0, FM1_AUDIO_TIMER_COUNT);
    render_ticks = used;
    if (used > render_peak_ticks) render_peak_ticks = used;
    /* CPU guard, in steps as Ravel's: above 90% of a block two blocks running, ease
     * (relief 1: SPACE's reverb rests; 2: also half of RESON's bands and of the new
     * grains). Still over at relief 2 for three such pairs: rest for about a second
     * (the audio interrupt must never starve the USB and control tasks, which carry
     * the update protocol). Below 60% for about 3 s: one step back. (No division
     * here: the compiler's divide helpers live in flash; 32-bit: a block is far
     * below 2^28 ticks.) */
    if (block_period && used < 0x10000000u && used * 10u > block_period * 9u) {
        calm_blocks = 0;
        if (++over_count >= 2) {
            over_count = 0;
            if (vsc_relief < 2) vsc_relief = (uint8_t)(vsc_relief + 1);
            else if (++hard_count >= 3) { hard_count = 0; rest_blocks = 350; vsc_fm1_overload = 1; }
        }
    } else {
        over_count = 0;
        if (block_period && used * 10u < block_period * 6u) {
            hard_count = 0;
            if (vsc_relief && ++calm_blocks >= 1000u) { calm_blocks = 0; vsc_relief = (uint8_t)(vsc_relief - 1); }
        } else calm_blocks = 0;
    }
    return 0;
}
/* Render time as a share of one block (0..100+), and its peak since the last read. */
unsigned vsc_fm1_cpu(unsigned *peak)
{
    /* (control task) as a share of the measured block period */
    uint32_t period = block_period ? block_period : 1u;
    unsigned now = (unsigned)((uint64_t)render_ticks * 100u / period);
    if (peak) { *peak = (unsigned)((uint64_t)render_peak_ticks * 100u / period); render_peak_ticks = 0; }
    return now;
}

/* (control task) each part's share of a block: T1..T4, SPACE, then the rest (inputs,
 * sequencer, master) */
void vsc_fm1_parts(uint8_t *pct)
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
struct bank_entry { const uint8_t *data; uint32_t frames; char name[16]; };
static struct bank_entry bank[VSC_BANK_SLOTS];
struct cache_line { int16_t pcm[FM1_SEA_FRAMES]; int16_t slot; uint16_t used; uint32_t chunk; };
static struct cache_line cache[VSC_CACHE];
static uint16_t cache_clock;
static volatile uint8_t bank_ok;

/* samples.c calls this after it binds a verified bank (data mapped from
 * flash), and with data NULL when the bank goes away. */
void vsc_fm1_bank_entry(unsigned slot, const uint8_t *data, uint32_t frames, const char *name)
{
    if (slot >= VSC_BANK_SLOTS) return;
    bank_ok = 0;
    bank[slot].data = frames ? data : 0;
    bank[slot].frames = data ? frames : 0;
    for (unsigned i = 0; i < 15; ++i) bank[slot].name[i] = name && data ? name[i] : 0;
    bank[slot].name[15] = 0;
    for (unsigned i = 0; i < VSC_CACHE; ++i) cache[i].slot = -1;
    /* a new bank: MATERIAL redraws each sample's shape */
    for (unsigned t = 0; t < VSC_TRACKS; ++t) vsc_meter.overview_slot[t] = 255;
    bank_ok = 1;
}
FM1_AUDIO_CODE unsigned vsc_bank_frames(unsigned slot) { return slot < VSC_BANK_SLOTS && bank_ok ? bank[slot].frames : 0; }
const char *vsc_bank_name(unsigned slot) { return slot < VSC_BANK_SLOTS ? bank[slot].name : ""; }
FM1_AUDIO_CODE const int16_t *vsc_bank_chunk(unsigned slot, unsigned chunk)
{
    if (!bank_ok || slot >= VSC_BANK_SLOTS || !bank[slot].data) return 0;
    if ((uint32_t)chunk * FM1_SEA_FRAMES >= bank[slot].frames) return 0;
    unsigned victim = 0, oldest = 0;
    ++cache_clock;
    for (unsigned i = 0; i < VSC_CACHE; ++i) {
        if (cache[i].slot == (int16_t)slot && cache[i].chunk == chunk) { cache[i].used = cache_clock; return cache[i].pcm; }
        /* empty lines first, then the least recently used */
        unsigned age = cache[i].slot < 0 ? 0x10000u : (uint16_t)(cache_clock - cache[i].used);
        if (age > oldest) { oldest = age; victim = i; }
    }
    struct cache_line *c = &cache[victim];
    const uint8_t *src = bank[slot].data + (uint32_t)chunk * 228u;   /* SEA5: 228 bytes per 320 frames */
    if (fm1_sea_decode(src, 228u, 5u, vsc_sea_dqt, c->pcm) != 0) return 0;
    c->slot = (int16_t)slot; c->chunk = chunk; c->used = cache_clock;
    return c->pcm;
}

/* ---------------------------------------------------------------- MIDI */
/* Routing task: queue notes and clock for the audio context. Channels 1-4
 * play tracks 1-4. */
void vsc_fm1_midi(unsigned source, const uint8_t *m, size_t n)
{
    vsc_midi_in(source, m, (unsigned)n, fm1_millis());
}

/* ------------------------------------------------------------ storage */
/* Eight 8 KB project slots in the top 64 KB of the sample-bank area, below
 * the DX7 library sectors. samples.c refuses banks that would reach them. */
#define SLOT_BYTES 8192u
static uint32_t slot_offset(unsigned slot) { return VSC_STORE_OFFSET + slot * SLOT_BYTES; }
_Static_assert(sizeof(struct vsc_project) <= SLOT_BYTES, "a project must fit its slot");

int vsc_fm1_storage_ok(void)
{
    if (fm1_flash_size() < GROOVE_FLASH_BYTES) return 0;
    /* A bank written by the stock firmware may run into our area. */
    uint32_t header[3];
    if (fm1_flash_read(GROOVE_SAMPLE_OFFSET, header, sizeof header)) return 0;
    if (header[0] == 0x53314d46u && header[2] > VSC_SAMPLE_LIMIT) return 0;
    return 1;
}
int vsc_fm1_slot_info(unsigned slot, char name[12])
{
    uint32_t head[4];
    if (fm1_flash_read(slot_offset(slot), head, sizeof head)) return 0;
    if (head[0] != VSC_MAGIC || head[2] != sizeof(struct vsc_project)) return 0;
    if (fm1_flash_read(slot_offset(slot) + 16u, name, 12)) return 0;
    return 1;
}
/* Saving writes flash, which silences audio briefly: only while stopped.
 * No staging copy: RAM is short, so the live project is stamped and written. */
int vsc_fm1_save(unsigned slot)
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
    return 0;
}
/* Loading checks the stored project's CRC straight from flash, then copies it. */
int vsc_fm1_load(unsigned slot)
{
    if (slot >= VSC_PROJECTS) return -1;
    uint32_t base = slot_offset(slot), head[4];
    if (fm1_flash_read(base, head, sizeof head)) return -3;
    if (head[0] != VSC_MAGIC || head[1] != VSC_FORMAT || head[2] != sizeof(struct vsc_project)) return -4;
    uint8_t buf[256];
    uint32_t c = 0xffffffffu, at = offsetof(struct vsc_project, name), end = sizeof(struct vsc_project);
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
    vsc_event(EV_STOP, 0, 0, 0);
    vsc_event(EV_PANIC, 0, 0, 0);
    if (fm1_flash_read(base, &vsc, sizeof vsc)) return -3;
    vsc_learn_release();
    return 0;
}

/* The FM-1's own settings, in the sector after the eight project slots. */
#define DEVICE_OFFSET (VSC_STORE_OFFSET + 8u * SLOT_BYTES)
#define BOOT_AT (DEVICE_OFFSET + FM1_FLASH_SECTOR_BYTES - 16u)   /* the start guard: the sector's last 16 bytes */
_Static_assert(sizeof(struct vsc_device) <= FM1_FLASH_SECTOR_BYTES - 16u, "device settings fit one sector, before the start guard");
_Static_assert(8u * SLOT_BYTES + FM1_FLASH_SECTOR_BYTES <= VSC_STORE_BYTES, "the settings sector is inside the store");
void vsc_fm1_device_load(void)
{
    if (!vsc_fm1_storage_ok() || fm1_flash_read(DEVICE_OFFSET, &vsc_dev, sizeof vsc_dev) || !vsc_device_valid(&vsc_dev))
        vsc_device_defaults();
}
/* The start guard. Each start programs one more byte of a 16-byte erased area to 0
 * (no erase needed: flash bits only go from 1 to 0); a start that runs well for a
 * few seconds erases the sector again (rewriting the settings). So the count is the
 * starts since the last good one. Returns that count before this start, or -1 if the
 * flash can't be used (then there is no guard). */
int vsc_fm1_boot_begin(void)
{
    uint8_t b[16];
    if (!vsc_fm1_storage_ok() || fm1_flash_read(BOOT_AT, b, sizeof b)) return -1;
    /* ours looks like zeros then 0xff; anything else (old sample data, a first start
     * after installing) isn't a count: start the sector afresh and count from 0 */
    unsigned tries = 0;
    while (tries < sizeof b && b[tries] == 0u) ++tries;
    for (unsigned i = tries; i < sizeof b; ++i)
        if (b[i] != 0xffu) { if (vsc_fm1_device_save()) return -1; tries = 0; break; }
    if (tries < sizeof b) { uint8_t z = 0; (void)fm1_flash_write(BOOT_AT + tries, &z, 1); }
    return (int)tries;
}
/* a good start: clear the count (erases the sector and writes the settings back) */
int vsc_fm1_boot_good(void) { return vsc_fm1_device_save(); }
int vsc_fm1_device_save(void)
{
    if (!vsc_fm1_storage_ok()) return -1;
    vsc_device_seal();
    if (fm1_flash_erase(DEVICE_OFFSET)) return -3;
    if (fm1_flash_write(DEVICE_OFFSET, &vsc_dev, sizeof vsc_dev)) return -3;
    return 0;
}
