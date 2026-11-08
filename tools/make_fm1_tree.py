#!/usr/bin/env python3
"""Assemble a buildable FM-1 firmware tree for Viscera.

Takes M-VAVE's FM1 Open V14 kit (a checkout of github.com/m-vave/fm1_open),
copies it, and overlays Viscera:
  - core/*            -> app/viscera/
  - fm1/app/*         -> app/          (screen.c and audio_output.c replace the stock ones)
  - small, checked patches to app/board/audio.c, app/samples.c, app/midi_router.c
  - app/version.h     -> Viscera's update identity

Then build on Windows with JieLi toolchain 2.5.3, from the new tree:
  $env:FM1_PI32_TOOLCHAIN = 'C:\\JL\\pi32'
  ./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\\JL\\pi32'

Usage: python3 tools/make_fm1_tree.py [--sdk PATH] [--out build/fm1-tree]
Without --sdk the kit is cloned at the pinned commit.
"""
import argparse, os, shutil, subprocess, sys

PINNED = "4c7496daac04a95ff10d1afdcc8f9ead2dbabc41"   # fm1_open V14, 2026-10-21
PRODUCT = "FM-1_8960001"                              # Viscera's update identity
RELEASE = "V01"

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, ".."))

def patch(path, old, new, count=1):
    text = open(path, encoding="utf-8").read()
    found = text.count(old)
    if found != count:
        sys.exit(f"patch failed: expected {count} match(es) in {path}, found {found}:\n{old}")
    open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", help="existing fm1_open checkout (V14)")
    ap.add_argument("--out", default=os.path.join(root, "build", "fm1-tree"))
    a = ap.parse_args()

    sdk = a.sdk
    if not sdk:
        sdk = os.path.join(root, "build", "fm1_open")
        if not os.path.isdir(sdk):
            subprocess.check_call(["git", "clone", "https://github.com/m-vave/fm1_open", sdk])
        subprocess.check_call(["git", "-C", sdk, "checkout", "-q", PINNED])
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(sdk, a.out, ignore=shutil.ignore_patterns(".git", "build"))
    app = os.path.join(a.out, "app")

    # the portable core
    os.makedirs(os.path.join(app, "viscera"))
    for f in os.listdir(os.path.join(root, "core")):
        shutil.copy(os.path.join(root, "core", f), os.path.join(app, "viscera", f))
    # the FM-1 bridge, screen and output stage
    for f in os.listdir(os.path.join(root, "fm1", "app")):
        shutil.copy(os.path.join(root, "fm1", "app", f), os.path.join(app, f))

    # audio interrupt: render Viscera instead of the stock engine
    patch(os.path.join(app, "board", "audio.c"),
          '#include "audio_output.h"\n',
          '#include "audio_output.h"\n#include "viscera_fm1.h"\n')
    patch(os.path.join(app, "board", "audio.c"),
          "fm1_engine_render(rendered, FM1_BLOCK_FRAMES) != 0",
          "vsc_fm1_render(rendered) != 0")

    # sample bank: tell Viscera about each verified entry; keep clear of its project area
    samples = os.path.join(app, "samples.c")
    patch(samples, '#include "fm_patch.h"\n', '#include "fm_patch.h"\n#include "viscera_fm1.h"\n')
    patch(samples,
          "static void unbind(void)\n{\n",
          "static void unbind(void)\n{\n    for (unsigned i = 0; i < 16; ++i) vsc_fm1_bank_entry(i, NULL, 0, NULL);\n")
    patch(samples,
          "    fm1_engine_resume();\n    state = 2;\n    return 0;\n",
          "    fm1_engine_resume();\n"
          "    for (unsigned i = 0; i < 16; ++i)\n"
          "        vsc_fm1_bank_entry(i, header.entry[i].frames ? mapped + header.entry[i].offset : NULL,\n"
          "                           header.entry[i].frames, header.entry[i].name);\n"
          "    state = 2;\n    return 0;\n")
    patch(samples, "bytes > GROOVE_SAMPLE_LIMIT", "bytes > VSC_SAMPLE_LIMIT")

    # MIDI in: Viscera sees every message before the stock router
    midi = os.path.join(app, "midi_router.c")
    patch(midi, '#include "instrument.h"\n', '#include "instrument.h"\n#include "viscera_fm1.h"\n')
    patch(midi,
          "void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)\n{\n",
          "void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)\n{\n    vsc_fm1_midi(source, m, n);\n")

    # identity
    patch(os.path.join(app, "version.h"), '#define FM1_RELEASE_VERSION "V14"', f'#define FM1_RELEASE_VERSION "{RELEASE}"')
    patch(os.path.join(app, "version.h"), '#define FM1_UPDATE_PRODUCT "FM-1_8000014"', f'#define FM1_UPDATE_PRODUCT "{PRODUCT}"')
    print("tree ready:", a.out)

if __name__ == "__main__":
    main()
