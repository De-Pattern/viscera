/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: compile-time sizes. Every buffer here lives in internal RAM. */
#ifndef VSC_CONFIG_H
#define VSC_CONFIG_H

#define VSC_VERSION "0.1.0"

#define VSC_TRACKS        4
#define VSC_SR            44100
#define VSC_BLOCK         128      /* frames per audio callback (SDK fixed) */
#define VSC_SUB           32       /* control-rate sub-block */

/* Buffers (bytes or frames). Grow these once the hardware link map shows
 * how much heap is left for Bluetooth. */
#define VSC_GRAIN_FRAMES  12288    /* per track, 8-bit mu-law */
#define VSC_GRAINS        8        /* grains per track */
#define VSC_LOOP_SLICE    16384    /* bytes per loop slice, 8-bit mu-law */
#define VSC_LOOP_SLICES   4
#define VSC_DELAY_FRAMES  12288    /* int16 mono */
#define VSC_KEYS_VOICES   4

#define VSC_STEPS         64
#define VSC_SCENES        8
#define VSC_MACROS        4
#define VSC_MACRO_ROUTES  8
#define VSC_MOD_ROUTES    12
#define VSC_MODS          4
#define VSC_PROJECTS      8

/* Flash sample bank (M-VAVE workbench format). */
#define VSC_BANK_SLOTS    16
#define VSC_BANK_RATE     32000
#define VSC_CHUNK         320      /* frames per SEA chunk */
#define VSC_CACHE         8        /* decoded chunks kept in RAM */

/* Placement. On the FM-1 every function and table the audio interrupt
 * touches must sit in RAM, because flash programming stalls code that runs
 * from flash. VSC_HOT marks functions, VSC_HOTDATA initialised tables,
 * VSC_INL helpers that must always inline into a hot caller. */
#if defined(__GCC_PI32V2__)
#define VSC_FM1 1
#define VSC_HOT     __attribute__((section(".volatile_ram_code"), noinline, used))
#define VSC_HOTDATA __attribute__((section(".volatile_ram.fm1"), aligned(4)))
#else
#define VSC_HOT
#define VSC_HOTDATA
#endif
#define VSC_INL static inline __attribute__((always_inline))

#endif
