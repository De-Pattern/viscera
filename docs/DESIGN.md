# Viscera: design

Viscera PUTTY (1.0) is a live-capture instrument for the M-VAVE FM-1, built on M-VAVE's official FM1 Open SDK (V14). It catches loops from the input and lets you work them like putty: two at a time, or bounced between two buffers; stretched without changing pitch, frozen, phase-vocoded, broken into glass-shard grains and rebuilt. It is its own instrument, after the spirit of a few others (Synthux Spotykach, Montreal Assembly's 856 for Zellersasn, Bastl Thyme, Make Noise Morphagene, Qu-Bit Nebulae), with no code, names or graphics from any of them. 0.x was a four-track sculpting sampler; 1.0 replaced its engine and kept the framework: four tracks, COLOR, SPACE, the modulators and matrix, scenes, macros, STEPS and CAROM, DISSECT's listener, the screen style and the install page.

## Why the SDK

The SDK keeps Bluetooth and USB working, and gives two live inputs:

- **Bluetooth audio**: a phone or tablet plays music into the FM-1 (`fm1_bluetooth_audio_read`).
- **USB audio**: a computer plays into the FM-1 (`fm1_usb_audio_read`).

It also gives BLE MIDI, DIN and USB MIDI, the official workbench (firmware updates and the sample bank), and the flash sample bank (16 entries, 32 kHz, SEA5-compressed).

Viscera keeps M-VAVE's platform library and its storage/transfer code, so the workbench still uploads sample banks. It replaces the screen and control code (`screen.c`) and the audio callback: the ISR renders Viscera instead of the stock engine.

## Signal flow

```
inputs: BT, USB, both ──┐
the partner's output ───┼─▶ per track: SOURCE ─▶ PUTTY ─▶ (RESON) ─▶ COLOR ─▶ level/pan ─┬─▶ master
the reel, the bank ─────┘   (capture,     (HANDS,     one track             └─▶ SPACE send (delay + reverb)
                             reel, thru)   SHARDS,     at a time
                                           SPECTRA)
```

Four tracks, A to D, in two pairs: **A+B** and **C+D**. A pair bounces, crosses and moves together.

### SOURCE (EDIT)

| Knob | |
| --- | --- |
| SOURCE | CAPTURE (a buffer in RAM), REEL (the reel or a bank sample) or THRU (the input, live, through a short ring) |
| INPUT | BT, USB, BT+USB (default), another track, or the main mix |
| RATE | HI 44.1 kHz (0.33 s), MID 22 kHz (0.65 s, default) or LO 11 kHz (1.31 s): each capture is 14,400 bytes of 8-bit µ-law |
| SOS | how much of the old an overdub keeps |
| BOUNCE | OFF; IN: the track captures its partner's output; PING: when one ends a capture, the other takes it back, as long, so the sound passes between them |
| REEL / SPLICE | KEPT (the reel) or a bank slot S1–S16; ALL or one splice |
| GAIN, KEYS | input gain; what keys do: OFF, PITCH, PLAY (restart and gate), SLICE (a sixteenth of the window each, from F3) |

**REC** on HANDS, SHARDS, SPECTRA or SOURCE: tap to start a capture, tap to end it (the gesture sets the length; a full buffer ends it). Hold to overdub. Hold and turn a knob to record a robot. REC + PLAY clears the capture and the track's robots. SAVE + REC keeps the capture on the reel.

**The reel.** KEEP writes the capture to flash as a splice. The reel lives in the stock firmware's project sectors 0 to 14 (Viscera never used them; the stock settings and staging sectors stay untouched): a 4 KB index, then 56 KB of audio, each splice starting on a sector, about 2.6 s at MID in all, 16 splices at most. The first KEEP asks again, because a new reel takes the place of any projects the stock firmware kept there. Recording straight to flash isn't possible: on this SDK every flash write briefly silences audio, so KEEP pauses the sound for a moment, as saving does. SETTINGS → PANEL → REEL shows the room left; turn it and press REC to erase the reel.

### PUTTY: three ways to work the capture

The page is the mode: opening HANDS, SHARDS or SPECTRA puts the track into it. Each reads a window of the source, POS to POS + SIZE, with a playhead moving through it at STRETCH (0.25× to 8×, the top: frozen), independent of PITCH (±24 semitones).

- **HANDS** (HOME): two overlapping Hann grains follow the playhead. STRETCH, PITCH, POS, SIZE | GRAIN (10 ms to 260 ms), JITTER, REVRS, LEVEL | FREEZE, KEYS, GAIN, SOS.
- **SHARDS** (FX): up to eight grains from the window. SHAPE runs smooth (Hann) → swell (reverse expodec) → expodec → glass (sharper, shorter, and at the top it SHATTERS: octaves up, denser). DENS, SPRAY (scatter in the window), TRIG: FREE (random), HIT (bursts on the listener's hits), NOTE (keys, steps, CAROM), CLOCK (on the steps) | SIZE, SPREAD (pitch scatter), REVRS (chance), STEREO | the hands. Sparse, sharp shards are brought up to the level of the source.
- **SPECTRA** (SEL): a 512-point phase vocoder at the playhead. STRETCH, BLUR (magnitudes smeared over time), SHIFT (±24 semitones, on top of PITCH), CROSS (the partner's magnitudes, through their geometric mean, on this track's phases) | GATE (the loud bins only), TILT, HOLD (peaks held), PHASE (scatter) | PITCH, POS, SIZE, FREEZE. Frames every 256 samples with sine windows; each spectral peak's phase moves on by its true frequency (from a parabola through its log magnitudes), and the bins around it keep their place relative to it (identity phase locking), so frozen sound stays a tone rather than a smear. Two tracks can be SPECTRA at once (RAM); a third plays as HANDS and says so. The two vocoders work on alternate blocks.

**Robots.** Hold REC on a putty page and turn a knob: the turn is recorded (up to about two seconds) and, when REC is let go, loops around the knob on its own. Four a track; a fifth replaces the oldest. A robot shows as a lavender mark in its knob's cell, with a tick where it has the parameter now. Robots aren't kept in projects.

**Two hands.** Hold OCT− and turn a knob: both tracks of the pair move together. Hold OCT+: they move apart. The octave only changes when an OCT button is let go without a knob turned. Both held: TEMP.

### RESON: a resonant filterbank (ENV, after COLOR)

Eight resonant bands at Grains' frequencies (200, 400, 800, 1200, 1600, 2400, 3200 and 4800 Hz), on one track at a time, after its PUTTY and before its COLOR. Opening the page puts it on the open track (TRACK on SETUP moves it, 0 turns it off); ENV toggles COLOR and RESON. Off, it costs nothing.

Each band is two bandpass biquads with the same centre: one the sound goes through (no narrower than WIDTH, made up in level as Grains does, ×4 at Q 8), one that rings with a decay of its own (RESO, and DECAY per band, 10 ms to several seconds). Both are the bilinear transform of k s / (s² + k s + 1), the curve of a TPT state-variable bandpass, at about half its arithmetic. Tuning, decay and gains are worked out once per 32 samples; the envelopes and gains move every fourth sample; a band with no gain isn't run.

| View | Knobs |
| --- | --- |
| BANK | PROFILE (LIQ MEM, GLASS, THROAT, DUST, RADIO, DROPS), WIDTH, RESO, MIX |
| BAND | BAND (or pads 1–8), GAIN (dB, 0: off), FREQ (±1 octave), DECAY |
| VOCODE | MODSRC (SELF, INPUT, PARTNR, OSC, NOISE, OUTPUT), ATTACK, RELEAS, DEPTH |
| PING | TRIG (OFF, HIGH, FULL, LOW, HITS, NOTES), QUANT (OFF, 1/32 … 1/4), HARD, DENS |
| MOTION | LIQUID, FDBK, SHIFT (±12 semitones, all bands), SPECTR |
| PARTICLES | COHES, DROPS, MIGR, OSC (sine → saw → pulse) |
| SETUP | MODE (OFF, FILTER, VOCODE, PING, HYBRID), SENS, TRACK |

- **FILTER**: the sound through the bank. **VOCODE**: each band's gain follows the modulator's level in that band (SELF: the band itself; INPUT: the track's input; PARTNR: the other track of the pair; OSC and NOISE: a source of its own, the carrier being the track; OUTPUT: the band's own output, a little late). **PING**: only the ringing bank, excited by bursts. **HYBRID** (default): half of each, vocoded.
- **Pings**: a transient detector on the modulator (a fast against a slow envelope of its HIGH, FULL or LOW band, with an adaptive threshold from SENS, hysteresis and 40 ms of refractory time), the listener's HITS, NOTES (keys, steps, CAROM on RESON's track) or REC on the page. QUANT holds a ping for the grid: the steps' when playing, the tempo's when not. HARD sets the burst from 4 ms (soft) to 0.3 ms (hard); SPECTR weights it towards the bands where the modulator has energy now.
- **LIQUID**: two slow random walks move each band's tuning (±3 %), decay and gain (±3 dB) a little, each band by its own weights. **FDBK** returns the wet sound, DC-blocked and soft-limited, into the ringing bank.
- **Particles**: DENS throws dust (specks into one band each); COHES ties them to the detector's activity. DROPS plays up to three micrograins (4–18 ms, Hann-like, ±1 % speed) from a 23 ms ring of the wet sound; MIGR sends them into another band instead of out.
- The screen: eight bars glowing with each band's output over faint stacks of their gains, a reflection under the line, the band being edited in a lavender pill; BAND opens a window on its curve, VOCODE shows carrier and modulator, PING throws sparks off the bars.
- Under the CPU guard's second step RESON runs only the bands within 12 dB of the loudest, and SELF in place of an analysis bank.

### COLOR and SPACE

- **COLOR** (ENV): noise, drive, crush and compression in that order, then a two-band tilt: DRIVE, COMP, CRUSH, TILT | NOISE, N.DEC, N.TONE, WET | C.MODE (BITS, REDUX, BOTH), XOVER, N.TYPE (WHITE, PINK, DUST), W.MODE.
- **SPACE** (GLO): a shared bus with a tempo-synced delay (8-bit µ-law, as the captures) and a reverb: DELAY, TIME, FDBK, TONE | REVERB, SIZE, DAMP, LONG. REC freezes it, holding REC clears it.

### DISSECT: the listener

GLO's second page. One listener for the whole engine hears its SOURCE once a block, only while something uses it: three bands, hits (SENS, FOCUS) and the pitch with its first eight partials. HITS can throw CAROM balls or clock the steps. SHARDS TRIG HIT bursts on the hits; FOLLOW modulators can follow the bands, the hits, the pitch or the brightness. REC on DISSECT opens SHARDS with TRIG HIT.

### Modulation

Four modulator slots per track, each **WAVE**, **RANDOM**, **FOLLOW** or **ADSR** (PRESETS changes a slot's type), with the S-4's parameters: WAVE has RATE, AMOUNT, PHASE, OFFSET, SKEW, FOLD, CURVE, SHAPE and SYNC (free, straight, triplet, dotted); RANDOM has RATE, AMOUNT, PHASE, OFFSET, LENGTH, VARIATION, SMOOTH and SYNC; FOLLOW has DELAY, AMOUNT, RISE, FALL, INPUT (this track, its source, any track, the main mix or the live input), FILTER, HOLD and GAIN; ADSR has ATTACK, DECAY, SUSTAIN, RELEASE, their three curves and AMOUNT, and plays from notes or REC.

**LFO** opens the **MATRIX** first, then each press goes to the next modulator (MOD 1–4, three pages each), then back to the matrix. SELECT pages through them all.

The MATRIX lists every route on every track: the source in its modulator's colour, the track and parameter it moves, and a bar for its AMOUNT with a tick where the modulator has it now. PRESETS picks a row; the last row is NEW, where turning any knob makes a route. The four knobs:

| KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 | REC |
| --- | --- | --- | --- | --- |
| SOURCE: any of the sixteen modulators, A WAV1 to D ENV4 (a modulator on one track can move another track) | DEVICE: A SOURCE, PUTTY, COLOR, MIX, MODS, then B … D, then SPACE, PERFORM, MASTER and DISSECT | PARAM: that device's parameters in the order of its page's knobs (PUTTY: the track's mode first); turned back past the first, the route goes | AMOUNT, either way | BIPOLAR or UNIPOLAR |

**Hold LFO and turn any knob** on any page to map: the current track's modulator (the last one you looked at; PRESETS changes it while LFO is held) moves that knob's parameter, and the direction you turn sets the amount. While LFO is held the header and the knob cells take the modulator's colour and the bottom line says which modulator a turn maps. On MIX, where the knobs are the four tracks, a track's modulator can move another track's level, pan or send. Every continuous parameter can be a destination, plus each modulator's RATE and AMOUNT. Modulators restart with PLAY.

### Perform, temp and scenes

- **PERFORM**: eight macros per project, each moving up to 8 parameters on any track. Hold **ARP** and turn a knob to map the selected macro (hold ARP and turn PRESETS to choose it).
- **TEMP**: hold OCT− and OCT+ together, tweak anything, and everything snaps back on release.
- **Scenes**: 8 per project, each a snapshot of every track's sound and the macros. ARP + a pad recalls, ARP + SAVE + a pad stores; MORPH glides toward the TARGET scene. MIDI program change recalls them too.

### Sequencer

One pattern per track, 16 to 64 steps, or CAROM (notes bounce in a box and play when they hit a wall). A step holds, after the 856's fragments, a SLICE (the playhead jumps to that sixteenth of the window; OFF: it plays on), a PITCH, a LEN (in eighths of a step) and a PROB, then a velocity and a parameter lock. PRESETS picks the step, the pads switch steps on and off. Internal tempo (30–300 BPM) with swing, or MIDI clock. Live recording quantises to the step.

### MIDI

MIDI comes in from the FM-1's MIDI jack, USB and Bluetooth MIDI alike.

- Notes on channels 1–4 play tracks A–D.
- **Clock** follows the CLOCK setting: AUTO follows MIDI clock whenever it arrives, MIDI always, INT never. Start, continue and stop drive the transport (except in INT). The tempo shown is measured from the clock. Clock is taken from one port at a time.
- **MIDI learn**: hold SAVE and turn a knob, then move a controller: that CC drives the parameter. SETTINGS lists the mappings; REC clears one. Mappings belong to the FM-1, not the project, and pick up: the parameter doesn't move until the controller passes its value.
- **KNOB CC**: CC 110–113 on any channel turn KNOB 1–4 of the page on screen, with pickup (unless that CC is learned). SETTINGS → PANEL switches it off.
- **Fixed CCs** on a track's channel: 7 level, 10 pan, 11 send, 12 mix filter; 14–19 the hands (POS, SIZE, STRETCH, PITCH, GAIN, SOS); 20–28 SHARDS (SHAPE, DENS, SPRAY, SIZE, SPREAD, REVRS, STEREO) and HANDS' GRAIN and JITTER; 70–76 SPECTRA (BLUR, SHIFT, CROSS, GATE, TILT, HOLD, PHASE); 80–87 color (drive, comp, crush, tilt, noise, noise decay, noise tone, wet); 102–105 the four modulators' AMOUNT. On channel 16: 7 main, 9 master comp, 20 delay, 22 feedback, 23 delay tone, 24 reverb, 25 size, 26 damp, 27 morph, 28–31 and 102–105 macros 1–8, 106 swing. 120/123 stop all notes.

## Controls

| Control | Job |
| --- | --- |
| KNOB 1–4 | The four parameters on screen |
| SELECT | The page's next view |
| ALGORITHM | Track A–D |
| PRESETS | The page's list: SOURCE's source, the step (SEQ), a modulator's type, a row in the matrix, a project or sound slot, a Bluetooth device, a learned mapping |
| MASTER | Output level |
| HOME / FX / SEL | HANDS / SHARDS / SPECTRA, and the track's mode with them (press again for the next view) |
| EDIT | SOURCE, then MIX |
| ENV | COLOR |
| GLO | SPACE, then DISSECT, then SETTINGS: Bluetooth, AUDIO IN, clock, PANEL, MIDI learn |
| LFO | The MATRIX, then the four modulators; hold + turn a knob = map |
| ARP | PERFORM: macros and scenes; hold + turn = map a macro; hold + pad = scene |
| SEQ | STEPS or CAROM |
| SAVE | PROJECT, then SOUNDS: REC loads (or saves into an empty slot), SAVE + PLAY saves over; hold + turn = MIDI learn; SAVE + REC on a putty page: KEEP |
| PLAY | Play / stop |
| REC | Putty pages and SOURCE: capture (tap, tap), overdub (hold), robot (hold + turn); REC + PLAY clears. Elsewhere the page's action: freeze (SPACE), trigger (ADSR), record steps, BI/UNI (matrix); held, clear |
| OCT− / OCT+ | Octave; held with a knob: the pair together (OCT−) or apart (OCT+); both held = TEMP |

The screen follows the PUTTY drawings (`docs/art/putty-mockups.png`): white on black with a lavender accent and small flashes of red. A small header (VISCERA PUTTY and the page's views as dots), the track and page large ("A / HANDS"), REC in red while anything records, and the pair's link as a lavender badge (A→B: B captures A; A<>B: PING; A×B: CROSS). Then the picture: HANDS draws both tracks of the pair with the window as a lavender pane, the playhead and, while capturing, a red record head; SHARDS throws up a cloud of dust where the shards play, a tick for each one born, and their window shape below; SPECTRA scrolls its spectrum; SEQ draws the steps as blocks (lavender where a step jumps to a slice), the bounces between the steps that play, and the source with the slices marked. Below, four knob cells: the name, the value large, a lavender bar (a centre mark for bipolar values), coloured ticks for modulators and a pale one for a robot; choices show as lavender badges and SHAPE draws its window. The footer: the input, the pair's lengths or the page's subject, and the tempo; a page's gestures show there for a moment when it opens, and notices replace it.

## Memory

The FM-1 has about 511 KB of RAM, and the platform library takes roughly 200 KB of it (Bluetooth, USB, RTOS, the DX7 engine's state and its RAM-resident code). Viscera's build replaces the SDK's 32 DX7 factory voices with one blank voice (it has no DX7 engine), which frees 16 KB of RAM and 10 KB of flash. The rest has to cover Viscera's buffers, its RAM-resident audio code (every audio function must sit in RAM, because flash programming stalls code fetched from flash), and the heap that Bluetooth needs.

| Buffer | Default | Notes |
| --- | --- | --- |
| Captures | 4 × 14,400 B µ-law | 0.33 s HI, 0.65 s MID, 1.31 s LO |
| Vocoders | 2 × 5 KB, plus 6 KB shared | 16-bit phases and overlap-add |
| Delay | 8 KB µ-law | 0.19 s, double in LONG mode at half rate |
| Reverb | about 7 KB | |
| RESON | about 1.6 KB, plus a 1 KB µ-law ring | one instance; its state and the droplets' ring |
| Bank chunk cache | 8 × 640 B | decoded SEA chunks for REEL S1–S16 |

All of these sizes are compile-time settings in `core/vsc_config.h`. The first hardware build should print the link map's free heap; the buffers can grow from there.

## Storage

Projects (sound, sequences, scenes, macros; about 7 KB each; 0.x projects don't load in 1.0) live in the top 64 KB of the sample-bank area, below the DX7 library sectors: 8 slots of 8 KB. The sample bank limit is lowered by 64 KB to make room, so the bank holds about 17 s instead of 19.5 s. A legacy bank that is already larger than that stays readable, but projects can't be saved until it's rewritten smaller. Saves happen only while stopped, with audio muted for the write.

## Licence

GPL-3.0 with M-VAVE's SDK linking exception, as required by the SDK's app sources. The SEA decoder and table are MIT (Dani Biro, M-VAVE).

### The panel and the FM-1's own settings

The page buttons light for the open page, PLAY while playing, REC blinks while recording, OCT shows the octave. The pads show steps and the playhead on SEQ, scenes on ARP and tracks on HOME. In the emulator REC is the lavender key and flashes red while recording. Unlit keys glow at the BACKLIT level (OFF/LOW/MID/HIGH, by PWM from the control loop, and only while that loop runs fast enough to keep the glow steady).

**Audio in**: SETTINGS → AUDIO IN gathers Bluetooth audio (KNOB 1 switches it), USB audio status, a level meter for each, and the selected track's INPUT, GAIN and the input monitor. PRESETS picks the track and REC opens its SOURCE page. The FM-1 has no analogue input: sound gets in over Bluetooth or USB, and samples are written to the flash bank over USB with M-VAVE's workbench.

**Sounds**: 16 slots that belong to the FM-1, not to a project. A sound is one track's SOURCE, PUTTY and COLOR settings and its four modulators with their routes; level, pan, send and the sequence stay with the track. REC on SOUNDS loads one onto the selected track.

SETTINGS (under GLO) → PANEL holds BACKLIT, KNOB CC and METRO (a click on each beat, accented on the bar, mixed after the limiter), the room left on the reel, and shows the version and the CPU load. These, the MIDI learn mappings and the sounds are a `struct vsc_device` in their own 4 KB flash sector after the eight project slots, sealed with a CRC and written a second after a change, only while stopped. The emulator keeps them in the browser.
