/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera: MIDI input from every port (the FM-1's MIDI jack, USB and Bluetooth MIDI).
 * Notes on channels 1-4 play tracks 1-4; CCs go to learned mappings or the fixed map;
 * program change recalls a scene; clock, start, continue and stop drive the transport.
 * Clock is taken from one port at a time, so two sources never double the tempo:
 * the first port to send clock owns it until it has been quiet for a second. */
#include "vsc.h"

static unsigned clock_port = 255;
static uint32_t clock_seen_ms;

void vsc_midi_in(unsigned port, const uint8_t *m, unsigned n, uint32_t now_ms)
{
    if (!m || !n) return;
    uint8_t st = m[0];
    if (st == 0xf8 || st == 0xfa || st == 0xfb || st == 0xfc) {
        if (clock_port != 255 && clock_port != port && now_ms - clock_seen_ms < 1000u) return;
        clock_port = port; clock_seen_ms = now_ms;
        vsc_midi_event(st == 0xf8 ? EV_CLOCK : st == 0xfa ? EV_START : st == 0xfb ? EV_CONT : EV_STOP, 0, 0, 0);
        return;
    }
    if (st >= 0xf0) return;
    unsigned ch = st & 15u, type = st & 0xf0u;
    if (type == 0xc0 && n >= 2) { vsc_midi_event(EV_PC, ch, m[1] & 127u, 0); return; }
    if (n < 3) return;
    unsigned a = m[1] & 127u, b = m[2] & 127u;
    if (type == 0xb0) {
        /* CC 110-113 turn KNOB 1-4 of the page on screen, on any channel (unless learned) */
        if (a >= 110 && a <= 113 && vsc_dev.knob_cc) {
            int learned = 0;
            for (unsigned l = 0; l < VSC_LEARN; ++l) learned |= vsc_dev.learn[l].param != 255 && vsc_dev.learn[l].cc == a && vsc_dev.learn[l].ch == ch;
            if (!learned) { vsc_ui_knob_cc(a - 110, b); return; }
        }
        vsc_midi_event(EV_CC, ch, a, b); return;
    }
    if (ch >= VSC_TRACKS) return;
    if (type == 0x90) vsc_midi_event(b ? EV_NOTE_ON : EV_NOTE_OFF, ch, a, b);
    else if (type == 0x80) vsc_midi_event(EV_NOTE_OFF, ch, a, 0);
}
