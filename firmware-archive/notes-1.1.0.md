> On the FM-1 this version overloads the CPU when RESON is on (clicks even at zero volume). Use 1.1.1 or later.


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


Install it from https://de-pattern.github.io/viscera/ (Chrome or Edge, FM-1 on USB). The guide: https://de-pattern.github.io/viscera/manual.html
