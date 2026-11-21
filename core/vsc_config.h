/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: compile-time sizes. Every buffer here lives in internal RAM. */
#ifndef VSC_CONFIG_H
#define VSC_CONFIG_H

#ifndef VSC_VERSION
#define VSC_VERSION "dev"
#endif

#define VSC_TRACKS        4
#define VSC_SR            44100
#define VSC_BLOCK         128      /* frames per audio callback (SDK fixed) */
#define VSC_SUB           32       /* control-rate sub-block */

/* The tape: four tracks of 4-bit ADPCM in RAM, VSC_TAPE_BLOCKS blocks of 128 samples
 * each (64 bytes, plus a 3-byte header). The buffers come out of the heap M-VAVE's
 * library starts USB, Bluetooth and MIDI from: with too little the FM-1 never starts.
 * tools/build_fm1.py checks the heap. */
#ifndef VSC_TAPE_BLOCKS
#define VSC_TAPE_BLOCKS   412      /* per track: 52,736 samples (2.39 s HI, 3.3 s MID, 4.78 s LO); 480 before the delay, the verb, SPLICE and TAPE took their memory */
#endif
#define VSC_TAPE_BLK      128      /* samples per ADPCM block */

#define VSC_PROJECTS      8
#define VSC_ROBOTS        2
#define VSC_GEST          48       /* points of a recorded gesture */
#define VSC_LEARN         16
#define VSC_MOTIONS       4        /* motion lanes */
#define VSC_ROUTES        8        /* the LFO matrix: robot-to-knob routes, past each robot's own target */
#define VSC_MOTION_PTS    64       /* points round the loop */

#define VSC_SHIFT_RING    1024     /* each track's pitch shifter's memory (23 ms) */

/* Placement. On the FM-1 every function and table the audio interrupt
 * touches must sit in RAM, because flash programming stalls code that runs
 * from flash. VSC_HOT marks functions, VSC_HOTDATA initialised tables,
 * VSC_INL helpers that must always inline into a hot caller. */
#if defined(__GCC_PI32V2__)
#define VSC_FM1 1
#define VSC_HOT     __attribute__((section(".volatile_ram_code"), noinline, used))
#define VSC_HOT_SMALL __attribute__((section(".volatile_ram_code"), noinline, used, minsize))
#define VSC_HOTDATA __attribute__((section(".volatile_ram.fm1"), aligned(4)))
#else
#define VSC_HOT
#define VSC_HOT_SMALL
#define VSC_HOTDATA
#endif
#define VSC_INL static inline __attribute__((always_inline))

#endif
