# Viscera: design

Viscera is a sculpting sampler for the M-VAVE FM-1, built on M-VAVE's official FM1 Open SDK (V14). It follows the working idea of a four-track sculpting sampler (material, grains, filter, colour, space, four modulators per track, macros and scenes) on the FM-1's keys, encoders and screen. It is its own instrument: no code, names or graphics from any other product.

## Why the SDK

The SDK keeps Bluetooth and USB working, and gives two live inputs:

- **Bluetooth audio**: a phone or tablet plays music into the FM-1 (`fm1_bluetooth_audio_read`).
- **USB audio**: a computer plays into the FM-1 (`fm1_usb_audio_read`).

It also gives BLE MIDI, DIN and USB MIDI, the official workbench (firmware updates and the sample bank), and the flash sample bank (16 entries, 32 kHz, SEA5-compressed, about 19 s in total).

Viscera keeps M-VAVE's platform library and its storage/transfer code, so the workbench still uploads sample banks. It replaces the screen and control code (`screen.c`) and the audio callback: the ISR renders Viscera instead of the stock DX7 engine. The DX7 engine is still linked (it lives in the same binary object as the platform) but never runs.

## Signal flow

```
inputs: BT audio, USB audio ─┐
track outputs (resample) ────┼─▶ per track: MATERIAL ─▶ GRAIN ─▶ FILTER ─▶ COLOR ─▶ level/pan ─┬─▶ master
flash sample bank ───────────┘                                                                └─▶ SPACE send (delay + reverb, shared)
```

### Material (one per track)

| Mode | What it does |
| --- | --- |
| PLAY | Streams one sample from the flash bank like a record: start, length, speed (−2× to +2×, reverse), pitch, loop off/on/back-and-forth. Steps and keys retrigger it. |
| LOOP | A live looper on a slice of the shared loop memory. Records from the track's input (BT, USB, both, another track, or the master), with overdub feedback, varispeed and a moving window. Keys jump to 16 slices of the loop. |
| KEYS | Four-voice chromatic sampler with an amp envelope, playing a bank sample or the track's loop. |
| THRU | The live input goes straight into the chain. Keys gate the ENV modulator. |

### Devices

Each track runs the same chain as the S-4's: MATERIAL, then GRAIN, FILTER and COLOR, then a send to SPACE. Parameters follow the S-4's devices; the FM-1 has four knobs, so each device's parameters come four at a time and SELECT steps through them.

- **GRAIN**: a per-track capture ring (8-bit µ-law) read by up to 8 grains. PITCH, RATE, SIZE, SHAPE | SCAN, SPRAY, DETUNE, WET | REVRS, FDBK, QUANT, W.MODE. REC locks the buffer.
- **FILTER**: **RESON**, a resonant bank of ten tuned bands after the S-4's filter device: CUTOFF, RESO, DECAY, PITCH | SLOPE (low-pass, band-pass, high-pass), TONE, SCALE, WET | WAVES, W.RATE, NOISE, N.RATE | TILT, DETUNE, D.MODE (SUSTAIN: the bands ring on after the shape closes; CHOKE: closing a band silences it), W.MODE (MIX or SEND). Or a classic LP, BP, HP, or PEAKS. PRESETS picks the device.
- **COLOR**: noise, drive, crush and compression in that order, then a two-band tilt: DRIVE, COMP, CRUSH, TILT | NOISE, N.DEC, N.TONE, WET | C.MODE (BITS, REDUX, BOTH), XOVER (80 Hz, 650 Hz, 2.5 kHz), N.TYPE (WHITE, PINK, DUST), W.MODE. The noise follows the input's envelope.
- **SPACE**: a shared bus (the FM-1's RAM holds one, where the S-4 has one per track). Each track has a send on MIX; the bus has a tempo-synced delay and a reverb: DELAY, TIME, FDBK, TONE | REVERB, SIZE, DAMP, LONG. REC freezes it, holding REC clears it.
- Every WET can be MIX (crossfade) or SEND (the dry signal stays, the effect is added), as on the S-4.

### Modulation

Four modulator slots per track, each **WAVE**, **RANDOM**, **FOLLOW** or **ADSR** (PRESETS changes a slot's type), with the S-4's parameters: WAVE has RATE, AMOUNT, PHASE, OFFSET, SKEW, FOLD, CURVE, SHAPE and SYNC (free, straight, triplet, dotted); RANDOM has RATE, AMOUNT, PHASE, OFFSET, LENGTH, VARIATION, SMOOTH and SYNC; FOLLOW has DELAY, AMOUNT, RISE, FALL, INPUT (this track, its material, any track, the main mix or the live input), FILTER, HOLD and GAIN; ADSR has ATTACK, DECAY, SUSTAIN, RELEASE, their three curves and AMOUNT, and plays from notes or REC.

**LFO** opens the **MATRIX** first, then each press goes to the next modulator (MOD 1–4, three pages each), then back to the matrix. SELECT pages through them all.

The MATRIX lists every route on every track: the source in its modulator's colour, the track and parameter it moves, and a bar for its AMOUNT with a tick where the modulator has it now. PRESETS picks a row; the last row is NEW, where turning any knob makes a route. The four knobs:

| KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 | REC |
| --- | --- | --- | --- | --- |
| SOURCE: any of the sixteen modulators, T1 WAV1 to T4 ENV4 (a modulator on one track can move another track) | DEVICE: T1 MATERIAL, GRAIN, FILTER, COLOR, MIX, MODS, then T2 … T4, then SPACE, PERFORM and MASTER | PARAM: that device's parameters in the order of its page's knobs (for the track's current mode and filter); turned back past the first, the route goes | AMOUNT, either way | BIPOLAR or UNIPOLAR |

**Hold LFO and turn any knob** on any page to map: the current track's modulator (the last one you looked at; PRESETS changes it while LFO is held) moves that knob's parameter, and the direction you turn sets the amount. While LFO is held the header and the knob cells take the modulator's colour and the bottom line says which modulator a turn maps. On MIX, where the knobs are the four tracks, a track's modulator can move another track's level, pan or send. Every continuous parameter can be a destination, plus each modulator's RATE and AMOUNT. Modulators restart with PLAY.

### Perform, temp and scenes

- **PERFORM**: eight macros per project, each moving up to 8 parameters on any track. Hold **ARP** and turn a knob to map the selected macro (hold ARP and turn PRESETS to choose it).
- **TEMP**: hold OCT− and OCT+ together, tweak anything, and everything snaps back on release.
- **Scenes**: 8 per project, each a snapshot of every track's sound and the macros. ARP + a pad recalls, ARP + SAVE + a pad stores; MORPH glides toward the TARGET scene. MIDI program change recalls them too.

### Sequencer

One pattern per track, 16 to 64 steps, or CAROM (notes bounce in a box and play when they hit a wall). A step holds a note, a velocity and up to two parameter locks. Internal tempo (30–300 BPM) with swing, or MIDI clock. Live recording quantises to the step.

### MIDI

MIDI comes in from the FM-1's MIDI jack, USB and Bluetooth MIDI alike.

- Notes on channels 1–4 play tracks 1–4.
- **Clock** follows the CLOCK setting: AUTO follows MIDI clock whenever it arrives, MIDI always, INT never. Start, continue and stop drive the transport (except in INT). The tempo shown is measured from the clock. Clock is taken from one port at a time.
- **MIDI learn**: hold SAVE and turn a knob, then move a controller: that CC drives the parameter. SETTINGS lists the mappings; REC clears one. Mappings belong to the FM-1, not the project, and pick up: the parameter doesn't move until the controller passes its value.
- **KNOB CC**: CC 110–113 on any channel turn KNOB 1–4 of the page on screen, with pickup (unless that CC is learned). SETTINGS → PANEL switches it off.
- **Fixed CCs** on a track's channel: 7 level, 10 pan, 11 send, 12 mix filter; 14–19 material (start, length, speed, pitch, level, SOS); 20–29 grain (pitch, rate, size, shape, scan, spray, detune, wet, feedback, reverse); 70–79 filter (cutoff, reso, decay, pitch, slope, tone, wet, waves, noise, tilt); 80–87 color (drive, comp, crush, tilt, noise, noise decay, noise tone, wet); 102–105 the four modulators' AMOUNT. On channel 16: 7 main, 9 master comp, 20 delay, 22 feedback, 23 delay tone, 24 reverb, 25 size, 26 damp, 27 morph, 28–31 and 102–105 macros 1–8, 106 swing. 120/123 stop all notes.

## Controls

The FM-1's left-hand encoders are the same kind of control on every page, but their job follows the page:

| Control | Job |
| --- | --- |
| KNOB 1–4 | The four parameters on screen |
| SELECT | The next four parameters of this device or page |
| ALGORITHM | Track 1–4 |
| PRESETS | The page's list: the device in this slot (MATERIAL, FILTER), the sequencer (STEPS or CAROM), a modulator's type, a row in the matrix, a project slot, a Bluetooth device, a learned mapping |
| MASTER | Output level |
| EDIT / FX / SEL / ENV | MATERIAL / GRAIN / FILTER / COLOR (press again for the next page) |
| GLO | SPACE, then SETTINGS: Bluetooth, AUDIO IN, clock, PANEL, MIDI learn (SELECT pages) |
| LFO | The MATRIX, then the four modulators; hold + turn a knob = map |
| HOME | MIX: level, pan, send, filter and mute, each across the four tracks; hold HOME + a top pad = track |
| ARP | PERFORM: macros and scenes; hold + turn = map a macro; hold + pad = scene |
| SEQ | STEPS or CAROM |
| SAVE | PROJECT, then SOUNDS: PRESETS picks a slot, REC loads (or saves into an empty one), SAVE + PLAY saves over; hold + turn = MIDI learn |
| PLAY | Play / stop |
| REC | The page's action: record or overdub (LOOP), restart (PLAY), lock (GRAIN), freeze (SPACE), trigger (ADSR), record steps; held, clear |
| OCT− / OCT+ | Octave; both held = TEMP |

The screen is white on black (an alternative background, a dark violet-grey gradient from #2C2A32 at the top to #100F12, is kept behind `VSC_SCREEN_GRADIENT` in `core/vsc_ui.c`). Its layout follows the S-4's: the track and page at the top with the tempo, a picture of the device, the four parameter cells (a bar for continuous values with a centre mark for bipolar ones, the value while you turn, a coloured marker where a modulator is mapped), and the page's actions along the bottom. MATERIAL, FILTER and the modulators draw as the S-4 does; GRAIN, COLOR and SPACE have their own pictures of the sound coming apart: grains lifting out of a dust horizon, a body of matter worn, coarsened and sheared by COLOR, echoes leaving as broken rings beside a drifting reverb cloud.

## Memory

The FM-1 has about 511 KB of RAM, and the platform library takes roughly 200 KB of it (Bluetooth, USB, RTOS, the DX7 engine's state and its RAM-resident code). Viscera's build replaces the SDK's 32 DX7 factory voices with one blank voice (it has no DX7 engine), which frees 16 KB of RAM and 10 KB of flash. The rest has to cover Viscera's buffers, its RAM-resident audio code (every audio function must sit in RAM, because flash programming stalls code fetched from flash), and the heap that Bluetooth needs.

| Buffer | Default | Notes |
| --- | --- | --- |
| Grain rings | 4 × 7.75 KB µ-law | 0.18 s each at 44.1 kHz |
| Loop memory | 32 KB µ-law, shared | 0.75 s HI (44.1 kHz) or 1.5 s LO (22 kHz); a track claims 1–4 of 4 slices |
| Delay | 16 KB int16 | 0.19 s, double in LONG mode at half rate |
| Reverb | about 7 KB | |
| Bank chunk cache | 8 × 640 B | decoded SEA chunks for PLAY, KEYS and grains |

All of these sizes are compile-time settings in `core/vsc_config.h`. The first hardware build should print the link map's free heap; the buffers can grow from there.

## Storage

Projects (sound, sequences, scenes, macros; about 7 KB each) live in the top 64 KB of the sample-bank area, below the DX7 library sectors: 8 slots of 8 KB. The sample bank limit is lowered by 64 KB to make room, so the bank holds about 17 s instead of 19.5 s. A legacy bank that is already larger than that stays readable, but projects can't be saved until it's rewritten smaller. Saves happen only while stopped, with audio muted for the write.

## Licence

GPL-3.0 with M-VAVE's SDK linking exception, as required by the SDK's app sources. The SEA decoder and table are MIT (Dani Biro, M-VAVE).

### The panel and the FM-1's own settings

The page buttons light for the open page, PLAY while playing, REC blinks while recording, OCT shows the octave. The pads show steps and the playhead on SEQ, scenes on ARP and tracks on HOME. Unlit keys glow at the BACKLIT level (OFF/LOW/MID/HIGH, by PWM from the control loop, and only while that loop runs fast enough to keep the glow steady).

**Audio in**: SETTINGS → AUDIO IN gathers Bluetooth audio (KNOB 1 switches it), USB audio status, a level meter for each, and the selected track's INPUT, LEVEL and the input monitor. PRESETS picks the track and REC opens its MATERIAL page (at the INPUT view for LOOP). The FM-1 has no analogue input: sound gets in over Bluetooth or USB, and samples are written to the flash bank over USB with M-VAVE's workbench.

**Sounds**: 16 slots that belong to the FM-1, not to a project. A sound is one track's MATERIAL, GRAIN, FILTER and COLOR settings and its four modulators with their routes; level, pan, send and the sequence stay with the track. REC on SOUNDS loads one onto the selected track.

SETTINGS (under GLO) → PANEL holds BACKLIT, KNOB CC and METRO (a click on each beat, accented on the bar, mixed after the limiter), and shows the version and the CPU load. These, the MIDI learn mappings and the sounds are a `struct vsc_device` in their own 4 KB flash sector after the eight project slots, sealed with a CRC and written a second after a change, only while stopped. The emulator keeps them in the browser.
