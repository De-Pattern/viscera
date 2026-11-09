/* Freestanding shim for the browser build of the SEA codec (see sea_wasm.c). */
#pragma once
#include <stddef.h>
void *calloc(size_t n, size_t size);
void free(void *p);
