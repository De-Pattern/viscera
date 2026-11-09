# Status and next steps

## Where it stands (0.1.0, 2026-11-08)

The firmware builds and packages on Linux, and the package passes M-VAVE's own updater checks. **It has not run on an FM-1 yet.**

- `core/`: the engine (`vsc_engine.c`), parameters and scenes (`vsc_params.c`) and the screen and controls (`vsc_ui.c`). Portable C99, single-precision float, no C library in the audio path.
- `fm1/app/`: the SDK glue. `screen.c` replaces the stock screen and runs the UI; `viscera_fm1.c` renders in the audio interrupt, reads Bluetooth and USB audio in, decodes the flash sample bank (SEA5) into a small cache, takes MIDI and stores projects; `audio_output.c` applies MASTER.
- `tools/strip_engines.py`: leaves M-VAVE's stock sound engines (DX7, 303, PSG, 808, pluck, sampler) out of the firmware. They live in the SDK's frozen library, whose own audio code still calls them, so the build rewrites a copy of the library so their functions point at a two-instruction `return 0`; the linker then drops their code, tables and state. Viscera never runs the stock render path, so the stub is never reached in practice. Functions that return a pointer keep their own code. This frees about 36 KB of flash and 35 KB of RAM.
- `tools/make_fm1_tree.py` overlays all of that on M-VAVE's FM1 Open V14 kit (pinned commit), with small checked patches to `board/audio.c`, `samples.c`, `midi_router.c` and `version.h`.
- `tools/build_fm1.py` is a Linux port of the firmware path of M-VAVE's `Build.ps1`, using JieLi's Linux toolchain (the same PI32v2 clang 4.0.1); `tools/fm1_package.py` ports `Package.cs`. `tools/verify_package.mjs` runs the result through `WebUpdate.js`'s `inspectPackage` and `prepare`.

Budget at 0.1.0: app image 443,528 of 453,921 bytes; Bluetooth heap 54,924 bytes (needs 49,152).

## Identity and build ID

The update identity is `FM-1_86` followed by the version as X YY ZZ: 0.1.2 reports `FM-1_8600102` ("86" is "V"; M-VAVE's SDK requires `FM-1_8` plus six digits). The build ID is a SHA-256 over the SDK manifest and every app file; the FM-1 reports it after an install, and the install page shows it next to the package's build ID after connecting. CI and local builds of the same commit give the same build ID.

## Releasing

Bump `VERSION`, add a `## X.Y.Z` section to `CHANGELOG.md`, push. The `Build, release and deploy` workflow builds the firmware, checks it, creates the `vX.Y.Z` release with the `.fwsc`, and deploys the install page with that build behind "Use latest build". Pushes without a new VERSION still rebuild and redeploy the page.

## Installing over other firmware

Felucca-based firmwares (Ravel, SLOOP and others) accept only packages whose 16 KB boot head equals the one already on the FM-1, which is still stock V15's (they never rewrite it). Viscera's package, built on M-VAVE's V14 Open SDK, carries a different head, so those firmwares refuse it (Felucca's `ota.c`, error -5). The install page therefore installs M-VAVE's V15 first (bundled under `site/firmware/stock/`, Apache-2.0) and then Viscera, through V15's own updater. From stock V15 it installs directly through V15's updater; from an Open-family firmware it installs directly.

`tools/emulator/` simulates an FM-1 for this: `fm1_device.js` replaces Web MIDI with a device in a Web Worker (its own clock, a strict per-block deadline, Felucca's head check, stock V15's voice-bank restore), and `flash_test.py` installs through the real page in headless Chrome from each starting firmware, checking reply times, every written byte and the build ID afterwards. CI runs it on every build.

Safe mode: hold OCT- and OCT+ while switching on. Viscera starts with sound and controls off; the update protocol still answers, so the page can always reinstall. A CPU guard rests the engine for about a second if rendering takes over 90 % of a block three times running.

## First hardware test

1. Back up projects and samples with M-VAVE's workbench.
2. Install from the page (tick "Switching from…" when coming from stock or another firmware).
3. Check: the screen draws; keys play T1; the MASTER knob works; Bluetooth audio from a phone reaches a THRU track; USB audio from a computer does too; saving a project while stopped; the sample bank plays on a PLAY track.
4. If the sound crackles, see "If the FM-1 runs out of CPU" below.

## Next

- Browser emulator (wasm build of `core/` with the same UI).
- PLAY material overview: decode the bank into a waveform picture in the control task.
- Project names, copy and paste of steps, a CPU meter on the SETTINGS page (`vsc_fm1_cpu` is ready).

## If the FM-1 runs out of CPU

Only act on these once the unit shows overloads (underruns, glitches). They come from what worked on Ravel; measure on hardware first, then apply them in this order. Keep the sound bit-exact unless a change says otherwise, and record each trade in `docs/cpu-tradeoffs.md`.

1. **Audio slack block and a guard.** One extra block of buffering absorbs single-block peaks (about +3 ms latency); a guard that eases grains and KEYS voices when render time stays above 90 % for two blocks.
2. **Skip silent work.** Muted tracks stop computing; the SPACE bus rests after about 70 ms of silence; GRAIN skips when MIX and FDBK are both 0 (already done); COLOR skips when every stage is at 0 (already done).
3. **Control rate where it's inaudible.** Mods, parameter maths and filter coefficients already run once per 32 frames; slow LFOs could run once per block.
4. **Cheaper SPACE.** Process the reverb at half rate and at 12 bits, and step the combs two at a time (Ravel: −18 %, about −55 dB change; needs approval because it isn't bit-exact).
5. **Grain caps.** Lower `VSC_GRAINS` under load, or cap active grains with a level correction (Ravel's Beads port needed about +3 dB at 16 grains).
6. **Placement.** Hot loops in RAM (`VSC_HOT`), small helpers always inlined, nothing in the audio path that calls into flash or the C library. Watch the RAM budget: every byte of RAM code costs heap that Bluetooth needs.
7. **Second core.** The SDK doesn't expose the second core to apps, so this isn't available yet; check new SDK releases before planning on it.

Notes: the pi32v2 FPU has a hardware divide but no square root. Host instruction counts only rank ideas; the FM-1's cycles differ.

## Flasher page

`site/` is the install page (GitHub Pages). It uses M-VAVE's updater (`site/vendor/WebUpdate.js`, `I18n.js`, copied unchanged from fm1_open V14) for the protocol and its safety checks.

- **One-time setup:** in the repo's Settings → Pages, set Source to **GitHub Actions**. The `Build, release and deploy` workflow then publishes `site/` (with the latest firmware) on every push. If Source is left on "Deploy from a branch", the root `index.html` sends visitors on to `site/`, and `.nojekyll` stops GitHub rendering the README as the home page.
- **Publishing a build** is automatic (see Releasing). The workflow writes `site/firmware/viscera-X.Y.Z.fwsc` and `site/firmware/latest.json`:
  `{"schema":1,"kind":"open","product":"FM-1_8xxxxxx","build_id":"<sha256 of the .fwsc>","family":7,"bytes":N,"sha256":"<sha256>","loader_sha256":"<sha256 of the package's loader>","url":"firmware/viscera-X.Y.Z.fwsc","version":"X.Y.Z"}`.
- The logo is the artwork in `docs/art/logo-source.png`. `tools/make_logo.py` makes its paper transparent and writes the site and README sizes, plus `site/mark.js`, the points the hero animation gathers into the reel. The splatter reel in `docs/art/icon-source.png` is the tab icon, the README image and the link preview (`site/social.png`); the same script builds those.
- **GitHub's repo preview** is set by hand: Settings → General → Social preview → upload `docs/art/social-preview.png`.
