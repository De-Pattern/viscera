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
# Update identity: FM-1_86 + the version as X YY ZZ ("86" is "V"; the SDK
# requires FM-1_8 plus six digits). 0.1.2 -> FM-1_8600102. The fixed part
# "FM-1_86" is Viscera's own, so firmware hubs can tell it apart.
def identity(version):
    major, minor, patch = (int(x) for x in version.split('.'))
    if major > 9 or minor > 99 or patch > 99:
        raise SystemExit('version out of range for the update identity')
    return f"FM-1_86{major}{minor:02d}{patch:02d}", f"V{minor * 10 + patch:02d}" if minor * 10 + patch < 100 else f"V{minor * 10 + patch}"

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
    ap.add_argument("--version", default=open(os.path.join(root, "VERSION")).read().strip())
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
    cfg = os.path.join(app, "viscera", "vsc_config.h")
    patch(cfg, '#ifndef VSC_VERSION\n#define VSC_VERSION "dev"\n#endif', f'#define VSC_VERSION "{a.version}"')
    # the FM-1 bridge, screen and output stage
    for f in os.listdir(os.path.join(root, "fm1", "app")):
        shutil.copy(os.path.join(root, "fm1", "app", f), os.path.join(app, f))

    # Viscera never plays DX7 voices: the 32 packed factory voices (10.5 KB of flash, 16 KB of
    # RAM once unpacked) become one blank voice. The functions stay, so the SDK's workbench
    # library code still links and answers; it just hands out the blank voice.
    with open(os.path.join(app, "fm_factory.c"), "w") as f:
        f.write("""/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Viscera build: one blank DX7 voice in place of the factory set (Viscera has no DX7 engine). */
#include "fm_patch.h"
static const uint8_t blank_voice[128] = {
    [102] = 99, [103] = 99, [104] = 99, [105] = 99, [106] = 50, [107] = 50, [108] = 50, [109] = 50,
    [118] = 'V', [119] = 'I', [120] = 'S', [121] = 'C', [122] = 'E', [123] = 'R', [124] = 'A', [125] = ' ', [126] = ' ', [127] = ' '};   /* name: bytes 118..127 */
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program) { (void)program; return blank_voice; }
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program) { groove_fm_unpack(p, groove_fm_factory_raw(program)); }
""")
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
    PRODUCT, RELEASE = identity(a.version)
    patch(os.path.join(app, "version.h"), '#define FM1_RELEASE_VERSION "V14"', f'#define FM1_RELEASE_VERSION "{RELEASE}"')
    patch(os.path.join(app, "version.h"), '#define FM1_UPDATE_PRODUCT "FM-1_8000014"', f'#define FM1_UPDATE_PRODUCT "{PRODUCT}"')
    print("tree ready:", a.out, PRODUCT, RELEASE)

if __name__ == "__main__":
    main()
