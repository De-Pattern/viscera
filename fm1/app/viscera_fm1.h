/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef VISCERA_FM1_H
#define VISCERA_FM1_H
#include <stdint.h>
#include <stddef.h>
#include "instrument.h"

/* Project storage: the top 64 KB of the sample-bank area. */
#define VSC_STORE_BYTES  65536u
#define VSC_STORE_OFFSET ((uint32_t)GROOVE_LIBRARY_OFFSET - VSC_STORE_BYTES)
#define VSC_SAMPLE_LIMIT ((uint32_t)GROOVE_SAMPLE_LIMIT - VSC_STORE_BYTES)

void vsc_fm1_start(void);
int vsc_fm1_render(int32_t *out);                 /* audio interrupt */
unsigned vsc_fm1_cpu(unsigned *peak);
void vsc_fm1_bank_entry(unsigned slot, const uint8_t *data, uint32_t frames, const char *name);
void vsc_fm1_midi(unsigned source, const uint8_t *m, size_t n);
int vsc_fm1_storage_ok(void);
int vsc_fm1_slot_info(unsigned slot, char name[12]);
int vsc_fm1_save(unsigned slot);
int vsc_fm1_load(unsigned slot);
#endif
