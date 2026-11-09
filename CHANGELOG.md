# Changelog

## 0.2.2

- **New:** the panel lights up. The open page's button and PLAY light, REC blinks while recording, the octave buttons show the octave, the pads show steps and the playhead on SEQ, scenes on ARP and tracks on HOME, and held keys light.
- **New:** BACKLIT (SETTINGS → PANEL): how the unlit keys glow, OFF, LOW, MID or HIGH.
- **New:** KNOB CC: CC 110–113 on any channel turn KNOB 1–4 of the page on screen, with pickup. It can be switched off.
- **New:** learned CCs pick up too. A controller far from the value does nothing until it passes it, so nothing jumps.
- **New:** METRO, a click on every beat with an accent on the bar, while playing.
- **New:** SETTINGS → PANEL shows the version and the CPU load, with the peak.
- **Changed:** BACKLIT, KNOB CC, METRO, the MIDI learn mappings and the sounds belong to the FM-1. They are kept through power-off and loading a project, in their own flash sector (in the emulator, in the browser).
- **Changed:** SETTINGS moved to GLO (press GLO on SPACE). SAVE is where things are kept: PROJECT, then SOUNDS.
- **New:** SOUNDS: 16 sound slots kept on the FM-1. A sound is a track's devices and modulators, without its mix or sequence; REC loads one onto the selected track, or saves into an empty slot, and SAVE + PLAY saves over.
- **Fixed:** SAVE + PLAY also started playback.
- **Fixed:** a saved project would not load back (the format check compared against the wrong number).
- **Changed:** projects saved by 0.2.1 don't load in 0.2.2 (MIDI learn moved out of the project).

## 0.2.1

- **Changed:** on the SEQ page, PRESETS switches the sequencer between STEPS and CAROM (it was a knob on the second page). The bottom line says which way it goes; pads still pick steps.

## 0.2.0

- **Changed:** built around the S-4's architecture on the FM-1's controls. KNOB 1–4 turn four parameters at a time, SELECT pages through them, ALGORITHM picks the track and PRESETS picks the device, type or list for the page. Parameters follow the S-4's devices, page by page.
- **New:** RESON, a resonant bank of ten tuned bands in the FILTER slot: cutoff, resonance, decay (sustain or choke), pitch, slope, tone, scale, wet, waves, noise, tilt and detune, spread across the stereo field.
- **New:** four modulator slots per track, each WAVE, RANDOM, FOLLOW or ADSR with the S-4's parameters, tempo-synced rates, and a MATRIX listing every route. Any continuous parameter can be modulated, globals included; routes are unipolar or bipolar.
- **New:** COLOR rebuilt in the S-4's order: noise that follows the input, drive, crush (bits, redux or both), compression and a two-band tilt. WET can be MIX or SEND on every device.
- **New:** MIX lays each control across the four tracks, with a per-track filter and a master compressor. PERFORM has eight macros.
- **New:** MIDI learn (hold SAVE and turn a knob), a fixed CC map, program change for scenes, and clock, start, continue and stop from the MIDI jack, USB and Bluetooth MIDI, one clock source at a time. CLOCK can be AUTO, INT or MIDI.
- **New:** the screen follows the S-4's layout, with our own pictures for GRAIN, COLOR and SPACE.
- **New:** REC acts on the page: freeze or clear SPACE, lock GRAIN, trigger an ADSR, restart PLAY.
- **Changed:** projects from 0.1.x don't load in 0.2 (the parameters changed).
- **New:** the emulator is laid out like the FM-1, and CI plays every page, device, modulator and MIDI path on the browser build before releasing.

## 0.1.4

- **Fixed:** installing over Ravel, SLOOP or another Felucca-based firmware failed part-way. Those firmwares accept only M-VAVE's own packages, so the install page now puts M-VAVE's V15 on first and then Viscera, in one go. It picks the route from what the FM-1 runs; the "switching from" tick box is gone.
- **New:** "Put M-VAVE's V15 back" on the install page.
- **New:** safe mode. Hold OCT- and OCT+ while switching on: Viscera starts with sound and controls off, and the install page can always reinstall.
- **New:** a CPU guard pauses the sound for a second if the engine ever overruns, so USB and updates are never starved.
- **Changed:** the install page stops all its animation while writing, keeps the screen awake, and warns if its tab goes to the background.
- **New:** every build is installed in CI through the real install page into a simulated FM-1 (from Ravel, stock V15 and an Open firmware), checking every block, every byte and the build ID afterwards.

## 0.1.3

- **Changed:** the bouncing sequencer is now called CAROM, after the billiards shot that glances off one cushion to strike the next.

## 0.1.2

- **Changed:** the update identity is now FM-1_86 followed by the version (0.1.2 reports FM-1_8600102), a fixed part no other firmware uses, so firmware hubs can tell Viscera apart.
- **New:** after connecting, the install page shows the build the FM-1 is running next to the package's build, so you can check an install took.

## 0.1.1

- **Fixed:** the knob columns have clear dividers, so long values never seem to run into the next knob.
- **Fixed:** the SETTINGS hint fits on screen.

## 0.1.0

First build for the FM-1. Untested on hardware.

- **New:** four tracks of MATERIAL (PLAY, LOOP, KEYS, THRU) → GRAIN → FILTER (LP, BP, HP, PEAKS) → COLOR, with a shared SPACE bus (tempo-synced delay and reverb).
- **New:** live input over Bluetooth audio and USB audio; tracks can also resample each other or the main mix.
- **New:** four modulators per track (LFO, RANDOM, FOLLOW, ENV); hold LFO and turn to assign. Four macros (hold ARP and turn), TEMP (both OCT buttons), eight scenes with morphing.
- **New:** two sequencers per track: STEPS (up to 64 steps, parameter locks, swing, live record) and CAROM, where notes bounce in a box and fire when they hit the walls (speed, gravity, loss, spread, jitter, quantise, ball count, wall intervals, gate).
- **New:** plays samples from the FM-1's flash bank, uploaded with M-VAVE's workbench. Eight project slots.
- **New:** Bluetooth MIDI (peripheral or central), USB and DIN MIDI in on channels 1 to 4, MIDI clock in.
