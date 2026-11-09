/* Freestanding shim for the browser build of the SEA codec (see sea_wasm.c). */
#pragma once
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
