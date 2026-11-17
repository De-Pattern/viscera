# Changelog

## 2.5.5 — the battery

- **A battery glyph at the bottom right of every page**, as Ravel has: three bars from the battery's level (Ravel's thresholds), red and empty when it's low, a bolt on USB power. Footer hints lose whole words before they'd reach it. (The browser emulator shows a full battery.)
- No change to the sound.

## 2.5.4 — the intro's stab, retuned

- **The vocal stab is clean now** (no 4-bit crush on it): a breathy "ah" with a little vibrato, through vowel resonances, in the same one-bar rhythm. Its line: D, D, B flat, D, G, then D, D, B flat, D, F.
- **Choppier, and denser**: the bass and hats chop on every sixteenth, the longer stabs stutter in thirty-seconds; the kick and bass keep their 4-bit grit.
- The bass is written under each chord (no sour notes at the drop to B flat). Same length (3.8 s) and level.

## 2.5.3 — the intro's vocal stab

- **The intro music is rebuilt around a choppy, breathy vocal "ah" stab** (sawtooths through two vowel resonances, cut short) in a distinct one-bar rhythm, over the kick, clap and the gritty D bass, now at 129 BPM, dropping to B flat in the second bar. Our own notes.
- The intro is half a second longer (3.8 s) to fit two bars. Still 4-bit, at about the same level.

## 2.5.2 — new intro music

- **The intro has new music**: an original, frantic lo-fi piece: a hard kick, a clap on two and four, a distorted sawtooth bass pulsing in sixteenths on D with octave jumps, a strobing minor stab, dropping to B flat for the last bar. Still 4-bit, the same length and the same level.
- Nothing else changes in the sound.

## 2.5.1 — the VU meters lit

- **The VU meters glow** a warm, deep orange from inside their corners, like a lamp behind the face.
- **The INPUT page's big meter** has a shorter needle, so its scale and numbers sit lower, clear of the frame.
- No change to the sound.

## 2.5.0 — the LFO matrix

- **Hold LFO and turn a knob** on any page: the robot (1, or the one last shown on ROBOTS) moves that knob, by as much as it's turned, either way; back to zero removes it. The knob stays where it was. PRESETS while holding LFO picks robot 1 or 2. A tap of LFO opens ROBOTS as before (now on release).
- **LFO MATRIX** (ROBOTS, SELECT past the robots): eight routes shared by the two robots, each drawn with its depth and the robot's swing; FROM, TO, DEPTH, DELETE on the knobs, PRESETS picks the row. These are on top of each robot's own target.
- Routes can move the tape, input, track, bounce and RESON knobs (not the EQ, the compressor or switches). They're saved with the project; 2.4's projects load, with an empty matrix.
- Nothing changes in the sound unless a route is set.
- **For the FM-1 Firmware Hub**: `fmhub.json` and a card image (`docs/card.png`).

## 2.4.3 — two screens drawn right

- **The master EQ's curve** drew a low cut wrongly near the bottom of the range (a dip that rose again); the sound was right. It's now worked out in the form that stays exact at low frequencies.
- **BOUNCE while printing** showed "TO -" and NO FREE TRACK; it shows where the print goes.

## 2.4.2 — mute and solo on the mixer; a quieter title

- **On the MIXER (EDIT), the track keys mute** (F3, G3, A3, B3: tracks 1–4; lit while muted) instead of arming, and **the next four white keys solo** (C4, D4, E4, F4; lit while soloed). With any track soloed, the others are silent; solo more than one to hear them together. The strip's button shows M and S. Elsewhere the track keys still arm.
- **The title's music plays at 60 %** of its level.

## 2.4.1 — the compressor keyed by the input

- **KEY INPUT**: the master compressor can listen to the live input (Bluetooth or USB, after its trim and drive) and duck the tape with it, whether the input is monitored or not. KEY is the sidechain: the mix, a track (after its strip, before its fader and mute, so a silent track can still drive it) or now the input.

## 2.4.0 — MOTION and SLICE (SEL)

- **MOTION** (SEL): the TAPE screen again, its knobs SPEED, PITCH, RESON's MIX and LEVEL, then (SEL again) the selected track's SPEED, PITCH, PAN and LEVEL. Press REC and turn knobs: each one turned is recorded round the loop (64 points a pass) and plays back on every pass after REC again. Up to four knobs move at once; a knob with motion shows an M, and its value moving. ARP + a knob erases that knob's motion; ARP + REC erases all of it. Saved with the project.
- **SLICE** (SEL a third time): a track's loop cut into 4, 8 or 16 slices on the white keys, on an even grid or (SNAP HITS) moved to the hits nearest each line; the screen draws the loop and the cuts. While the page is open the track's own tape is silent and its slices play through its strip. REC records what you play into a free track (TO: FREE, or one you pick), round the loop until REC again; the original stays on its track, so ARP + REC clears the take to start over, and clearing the original afterwards frees its track.
- Projects from 2.3 and before don't load (the project has room for the motion now).
- The tape is 1.5 % shorter (MID 3.8 s, LO 5.6 s a track, HI still 2.8 s): the room the new pages take from the heap M-VAVE's library starts from.

## 2.3.0 — loop START and END; one track armed at a time

- **Trim the loop at both ends**: HOME's new LOOP view (HOME, then HOME again) has START and END (a hundredth and a twentieth of a second a click), SLIDE (moves the loop along the take, its length kept) and FULL (back to the whole take). The tracks show the take with what's outside the loop dimmed and START and END marked. Punches and DUB passes run round the trimmed loop cleanly, wherever START falls.
- **Arming a track disarms the one before** (and punches it out if it was recording): one track records at a time.
- HOME's views are now TAPE, LOOP and TRACK.

## 2.2.0 — a start-up title

- **At power-on, VISCERA: TAPE flashes up in a different face every beat** (block, condensed, wide, hollow, slanted, dotted; red on black, now and then in a green box or on a magenta slab), over three seconds of original 4-bit chip music: a four-on-the-floor kick, a buzzing bass, a pulse arpeggio and noise hats. Then the name holds for a moment. Any button or key skips it. Drawn and played by the firmware itself (no pictures, no samples), so it costs almost nothing.
- START WITH's message ("OPENED 3 ...") stays up long enough to be read after the title.

## 2.1.1 — the backlight, as Ravel's

- **Backlit buttons and keys**: the ones that aren't lit glow dimly so you can find them in the dark; what's lit is lit fully. The panel's own scan does it (one frame in six at LOW, three at MID, two at HIGH), as Ravel does, so it is steady however busy the FM-1 is. Before, the glow came from the control loop and often didn't show. GLO › SETTINGS › LIGHT sets it (OFF, LOW, MID, HIGH); it is kept through power-off.
- The install page's version menu names the two firmwares: VISCERA: TAPE (2.x, the 4-track) and VISCERA (original) (1.x).

## 2.1.0 — tracks of their own, a master section, START WITH

- **Each track has its own SPEED, PITCH and DUB level** (HOME, then HOME again: TRACK). Tracks at different speeds drift against each other round the loop. DUB sets how much of a track a DUB pass keeps (100 %: all of it; lower: older passes fade). The tape's SPEED and PITCH still move everything.
- **Selecting a track without arming it**: hold HOME and press its key, or turn ALGORITHM on TAPE or MIXER.
- **CLEAR is ARP**: hold ARP and press a track's key to clear that track, or PLAY to erase the tape (it was HOME, which now selects).
- **MASTER (FX)**: a four-band EQ on everything you hear (bells; band 1 also a low shelf or LO CUT, band 4 a high shelf or HI CUT), drawn as a curve; then Ravel's glue compressor (threshold, ratio, attack, release with AUTO, makeup, mix, and KEY: the mix or one track), with a gain-reduction bar and its curve.
- **START WITH** (GLO › SETTINGS): LAST opens the project last saved or loaded at power-on and says what opened; BLANK starts fresh. After a start that never ran properly it starts blank and says "AFTER A RESET: NEW PROJECT".
- INPUT is now the last view of EDIT (after the mixer). The browser emulator has no demo loop any more: choose a file, your computer's audio or a microphone.
- Projects from 2.0.0 don't load (the desk has new parameters). CPU (fm1-emu): four tracks all pitch-shifted, with an overdub, 22 %; with RESON 33 %.

## 2.0.0 — 4-Track

A new firmware: an overdubbing four-track tape looper, in the spirit of a TASCAM Portastudio. Everything of 1.x but RESON is gone; 1.x stays on the install page, and its projects don't load here.

- **The tape**: four mono tracks of 4-bit ADPCM in RAM, at a TAPE SPEED of HI (22 kHz, 2.8 s a track), MID (16 kHz, 3.9 s) or LO (11 kHz, 5.7 s). The first take sets the loop (or LOOP sets it first); then REC punches the armed tracks in and out, DUB or REPLACE. A punch that ends mid-block runs on to the block's end, so there are no clicks where it stops.
- **SPEED** is varispeed (half to double, pitch and time together); **PITCH** shifts the tape's mix ±12 semitones without changing its time.
- **Preamps** after the TASCAM 424's when pushed: clean below 0 VU, then a soft knee with a little second harmonic. One on the input (TRIM and DRIVE) and one on each channel (TRIM, with a lamp).
- **The mixer**: four strips with VU meters; KNOB 1–4 are the four levels, then trims, then pans.
- **INPUT** on one page: BT, USB or BOTH with each one's state and a live lamp, a big VU meter, monitoring (AUTO while armed), DUB or REPLACE. Choosing BT switches Bluetooth audio on.
- **BOUNCE**: the picked tracks, through RESON and a preamp, onto a free track, one loop; the sources kept or cleared.
- **RESON lite**: the eight bands with six profiles on MODE (LIQUID, GLASS, THROAT, DUST, RADIO, DROPS), MIX, DECAY, WIDTH, SHIFT, DUST and the band levels; on a track, the input or a bounce.
- **Robots**: two, as LFOs (sine, triangle, saw, square, S+H, drift, with skew) or as a knob gesture recorded with HOME held, on SPEED, PITCH, levels, pans or RESON.
- **MIDI learn** for knobs (SAVE + turn) and for REC, PLAY and the arm keys (SAVE + press, then a note).
- **The screen** is a tape machine: reels that wind with the loop, a counter, amber VU meters, aluminium knobs. The browser emulator gets a Portastudio-grey panel with a four-LED-ladder meter bridge.
- **CPU** (fm1-emu, the FM-1's timing): four tracks playing with an overdub and PITCH 15 %; RESON adds 10–11 %.
- Projects keep the desk, not the tape (it stays in RAM).

## 1.2.0 — lighter on the CPU

The FM-1 has proved slower than the emulator these figures come from, so this version takes work out where it is hardly heard. In fm1-emu at the FM-1's timing:

| | 1.1.3 | 1.2.0 |
| --- | --- | --- |
| RESON as it starts (LIQ MEM) | 16 % | 11 % |
| RESON, DUST profile | 17 % | 12 % |
| RESON at its heaviest | 21 % | 15 % |
| Two SPECTRA tracks | 48 % | 35 % |
| SPACE | 7 % | 5 % |

- **RESON runs its bank, detector and dust at 22 kHz**, on each pair of samples, and interpolates its output back. The bands reach 9.9 kHz, above the top band's 4800 Hz even shifted an octave up (+FREQ +SHIFT at the top are capped there). Pings, dust and droplets sound as before; levels match within about 2 dB.
- **SPECTRA leaves out the bins above its source's Nyquist** (11 kHz at MID, 5.5 kHz at LO, moved by the pitch): there is nothing there to process. HI captures are processed in full.
- **The reverb runs at 22 kHz** with its delays halved, so it has the same times and a top end that stops at 11 kHz. It frees 3.1 KB of RAM.
- **The CPU guard steps in at 75 %** of a block, from 90 %, and steps back below 50 %.
- **The screen draws ~10 frames a second, from ~30, while the guard eases or the audio takes over 60 %**: it shares the core with the audio, and the knobs stay quick.
- **Changed:** 1.0's projects (format 6) load in the emulator only now, for flash; shorter hints.

## 1.1.3 — out of safe mode with PLAY; LFO opens the LFO

- **Safe mode can be left:** press **PLAY** in safe mode to start Viscera anyway. Safe mode now takes three starts that never ran well (from two), and a start counts as good after 4 s (from 8 s stopped). A safe-mode start still clears the count after 4 s, so the next power-on starts Viscera.
- **LFO opens the first modulator** (RATE, AMOUNT, PHASE, OFFSET; SELECT for SKEW, FOLD, CURVE, SHAPE, then SYNC). Each press goes to the next modulator, then to the MATRIX of every route, then back to the first. It opened on the matrix before, so a modulator's own controls were a press away.
- **Changed:** shorter screen messages (flash).

## 1.1.2 — safe mode only when starts really fail

- **Fixed:** safe mode ("LAST STARTS FAILED") came up after installing. The start guard counted every start that never sat stopped for 8 s, and kept counting across installs, so starts that were played from the beginning, or that the 1.1.0 overload cut short, put the next version into safe mode. Now each version starts the count afresh, and a start counts as good after 8 s stopped or 20 s playing. Marking it good writes one byte of flash, without an erase.
- **Changed:** shorter messages on the screen (flash).

## 1.1.1 — one long capture; RESON on the FM-1

- **New: one long capture, the default.** The four capture buffers work end to end as one capture the tracks share: about 1.3 s at HI, 2.6 s at MID, 5.2 s at LO (from 0.33, 0.65 and 1.31 s). Any track on CAPTURE plays it; one records at a time. A track on THRU keeps one buffer for its ring, and the long capture gets the rest. SOURCE → BUFFER: LONG or SPLIT (a buffer each, as before; PING needs it). Kept on the device, not in projects. KEEP keeps as much of a long capture as the reel has room for.
- **Fixed: RESON overloading the FM-1.** Every float of its state is cleared once it falls under about 1e-26, so nothing sinks to a denormal (the FPU may be slow on them; the emulator doesn't show it). The CPU guard eases RESON from its first step: only the bands within 12 dB of the loudest, the band itself as the modulator; at the second step only those within 6 dB, and no droplets.
- **Faster:** RESON skips its feedback when FDBK is 0 and its droplets' ring when DROPS is 0; one draw a sample for the droplets' births; µ-law encoding reads the segment off the float's exponent (bit-exact, also for captures and the delay).
- **Changed:** some hints are shorter, and the RESON page no longer draws the source behind its bars (flash).

## 1.1.0 — RESON

**RESON: an eight-band resonant filterbank**, after Grains' bank and its liquefier. ENV toggles COLOR and RESON; the page puts it on the open track, after PUTTY and before COLOR.

- **Modes:** FILTER, VOCODE, PING, HYBRID (default).
- **Modulators:** SELF, INPUT, PARTNR, OSC (sine → saw → pulse), NOISE, OUTPUT.
- **Pings:** a transient detector (HIGH, FULL or LOW band; SENS; hysteresis, refractory time), the listener's HITS, NOTES, or REC on the page; QUANT to 1/32–1/4 of the steps or the tempo; HARD; SPECTR weights the burst to where the sound is.
- **Motion:** LIQUID (slow, correlated drift of tuning, decay and gain), FDBK, SHIFT.
- **Particles:** DENS (dust) with COHES (tied to the hits), DROPS (micrograins of the wet sound), MIGR (the grains ring other bands).
- **Profiles:** LIQ MEM (the default), GLASS, THROAT, DUST, RADIO, DROPS.
- **Per band:** GAIN, FREQ, DECAY; pads 1–8 pick the band on the BAND view.
- **The screen** follows the RESON drawings: glowing stacked bars with reflections, the band in a lavender pill, the band's curve, carrier and modulator, sparks on pings.
- **Projects:** format 7. 1.0 projects load, with RESON at its defaults (on the FM-1 and in the emulator).
- **Changed:** a capture at MID is 0.65 s (from 0.68): RAM for RESON. Some hints and messages are shorter, to fit.
- **Budgets:** app image 453.9 KB of 453.9 KB; heap 119.2 KB. CPU (fm1-emu, the FM-1's timing): RESON adds 16 % of a block as it starts and 21 % at its heaviest; the engines without it cost about 3 % more than 1.0 (helpers built for size). The guard's second step also eases RESON.

## 1.0.0 — PUTTY

**Viscera is now PUTTY: catch loops from the input and work them like putty.** The engine is new; the framework stays (four tracks, COLOR, SPACE, the modulators and matrix, scenes, macros, STEPS, CAROM, the listener, the install page). After the spirit of Synthux Spotykach, a little of Montreal Assembly's 856 for Zellersasn, a little of Bastl Thyme, Morphagene and Nebulae.

- **Tracks are A–D, in pairs: A+B and C+D.**
- **SOURCE** (EDIT): CAPTURE, REEL or THRU.
  - CAPTURE: tap REC, tap again: the gesture sets the length. Hold REC to overdub (SOS keeps the old). RATE HI, MID or LO: 0.34, 0.68 or 1.36 s.
  - INPUT: BT+USB by default, or either, another track or the mix.
  - BOUNCE: IN captures the partner's output (B does, by default); PING passes the sound back and forth between the pair.
  - REEL: SAVE + REC KEEPs a capture on the reel in flash (about 2.6 s, 16 splices); REEL plays it, splice by splice, or any bank sample.
- **Three ways to work it** — the page is the mode:
  - **HANDS** (HOME): STRETCH from 0.25× to 8× and frozen at the top, PITCH ±24 semitones, independent of each other; POS and SIZE set the window; GRAIN, JITTER, REVRS.
  - **SHARDS** (FX): up to eight glass-shard grains: SHAPE from smooth through reverse-expodec and expodec to shatter; DENS, SPRAY; TRIG FREE, HIT (the listener's hits), NOTE or CLOCK; SIZE, SPREAD, REVRS, STEREO.
  - **SPECTRA** (SEL): a phase vocoder: STRETCH, BLUR, SHIFT, CROSS (the partner's spectrum on this one's phases), GATE, TILT, HOLD, PHASE. Two tracks at a time.
- **Robots:** hold REC on a putty page and turn a knob: the gesture loops on its own around the knob. Four a track. REC + PLAY clears the capture and its robots.
- **Two hands:** hold OCT− (together) or OCT+ (apart) and turn a knob to move both tracks of the pair.
- **SEQ:** each step has SLICE, PITCH, LEN and PROB. PRESETS picks the step.
- **The screen** follows the PUTTY drawings: white on black with lavender and small flashes of red; both tracks of the pair, the shard cloud, the spectrum, the steps as blocks.
- **The emulator's** knobs and buttons look as in the drawings: matte black knobs, charcoal keys, a lavender REC that flashes red while recording.
- **Kept:** COLOR, SPACE, the modulators and matrix, PERFORM, scenes, macros, CAROM, DISSECT's listener (now driving SHARDS), MIDI learn, sounds, settings, the CPU guard.
- **Gone:** PLAY, LOOP, KEYS and THRU modes as they were, GRAIN, the filters, RESON and REBUILD.
- **Changed:** project format 6 and device settings version 3: 0.x projects and sounds don't load. The delay line is 8-bit µ-law (as the captures), which frees RAM for the vocoders.
- **Budgets:** app image 453.3 KB of 453.9 KB; heap 117.7 KB (needs 110). CPU in fm1-emu at the FM-1's timing, with two SPECTRA tracks (one crossing), dense SHARDS, HANDS and SPACE all busy: 72–85% a block (the guard steps in above 90%); one SPECTRA instead: 51–77%; HANDS and SHARDS only: 54%.

## 0.3.0

**DISSECT: take a piece of music apart and rebuild it.** GLO's second page (GLO: SPACE, DISSECT, SETTINGS). Projects saved by 0.2.x load and are converted; sounds keep working.

- **New: one listener for the whole engine.** It hears SOURCE (BT, USB, both, a track or the main mix). It runs once a block, and only while something uses it or the page is open.
  - Three bands.
  - Hits: SENS sets how easily they count; FOCUS takes them from one band.
  - The pitch (50 Hz to 1 kHz) and the levels of its first eight partials.
- **New: GRAIN TRIG.**
  - HITS: each hit plays a burst of 1 to 8 grains from where that hit fell in the buffer.
  - NOTES: keys, steps and CAROM play them.
  - SCAN picks which of the last 8 hits, SPRAY scatters among them.
  - QUANT TUNE tunes each grain so the pitch heard lands on the note.
  - REC on DISSECT locks the buffer, and its hits with it.
- **New: RESON EXCITE and FOLLOW.**
  - EXCITE: the hits strike the bank, and the track's own sound feeds it less.
  - FOLLOW: the lowest band goes to the pitch heard, to the semitone.
  - New scales HARM, MEMBR and BAR: a string's, a drum head's and a struck bar's partials.
- **New: REBUILD (FILTER).** Up to eight sine partials at the pitch heard and its partials' levels. Controls: SHIFT, SMEAR, STRTCH, TILT, DETUNE, HOLD or FADE when there's no pitch, and SNAP.
- **New: HITS on EXTRACT.**
  - BALLS: each hit throws a ball into every CAROM track at the pitch heard.
  - STEPS: hits clock the STEPS sequencers while PLAY is on.
- **New: FOLLOW modulators follow the listener.** New INPUTs: LOW, MID, HIGH, HITS, PITCH, BRIGHT.
- **CPU, after Ravel:**
  - The listener is off unless something uses it.
  - It skips the pitch while the source is silent and at the guard's second step.
  - It stops at the first clear pitch, and the search is integer work on every other frame.
  - REBUILD skips silent partials, and plays four at most at the guard's second step.
  - In fm1-emu at the FM-1's timing:
    - Nothing listening: as 0.2.6 (20% with one sample).
    - Every DISSECT device on and all tracks awake: about 55% (peak 58%).
- **RAM:** the inputs are now converted in place and the engine renders straight into the output block, which saves 2.5 KB. The screen's curve and scope are 8-bit. Heap 114.6 KB (needs 110).
- **Unchanged:** existing sounds are bit-exact on the host until a DISSECT control is used.
- **Changed:** project format 5. 0.2.x projects load (on the FM-1 and in the browser) and gain the new controls at their defaults.

## 0.2.6

CPU, after what worked on Ravel. In fm1-emu at the FM-1's measured timing (360 MHz): one sample playing 25% → 20% (peak 30% → 27%); all four tracks and SPACE busy 59% → 53% (peak 58%).

- **Changed: quiet tracks sleep even while they play.** A track that is muted, or has LEVEL and SEND both at 0, skips its chain unless another track takes it as its INPUT. Its sequencer and modulators keep running.
- **New: the CPU guard eases before it mutes, in steps as Ravel's.** Above 90% of a block two blocks running, it moves up one step:
  - EASED 1: SPACE's reverb rests (the delay keeps going).
  - EASED 2: also every other RESON band (+3 dB to make up for them) and half of the new grains.
  - Only if it's still over at EASED 2 does it rest the sound for about a second, as before.
  - Below 60% for about 3 s, it steps back.
  - Audible only while it's easing; the panel says so ("CPU HIGH: …").
- **New: GLO → SETTINGS → PANEL shows where the CPU goes:** T1–T4, SP (SPACE) and + (inputs, sequencer, master) as % of a block, and the guard's step.
- **Faster, same sound (bit-exact on the host):**
  - RESON runs band by band over each 32-frame sub-block, two bands at a time, and keeps its tuning until PITCH, SCALE, DETUNE or DECAY moves.
  - Parameter maths only clamps what a route or macro touched, and skips the rest of the work when nothing moved.
- **Fixed:** the master limiter could round a hair past full scale (the FM-1 clipped it there anyway).
- **Faster, inaudible:** a RESON band that is closed (SUSTAIN), rung out and below -100 dB after makeup is skipped.
- **Not possible on this SDK: the second core.** M-VAVE's library starts it for its own engines' render jobs and gives apps no way to run work there. Getting it would mean reverse-engineering that library or moving Viscera to Felucca (as Ravel did), which loses Bluetooth and audio input.

## 0.2.5

- **Fixed: CPU OVERLOAD and no sound on an FM-1, even with one sample.** Every track ran its whole chain (GRAIN, FILTER/RESON, COLOR) and SPACE ran on every block, sound or not: about 58% of the CPU while silent, 61% with one sample playing, in fm1-emu at the FM-1's measured timing. A real FM-1 is slower than that, so the CPU guard kept muting the sound. Now a track with nothing to play sleeps (its material and modulators still run, so a sample, a loop, the input or a note wakes it at once) and stays awake until its output has been quiet for about 1.5 s, so tails finish. COLOR's noise (a source of its own) and frozen grains keep a track awake. SPACE sleeps the same way, after its echoes and tail die away (about 4 s). In fm1-emu: silent 12%, one sample 25% (peak 30%). With all four tracks and SPACE busy it's still about 59% there.

## 0.2.4

- **Fixed: Viscera never started on an FM-1** (0.2.2 and 0.2.3: no screen, no lights, no USB). It left M-VAVE's library about 68 KB of heap, and the library's USB, Bluetooth and MIDI start-up needs more: it couldn't create its MIDI thread, gave up, and then started neither the audio nor the control task. Found by running the package in fm1-emu, where M-VAVE's own Open kit starts and 0.2.3 doesn't. The heap is now about 119 KB.
- **Changed, to make that room:** loop memory 64 → 32 KB (0.75 s HI, 1.5 s LO, shared by the tracks), grain rings 0.26 → 0.18 s per track, delay 0.28 → 0.19 s (0.37 s in LONG).
- **Fixed:** start-up no longer waits 100 ms for the safe-mode buttons (an RTOS sleep inside the library's start-up); OCT− and OCT+ are read during the first 150 ms of control passes instead.
- **New checks:** the build refuses a heap under 110 KB (M-VAVE's script asked for 48 KB). CI starts every build in fm1-emu (`tools/emu_boot.sh`), with its fast timing and the FM-1's measured timing, and fails it unless the screen and LEDs come up; 0.2.3 fails it, M-VAVE's unmodified Open kit passes.

## 0.2.3

- **Fixed (the reason 0.2.2 never started on a real FM-1):** Viscera's screen.c replaces M-VAVE's, and it never brought up the board: it didn't start the panel scanner (`fm1_controls_init`) or set up and switch on the LCD (`fm1_screen_init`). The linker dropped both, so the shipped firmware didn't contain them, and it then sent frames to a display that was never set up. Viscera now does what M-VAVE's start-up does, in the same order: panel, LEDs off, display.
- **Fixed:** safe mode (OCT- and OCT+ held at power-on) could never trigger, because the buttons were read before the scanner was running. They're read after it now.
- **New:** a start guard. Two starts in a row that never ran well (8 seconds) make the next power-on start in safe mode, where the install page can reinstall. Its count and Viscera's settings are read and written on the first control pass, not at start-up (the SDK allows flash only from the control task).
- **New:** the build refuses a firmware that doesn't contain the board start-up.
- **Fixed:** the audio interrupt called code that lives in flash, which the SDK forbids: the timer read (`fm1_device_ticks`), the parameter table, sample-bank lookups, scene recall and memory clearing, and the compiler's 64-bit division helpers. All of it now runs from RAM or is gone (the timer is read directly, positions wrap without division). Its 1 KB of stack arrays are static.
- **New:** the build runs M-VAVE's own firmware checks (ported from the SDK's Build.ps1), and checks that everything the audio interrupt can reach is in internal RAM (`tools/check_audio_ram.py`).
- **Fixed:** the blank factory voice put a letter of its name over the transpose byte.
- **Changed:** the stock-engine removal from 0.2.2 is narrowed to the engines' seven render functions. The library calls them only from its own render(), which Viscera never runs; the build checks that before changing anything and refuses otherwise. Everything else of the engines (init, notes, MIDI, parameters) keeps its code.

## 0.2.2

- **New:** the panel lights up. The open page's button and PLAY light, REC blinks while recording, the octave buttons show the octave, the pads show steps and the playhead on SEQ, scenes on ARP and tracks on HOME, and held keys light.
- **New:** BACKLIT (SETTINGS → PANEL): how the unlit keys glow, OFF, LOW, MID or HIGH.
- **New:** KNOB CC: CC 110–113 on any channel turn KNOB 1–4 of the page on screen, with pickup. It can be switched off.
- **New:** learned CCs pick up too. A controller far from the value does nothing until it passes it, so nothing jumps.
- **New:** METRO, a click on every beat with an accent on the bar, while playing.
- **New:** SETTINGS → PANEL shows the version and the CPU load, with the peak.
- **Changed:** BACKLIT, KNOB CC, METRO, the MIDI learn mappings and the sounds belong to the FM-1. They are kept through power-off and loading a project, in their own flash sector (in the emulator, in the browser).
- **Changed:** SETTINGS moved to GLO (press GLO on SPACE). SAVE is where things are kept: PROJECT, then SOUNDS.
- **New:** SETTINGS → AUDIO IN (the second view under GLO): every way sound gets in, in one place. Bluetooth audio on/off and its state, USB audio, level meters for both, and the selected track's INPUT, level and monitoring. PRESETS picks the track; REC opens its MATERIAL page. The FM-1 has no analogue input; samples are loaded over USB with M-VAVE's workbench.
- **Changed:** LFO opens the MATRIX first, then MOD 1–4. The matrix lists every route on every track, with live bars, and its knobs are SOURCE, DEVICE, PARAM and AMOUNT (REC: bipolar or unipolar). DEVICE walks Viscera's pages (each track's MATERIAL to MODS, then SPACE, PERFORM and MASTER) and PARAM follows the page's knob order. A NEW row makes a route with any knob.
- **New:** a modulator on one track can move another track's parameters, from the matrix or with LFO + a knob on MIX.
- **New:** while LFO is held, the bottom line says which modulator a knob turn maps.
- **New:** a sample manager on the install page (Samples). Over USB it reads the FM-1's eight sample slots, swaps in your audio (WAV, AIFF, FLAC or MP3, converted to the FM-1's format in the browser), renames, clears and plays them, saves and opens backups, and writes the bank back, read back and checked byte for byte. It uses M-VAVE's own sample protocol and format, and keeps banks inside Viscera's limit (about 16.5 s).
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
