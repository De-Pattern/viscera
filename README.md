<p align="center">
  <img src="docs/art/icon.png" width="520" alt="Viscera: a tape reel spun from splattered ink">
</p>

> **Being tested on hardware.** The [install page](https://de-pattern.github.io/viscera/) offers the latest build for hardware testing, installed from M-VAVE's stock V15 (it refuses to install from Ravel or other Felucca-based firmware). Back up first. If the FM-1 stays dark afterwards, [Ravel's install page](https://de-pattern.github.io/Ravel_FM1/install/) "Back to the stock firmware" with a V15 file puts M-VAVE's firmware back. The `.fwsc` files on the releases page are older builds: please don't flash those. You can also try Viscera in the [browser emulator](https://de-pattern.github.io/viscera/emulator/).

<h1 align="center">viscera</h1>

<p align="center">A four-track sculpting sampler firmware for the M-VAVE FM-1.<br>
<a href="https://de-pattern.github.io/viscera/">Install page</a> · <a href="docs/DESIGN.md">Design</a> · <a href="docs/STATUS.md">Status</a></p>

<p align="center">
  <img src="docs/art/cycle.gif" width="520" alt="The install page animation: vapour, rain, swell, melt, settle">
</p>

Viscera brings sound in over Bluetooth (a phone or tablet) or USB (a computer), or plays it from the FM-1's flash sample bank, then takes it apart. Each track runs MATERIAL → GRAIN → FILTER → COLOR into a shared SPACE bus (delay and reverb), with four modulators per track, macros, scenes that morph, and a step sequencer with parameter locks. DISSECT listens to the music coming in (its bands, hits and pitch) and lets it drive the devices: hits start grains and strike RESON, the pitch tunes RESON, REBUILD resynthesises it as partials, and hits can throw CAROM balls or clock the steps.

It's built on M-VAVE's official FM1 Open SDK (V14), so Bluetooth, USB audio and MIDI keep working, and samples uploaded with M-VAVE's workbench still play.

**Status: early, untested on hardware.** The firmware builds, packages and passes M-VAVE's updater checks; it hasn't run on an FM-1 yet. Two sequencers per track: STEPS, and CAROM, where notes bounce around a box and play when they hit the walls.

| | |
|:-:|:-:|
| <img src="docs/art/vapour.png" width="360" alt="Vapour: grains rising and curling"> | <img src="docs/art/rain.png" width="360" alt="Rain: streaks falling and pooling"> |
| vapour | rain |
| <img src="docs/art/swell.png" width="360" alt="Swell: grains riding layered waves"> | <img src="docs/art/settle.png" width="360" alt="Settle: grains gathering into the reel"> |
| swell | settle |

## Try it in the browser

The [emulator](https://de-pattern.github.io/viscera/emulator/) runs the firmware's own C core, compiled to WebAssembly: the engine, the screen and the controls, with an audio file or your microphone standing in for Bluetooth and USB audio in.

## Install

Open the [install page](https://de-pattern.github.io/viscera/) in Chrome or Edge on a computer, with the FM-1 on and connected by USB. It uses M-VAVE's own update protocol. Back up your projects and samples with M-VAVE's workbench first; the workbench can also put the stock firmware back.

Each release is built in CI and goes straight to the install page. See [Releases](https://github.com/De-Pattern/viscera/releases) for the `.fwsc` files and [CHANGELOG.md](CHANGELOG.md) for what changed.

## Licence

GPL-3.0 with M-VAVE's FM1 SDK linking exception, as the SDK's app sources require. The SEA decoder table is MIT (Dani Biro, M-VAVE).
