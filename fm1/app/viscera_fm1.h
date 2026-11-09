/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef VISCERA_FM1_H
#define VISCERA_FM1_H
#include <stdint.h>
#include <stddef.h>
#include "instrument.h"

/* Project storage: the top 68 KB of the sample-bank area: eight 8 KB project
 * slots, then one 4 KB sector for the FM-1's own settings. */
#define VSC_STORE_BYTES  69632u
#define VSC_STORE_OFFSET ((uint32_t)GROOVE_LIBRARY_OFFSET - VSC_STORE_BYTES)
#define VSC_SAMPLE_LIMIT ((uint32_t)GROOVE_SAMPLE_LIMIT - VSC_STORE_BYTES)

void vsc_fm1_start(void);
extern volatile uint8_t vsc_fm1_overload;
int vsc_fm1_render(int32_t *out);                 /* audio interrupt */
unsigned vsc_fm1_cpu(unsigned *peak);
void vsc_fm1_bank_entry(unsigned slot, const uint8_t *data, uint32_t frames, const char *name);
void vsc_fm1_midi(unsigned source, const uint8_t *m, size_t n);
int vsc_fm1_storage_ok(void);
int vsc_fm1_slot_info(unsigned slot, char name[12]);
int vsc_fm1_save(unsigned slot);
int vsc_fm1_load(unsigned slot);
/* The FM-1's own settings: loaded at start, written when changed (control task). */
void vsc_fm1_device_load(void);
int vsc_fm1_device_save(void);
/* The start guard: starts since the last good one (-1: no flash), and marking a good one. */
int vsc_fm1_boot_begin(void);
int vsc_fm1_boot_good(void);
#endif
