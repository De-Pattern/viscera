# Viscera

A four-track sculpting sampler firmware for the M-VAVE FM-1, built on M-VAVE's official FM1 Open SDK (V14), so Bluetooth and USB keep working.

Live sound comes in over Bluetooth (a phone or tablet) or USB (a computer), or from the FM-1's flash sample bank. Each track runs it through MATERIAL → GRAIN → FILTER → COLOR, with a shared SPACE bus (delay and reverb), four modulators per track, macros, scenes with morphing, and a step sequencer with parameter locks.

**Status: on hold, early.** The audio engine is written and compiles for the host and WebAssembly. The screen and controls, the SDK glue and the browser emulator aren't written yet. Nothing runs on the FM-1 yet. See [docs/STATUS.md](docs/STATUS.md) and [docs/DESIGN.md](docs/DESIGN.md).

## Licence

GPL-3.0 with M-VAVE's FM1 SDK linking exception, as the SDK's app sources require.
