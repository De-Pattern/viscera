/* SEA5 chunk constants and decoder declarations, as in the FM1 SDK's fm1_sea.h:
 * 320 frames per chunk, 20 frames per scale group. */
#ifndef FM1_SEA_H
#define FM1_SEA_H
#include <stdint.h>
#include <stddef.h>
#define FM1_SEA_FRAMES 320u
#define FM1_SEA_GROUP 20u
int fm1_sea_decode(const uint8_t *chunk, size_t bytes, unsigned bits,
    const int16_t *dqt, int16_t pcm[FM1_SEA_FRAMES]);
int fm1_sea_decode_groups(const uint8_t *chunk, size_t bytes, unsigned bits,
    const int16_t *dqt, unsigned first, unsigned last, int32_t state[8],
    int16_t pcm[FM1_SEA_GROUP]);
#endif
