<p align="center">
  <img src="docs/art/icon.png" width="520" alt="Viscera: a tape reel spun from splattered ink">
</p>

<h1 align="center">viscera</h1>

<p align="center">A four-track sculpting sampler firmware for the M-VAVE FM-1.<br>
<a href="https://de-pattern.github.io/viscera/">Install page</a> · <a href="docs/DESIGN.md">Design</a> · <a href="docs/STATUS.md">Status</a></p>

<p align="center">
  <img src="docs/art/cycle.gif" width="520" alt="The install page animation: vapour, rain, swell, melt, settle">
</p>

Viscera brings sound in over Bluetooth (a phone or tablet) or USB (a computer), or plays it from the FM-1's flash sample bank, then takes it apart. Each track runs MATERIAL → GRAIN → FILTER → COLOR into a shared SPACE bus (delay and reverb), with four modulators per track, macros, scenes that morph, and a step sequencer with parameter locks.

It's built on M-VAVE's official FM1 Open SDK (V14), so Bluetooth, USB audio and MIDI keep working, and samples uploaded with M-VAVE's workbench still play.

**Status: on hold, early.** The audio engine is written and compiles for the host and WebAssembly. The screen and controls, the SDK glue and the first hardware build come next. Nothing runs on the FM-1 yet.

| | |
|:-:|:-:|
| <img src="docs/art/vapour.png" width="360" alt="Vapour: grains rising and curling"> | <img src="docs/art/rain.png" width="360" alt="Rain: streaks falling and pooling"> |
| vapour | rain |
| <img src="docs/art/swell.png" width="360" alt="Swell: grains riding layered waves"> | <img src="docs/art/settle.png" width="360" alt="Settle: grains gathering into the reel"> |
| swell | settle |

## Install

Open the [install page](https://de-pattern.github.io/viscera/) in Chrome or Edge on a computer, with the FM-1 on and connected by USB. It uses M-VAVE's own update protocol. Back up your projects and samples with M-VAVE's workbench first; the workbench can also put the stock firmware back.

No build is published yet.

## Licence

GPL-3.0 with M-VAVE's FM1 SDK linking exception, as the SDK's app sources require. The SEA decoder table is MIT (Dani Biro, M-VAVE).
