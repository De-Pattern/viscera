/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: final output stage. The live inputs (USB and Bluetooth audio) go
 * into the engine as material, so they are not mixed in here; this stage
 * applies the MASTER knob and feeds USB recording. Based on M-VAVE's
 * audio_output.c from FM1 Open V14. */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"

static unsigned output_gain;
void fm1_output_reset(void) { output_gain = 0; }
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    unsigned target = fm1_engine_output_level();   /* the MASTER knob */
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i)
        output[i] = (int32_t)(((int64_t)output[i] * output_gain) >> 7);
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
}
