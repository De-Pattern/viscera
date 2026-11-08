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

- **GRAIN**: a per-track capture ring (8-bit µ-law) read by up to 8 grains. Size, rate (density), pitch, spread (position jitter), contour (window shape), reverse chance, freeze, mix.
- **FILTER**: state-variable low-pass, band-pass, high-pass, or PEAKS (four tuned resonant bands following a scale, with decay and spread).
- **COLOR**: drive, compression, bit crush, rate reduction, noise (hiss or crackle), tilt tone.
- **SPACE**: a shared bus. Each track has a send; the bus has a tempo-synced delay and a small reverb.

### Modulation

Four modulators per track: **LFO** (shape, rate, sync), **RANDOM** (rate, smoothing), **FOLLOW** (envelope follower on the material), **ENV** (ADSR from keys and steps). Up to 12 routes per track. Hold **LFO** and turn any encoder to set how much the selected modulator moves that parameter.

### Macros, temp and scenes

- Four macros per project, each moving up to 8 parameters on any track, with a depth per route. Hold **ARP** and turn an encoder to assign the selected macro.
- **TEMP**: hold OCT− and OCT+ together, tweak anything, and everything snaps back on release.
- **Scenes**: 8 per project, each a snapshot of every track's sound and the macros. Recall them from the SCENE page or with ARP + a key; morph between the current scene and a target scene with one encoder.

### Sequencer

One pattern per track, 16 to 64 steps. A step holds a note, a velocity and up to two parameter locks. Steps retrigger PLAY, play KEYS notes, jump LOOP slices, or gate THRU's ENV modulator. Internal tempo (30–300 BPM) with swing, or MIDI clock. Live recording quantises to the step.

## Controls

| Control | Job |
| --- | --- |
| E1–E6 | The six parameters on screen |
| E7 | Next/previous parameter row on the page |
| HOME | Mixer; hold HOME and press one of the top four keys to pick track 1–4 |
| EDIT | MATERIAL |
| FX | GRAIN |
| SCL | FILTER |
| ENV | COLOR |
| LFO | MODS (press again for the next modulator); hold + turn = assign |
| GLO | SPACE and clock |
| ARP | SCENE / MACROS; hold + turn = assign the selected macro; hold + key = recall scene |
| SEQ | STEPS (the first 16 keys toggle steps) |
| SAVE | PROJECT (press again for SETTINGS: Bluetooth, MIDI, input) |
| PLAY / REC | Transport; REC arms the loop on LOOP tracks, step or live recording on STEPS |
| OCT− / OCT+ | Octave; both held = TEMP |
| Keys | Play the selected track |

## Memory

The FM-1 has about 511 KB of RAM, and the platform library takes roughly 200 KB of it (Bluetooth, USB, RTOS, the DX7 engine's state and its RAM-resident code). The rest has to cover Viscera's buffers, its RAM-resident audio code (every audio function must sit in RAM, because flash programming stalls code fetched from flash), and the heap that Bluetooth needs.

| Buffer | Default | Notes |
| --- | --- | --- |
| Grain rings | 4 × 12 KB µ-law | 0.28 s each at 44.1 kHz |
| Loop memory | 64 KB µ-law, shared | 1.5 s HI (44.1 kHz) or 3 s LO (22 kHz); a track claims 1–4 of 4 slices |
| Delay | 24 KB int16 | 0.28 s, double in LONG mode at half rate |
| Reverb | about 7 KB | |
| Bank chunk cache | 8 × 640 B | decoded SEA chunks for PLAY, KEYS and grains |

All of these sizes are compile-time settings in `core/vsc_config.h`. The first hardware build should print the link map's free heap; the buffers can grow from there.

## Storage

Projects (sound, sequences, scenes, macros; about 7 KB each) live in the top 64 KB of the sample-bank area, below the DX7 library sectors: 8 slots of 8 KB. The sample bank limit is lowered by 64 KB to make room, so the bank holds about 17 s instead of 19.5 s. A legacy bank that is already larger than that stays readable, but projects can't be saved until it's rewritten smaller. Saves happen only while stopped, with audio muted for the write.

## Licence

GPL-3.0 with M-VAVE's SDK linking exception, as required by the SDK's app sources. The SEA decoder and table are MIT (Dani Biro, M-VAVE).
