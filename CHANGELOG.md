# Changelog

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
