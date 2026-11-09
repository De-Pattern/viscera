/* SPDX-License-Identifier: MIT
 * The FM-1 sample codec for the install page's sample manager: SEA5 encode
 * (M-VAVE's fixed-format encoder from the FM1 Open SDK, after Dani Biro's SEA)
 * and decode, compiled to WebAssembly with no libc. One job at a time: the
 * page writes PCM or a sample's chunks into the buffer this hands out, then
 * encodes or decodes, and copies the result out before the next call. */
#include <stdint.h>
#include <stddef.h>
#include "shim/stdlib.h"
#include "shim/string.h"

#define W_EXPORT(name) __attribute__((export_name(#name)))

/* a bump heap over the module's memory, reset at the start of every job */
extern unsigned char __heap_base;
static size_t heap_top;
static void heap_reset(void) { heap_top = ((size_t)&__heap_base + 15u) & ~(size_t)15u; }
static void *take(size_t bytes)
{
    size_t need = heap_top + ((bytes + 15u) & ~(size_t)15u);
    size_t have = __builtin_wasm_memory_size(0) * 65536u;
    if (need > have && __builtin_wasm_memory_grow(0, (need - have + 65535u) / 65536u) == (size_t)-1) return NULL;
    void *p = (void *)heap_top; heap_top = need; return p;
}
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
void *calloc(size_t n, size_t size) { void *p = take(n * size); if (p) memset(p, 0, n * size); return p; }
void free(void *p) { (void)p; }

#include "fm1_sea.h"
#include "sea_dqt.inc"
#include "sea_encode.h"
#include "sea_decode.c"

#define CHUNK_BYTES 228u   /* 5 bits: 28 + 320 * 5 / 8 */
static void *input, *output;
static unsigned job_frames, out_bytes;

/* room for `frames` of 16-bit mono PCM (to encode) */
W_EXPORT(pcm_buffer) int16_t *w_pcm_buffer(unsigned frames)
{
    heap_reset(); job_frames = frames; out_bytes = 0;
    return input = take((size_t)((frames + 319u) / 320u) * 640u);
}
/* encode the PCM buffer; returns the SEA5 chunks (sea_bytes() long) */
W_EXPORT(encode) uint8_t *w_encode(void)
{
    if (!input || !job_frames) return NULL;
    output = fm1_sea_encode(input, job_frames, sample_dqt);
    out_bytes = output ? (job_frames + 319u) / 320u * CHUNK_BYTES : 0;
    return output;
}
W_EXPORT(sea_bytes) unsigned w_sea_bytes(void) { return out_bytes; }
/* room for a sample's chunks (to decode) */
W_EXPORT(sea_buffer) uint8_t *w_sea_buffer(unsigned frames)
{
    heap_reset(); job_frames = frames;
    return input = take((size_t)((frames + 319u) / 320u) * CHUNK_BYTES);
}
/* decode the chunks into PCM (frames long); NULL if a chunk is damaged */
W_EXPORT(decode) int16_t *w_decode(void)
{
    unsigned chunks = (job_frames + 319u) / 320u;
    int16_t *pcm = take((size_t)chunks * 640u);
    if (!pcm || !input) return NULL;
    for (unsigned c = 0; c < chunks; ++c)
        if (fm1_sea_decode((const uint8_t *)input + c * CHUNK_BYTES, CHUNK_BYTES, 5, sample_dqt, pcm + c * 320u)) return NULL;
    return pcm;
}
