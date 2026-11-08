# Status and next steps

Paused 2026-11-08.

## Done

- `docs/DESIGN.md`: the instrument, controls, memory budget and storage plan.
- `core/vsc_config.h`, `core/vsc.h`: sizes, parameter IDs, project format, platform and UI interfaces.
- `core/vsc_params.c`: parameter table and labels, defaults, scenes, mod and macro routing, CRC.
- `core/vsc_engine.c`: the whole audio engine — PLAY / LOOP / KEYS / THRU material, grains, SVF and PEAKS filter, colour, delay and reverb bus, four modulators, macros, scene morph, parameter locks, sequencer with swing and MIDI clock, event queues, meters. Compiles warning-free for the host and wasm32 and renders on the host; not listened to yet.

## SDK findings (fm1_open V14, commit 4c7496d)

- Inputs: `fm1_bluetooth_audio_read` (16-bit) and `fm1_usb_audio_read` (Q23).
- The audio ISR lives in the app (`app/board/audio.c`); swap `fm1_engine_render` for `vsc_render` there, and rework `app/audio_output.c` so input feeds the engine instead of the output mix.
- The library needs these app hooks: `groove_screen_init/control/mmc` (replace `screen.c`), plus the stock `project.c`, `samples.c`, `transfer.c`, `midi_router.c`, `fm_library.c` (keep them so the workbench still uploads sample banks).
- Tap MIDI in `fm1_app_midi_received` (midi_router.c) and forward to `vsc_midi_event`.
- Sample bank: 512-byte header ("FM1S"), 16 entries, 32 kHz, SEA5 chunks of 228 bytes / 320 frames, mapped with `fm1_flash_map`; decode with `fm1_sea_decode` and `sample_dqt` from `web/synth/sample_metadata.inc` (MIT).
- Audio code and tables must be in RAM (`.volatile_ram_code`); `VSC_HOT` does this. The compiler has an FPU (`-mfprev1`), single precision only.
- Builds need Windows and JieLi toolchain 2.5.3; `Build.ps1` compiles everything under `app/` except `app/synth`.

## Next

1. `core/vsc_ui.c`: band-painted screen (240×240 in 16-row bands), pages and controls per DESIGN.md.
2. `fm1/app/`: SDK glue (screen.c replacement, audio.c/audio_output.c changes, bank chunk cache, project storage in the top 64 KB of the sample area, MIDI tap) and a script to overlay it on fm1_open.
3. `web/`: wasm build and browser emulator (AudioWorklet, mic or file as BT/USB input, WAV drop-in for the bank), published on GitHub Pages.
4. First hardware build: read the link map's heap, then size the buffers in `vsc_config.h`.
