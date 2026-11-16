/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera 4-Track: MIDI input from every port (the FM-1's MIDI jack, USB and Bluetooth
 * MIDI). CCs go to learned mappings (or CC 110-113 to the knobs on screen); notes play
 * RESON or trigger learned actions; Start and Stop run the transport. */
#include "vsc.h"

void vsc_midi_in(unsigned port, const uint8_t *m, unsigned n, uint32_t now_ms)
{
    (void)port; (void)now_ms;
    if (!m || !n) return;
    uint8_t st = m[0];
    if (st == 0xfa || st == 0xfb) { vsc_midi_event(EV_PLAY, 1, 0); return; }
    if (st == 0xfc) { vsc_midi_event(EV_STOP, 0, 0); return; }
    if (st >= 0xf0) return;
    unsigned ch = st & 15u, type = st & 0xf0u;
    if (n < 3) return;
    unsigned a = m[1] & 127u, b = m[2] & 127u;
    if (type == 0xb0) {
        /* CC 110-113 turn KNOB 1-4 of the page on screen, on any channel (unless learned) */
        if (a >= 110 && a <= 113 && vsc_dev.knob_cc && !vsc_learn_armed) {
            int learned = 0;
            for (unsigned l = 0; l < VSC_LEARN; ++l)
                learned |= vsc_dev.learn[l].param != 255 && vsc_dev.learn[l].kind == 0 && vsc_dev.learn[l].cc == a && vsc_dev.learn[l].ch == ch;
            if (!learned) { vsc_ui_knob_cc(a - 110, b); return; }
        }
        vsc_midi_event(EV_CC, ch, a | b << 8);
        return;
    }
    if (type == 0x90 && b) vsc_midi_event(EV_NOTE_ON, ch, a | b << 8);
}
