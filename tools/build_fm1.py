#!/usr/bin/env python3
"""Build Viscera's FM-1 firmware on Linux.

A port of the firmware path of M-VAVE's sdk/Build.ps1 (FM1 Open V14) to JieLi's
Linux toolchain (the same PI32v2 clang 4.0.1), so CI can build without Windows:

  1. assemble the tree (tools/make_fm1_tree.py)
  2. compile every app source with the SDK's flags
  3. write the build identity, link against the SDK's frozen library
  4. check the image: entry point, sections, runtime heap for Bluetooth
  5. package the .fwsc (tools/fm1_package.py, a port of sdk/Package.cs)

Usage:
  JIELI_TOOLCHAIN=~/.jieli/toolchain python3 tools/build_fm1.py [--sdk PATH] [--version X.Y.Z]
Get the toolchain with:  sh tools/get_toolchain.sh ~/.jieli
Outputs build/viscera.fwsc, build/latest.json and build/fm1-report.json.
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..'))
sys.path.insert(0, here)
import fm1_package
import make_fm1_tree

USER_BYTES, FAMILY = 536576, 7
# The heap M-VAVE's library starts USB, Bluetooth and MIDI from. M-VAVE's script asks for 48 KB (Bluetooth's
# pools), but its IO start-up needs much more: with 68 KB (0.2.2, 0.2.3) it can't create its MIDI thread, and
# the FM-1 never starts. In fm1-emu it starts from about 100 KB (and not at 85 KB); 110 KB leaves room for
# what Bluetooth takes when something connects. tools/emu_boot.sh checks the start itself.
MIN_HEAP = 112640

def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        raise SystemExit(f'{os.path.basename(cmd[0])} failed ({r.returncode})')
    return r.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sdk', help='existing fm1_open V14 checkout (cloned at the pinned commit if absent)')
    ap.add_argument('--version', default=open(os.path.join(root, 'VERSION')).read().strip())
    ap.add_argument('--keep-engines', action='store_true',
                    help="link M-VAVE's library unchanged (the engines' render functions stay; the app no longer fits)")
    a = ap.parse_args()
    tc = os.environ.get('JIELI_TOOLCHAIN', os.path.expanduser('~/.jieli/toolchain'))
    bin_ = os.path.join(tc, 'pi32v2', 'bin')
    if not os.path.exists(os.path.join(bin_, 'clang')):
        raise SystemExit('JieLi Linux toolchain not found: run  sh tools/get_toolchain.sh ~/.jieli')

    build = os.path.join(root, 'build')
    tree = os.path.join(build, 'fm1-tree')
    sys.argv = ['make_fm1_tree.py', '--out', tree, '--version', a.version] + (['--sdk', a.sdk] if a.sdk else [])
    make_fm1_tree.main()
    sdk = os.path.join(tree, 'sdk')
    out = os.path.join(tree, 'build', 'viscera')
    os.makedirs(os.path.join(out, 'obj'), exist_ok=True)
    os.makedirs(os.path.join(out, 'generated'), exist_ok=True)

    version_h = open(os.path.join(tree, 'app', 'version.h')).read()
    product = re.search(r'#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"', version_h).group(1)

    include = ['sdk/include', 'sdk/include/libc', 'app/synth', 'sdk/config', 'app', 'app/board']
    flags = ['-target', 'pi32v2', '-integrated-as', '-mcpu=r3', '-mfprev1', '-Wuninitialized',
             '-Wno-invalid-noreturn', '-fno-common', '-flto', '-fallow-pointer-null', '-fprefer-gnu-section',
             '-femulated-tls', '-Wno-shift-negative-value', '-Wframe-larger-than=2560', '-fms-extensions',
             '-fno-unwind-tables', '-ffunction-sections', '-fdata-sections', '-fmessage-length=0',
             '-cl-single-precision-constant', '-mllvm', '-pi32v2-large-program=true', '-D__GCC_PI32V2__', '-O2']
    flags += ['-I' + os.path.join(tree, d) for d in include]

    sources = []
    for dirpath, _, files in os.walk(os.path.join(tree, 'app')):
        if os.path.relpath(dirpath, tree).replace('\\', '/').startswith('app/synth'):
            continue
        for f in files:
            if f.endswith(('.c', '.S')):
                sources.append(os.path.relpath(os.path.join(dirpath, f), tree).replace('\\', '/'))
    sources.sort()

    small = {'app/project.c', 'app/transfer.c', 'app/samples.c', 'app/screen.c', 'app/presets.c',
             'app/midi_router.c', 'app/fm_factory.c', 'app/fm_library.c',
             # Viscera's screen, parameter table and MIDI parsing run in the control task: size over speed
             'app/viscera/vsc_ui.c', 'app/viscera/vsc_params.c', 'app/viscera/vsc_midi.c'}
    def compile_one(i_src):
        i, src = i_src
        obj = os.path.join(out, 'obj', f'{i}_' + re.sub(r'[^a-zA-Z0-9._-]', '_', src) + '.obj')
        opts = list(flags)
        if src in ('app/screen.c', 'app/transfer.c', 'app/midi_router.c'):
            opts.append('-fno-lto')
        if src == 'app/board/panel.c':
            opts += ['-fno-lto', '-fno-pic']
        opts += ['-fwrapv', '-fno-fast-math', '-ffp-contract=off']
        if src in small:
            opts.append('-Oz')
        if src == 'app/screen.c':
            opts.append('-fno-inline-functions')
        if src == 'app/viscera/vsc_engine.c':
            opts += ['-fno-lto', '-fno-unroll-loops']
        run([os.path.join(bin_, 'clang')] + opts + ['-c', os.path.join(tree, src), '-o', obj])
        return obj
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objects = list(ex.map(compile_one, enumerate(sources)))
    print(f'compiled {len(objects)} sources')

    # build identity: profile, SDK manifest and every app file
    lines = ['viscera', sha256(os.path.join(sdk, 'manifest.json'))]
    for dirpath, _, files in sorted(os.walk(os.path.join(tree, 'app'))):
        for f in sorted(files):
            p = os.path.join(dirpath, f)
            lines.append(os.path.relpath(p, tree).replace('\\', '/') + ':' + sha256(p))
    build_id = hashlib.sha256('\n'.join(lines).encode()).hexdigest()
    ident = os.path.join(out, 'generated', 'build_identity.c')
    open(ident, 'w').write(
        '#include "layout.h"\n'
        f'const struct fm1_firmware_layout fm1_layout = {{{USER_BYTES}U,{FAMILY}U}};\n'
        'const unsigned char fm1_build_id[32] = {' + ','.join('0x' + build_id[i:i + 2] for i in range(0, 64, 2)) + '};\n'
        f'const unsigned char fm1_update_identity[27] = "{product}";\n')
    ident_obj = os.path.join(out, 'obj', 'build_identity.obj')
    run([os.path.join(bin_, 'clang')] + flags + ['-c', ident, '-o', ident_obj])
    objects.append(ident_obj)

    linker = open(os.path.join(sdk, 'config', 'linker.ld.in')).read()
    linker = linker.replace('@SBC_DECODER@', 'sbc_decoder\n').replace('@ACL_TX_EXTRA@', ' + 1024').replace('@ACL_RX_EXTRA@', ' + 4096')
    open(os.path.join(out, 'generated', 'linker'), 'w').write(linker)
    elf = os.path.join(out, 'sdk.elf')
    # the stock engines' render functions stay out (tools/strip_engines.py): the library
    # calls them only from its own render path, which Viscera never runs
    engine_lib = os.path.join(sdk, 'lib', 'libfm1_engine.a')
    if not a.keep_engines:
        engine_lib = os.path.join(out, 'libfm1_engine_viscera.a')
        run([sys.executable, os.path.join(here, 'strip_engines.py'), os.path.join(sdk, 'lib', 'libfm1_engine.a'), engine_lib,
             '--toolchain', bin_, '--report', os.path.join(out, 'engines.json')])
    link = ['--plugin', os.path.join(bin_, 'LLVMgold.so'), '-o', elf] + objects + [
        '-flto', '--plugin-opt=-pi32v2-always-use-itblock=false', '--plugin-opt=-enable-ipra=true',
        '--plugin-opt=-pi32v2-merge-max-offset=4096', '--plugin-opt=-pi32v2-enable-simd=true', '--plugin-opt=mcpu=r3',
        '--plugin-opt=-global-merge-on-const', '--plugin-opt=-inline-threshold=5', '--plugin-opt=-inline-max-allocated-size=32',
        '--plugin-opt=-inline-normal-into-special-section=true', '--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar',
        '--plugin-opt=-pi32v2-enable-rep-memop', '--plugin-opt=-emulated-tls', '--sort-common',
        '--plugin-opt=-pi32v2-large-program=true', '--plugin-opt=-pi32v2-csync-before-rti=true', '--start-group',
        '--whole-archive', engine_lib, '--no-whole-archive',
        os.path.join(sdk, 'lib', 'libc.a'), os.path.join(sdk, 'lib', 'libcompiler_rt.a'),
        '--end-group', '--gc-sections', '-T' + os.path.join(out, 'generated', 'linker'), '-M=' + os.path.join(out, 'sdk.map'),
        '--undefined=connectivity_wl82_bluetooth_start', '--undefined=audio_app_bluetooth_open',
        '--undefined=bt_get_local_name', '--undefined=bt_get_mac_addr']
    run([os.path.join(bin_, 'ld')] + link, cwd=out)
    print('linked')

    # checks
    raw = open(elf, 'rb').read()
    if struct.unpack_from('<I', raw, 24)[0] != 0x02000120:
        raise SystemExit('entry point does not match the FM1 firmware entry')
    syms = {}
    for line in run([os.path.join(bin_, 'nm'), '--print-size', elf]).splitlines():
        m = re.match(r'([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?(\w)\s+(\S+)', line)
        if m:
            syms[m.group(4)] = (int(m.group(1), 16), int(m.group(2), 16) if m.group(2) else None, m.group(3))
    heap = None
    if 'HEAP_BEGIN' in syms and 'HEAP_END' in syms:
        heap = syms['HEAP_END'][0] - syms['HEAP_BEGIN'][0]
    if heap is not None and heap < MIN_HEAP:
        raise SystemExit(f'runtime heap {heap} B is below the {MIN_HEAP} B that USB, Bluetooth and MIDI start-up need: shrink buffers in core/vsc_config.h')
    # audio code must be in RAM: the render path's symbols should sit outside flash (0x02xxxxxx)
    flash_audio = [n for n in ('vsc_render', 'vsc_fm1_render', 'vsc_bank_chunk', 'fm1_output_mix')
                   if n in syms and (syms[n][0] >> 24) == 0x02]
    if flash_audio:
        raise SystemExit('audio functions ended up in flash: ' + ', '.join(flash_audio))
    # the board must be brought up: the panel scanner and the LCD. 0.2.2 shipped without
    # either (screen.c never called them, so the linker dropped them) and never started.
    missing = [n for n in ('fm1_controls_init', 'fm1_screen_init', 'fm1_device_start', 'groove_screen_init')
               if n not in syms]
    if missing:
        raise SystemExit('board start-up missing from the firmware (nothing calls it): ' + ', '.join(missing))

    # M-VAVE's own firmware checks (Build.ps1, Test-Firmware)
    print(run([sys.executable, os.path.join(here, 'check_firmware.py'), elf, os.path.join(out, 'sdk.map'),
               os.path.join(sdk, 'synth.json'), '--toolchain', bin_]).strip())
    # nothing the audio interrupt reaches may be in flash (the SDK's rule)
    print(run([sys.executable, os.path.join(here, 'check_audio_ram.py'), tree, elf, '--toolchain', bin_,
               '--flags', json.dumps([f for f in flags if f != '-flto'] + ['-fwrapv', '-fno-fast-math', '-ffp-contract=off'])]).strip())

    # package
    gen = os.path.join(out, 'generated')
    image = b''
    for section in ('text', 'data', 'ram0_data', 'cache_ram_data'):
        binf = os.path.join(gen, section + '.bin')
        run([os.path.join(bin_, 'objcopy'), '-O', 'binary', '-j', '.' + section, elf, binf])
        image += open(binf, 'rb').read()
        os.remove(binf)
    fwsc = os.path.join(build, 'viscera.fwsc')
    limit = fm1_package.FLASH_LIMIT - fm1_package.USER_SIZE - fm1_package.BT_SIZE - fm1_package.MINIMUM_VM - fm1_package.AREA - fm1_package.APP_DATA - 383 - 64
    if len(image) > limit and not os.environ.get('VSC_ALLOW_BIG'):
        sys.exit(f'app image {len(image)} bytes is over the {limit} the FM-1 layout leaves: {len(image) - limit} bytes too many')
    layout = fm1_package.create(image, os.path.join(sdk, 'tools', 'pack'), fwsc, USER_BYTES, product)
    meta = {'schema': 1, 'kind': 'open', 'product': product, 'build_id': build_id, 'family': FAMILY,
            'bytes': os.path.getsize(fwsc), 'sha256': sha256(fwsc),
            'loader_sha256': sha256(os.path.join(sdk, 'tools', 'pack', 'ota.bin')),
            'url': f'firmware/viscera-{a.version}.fwsc', 'version': a.version}
    json.dump(meta, open(os.path.join(build, 'latest.json'), 'w'), indent=1)
    sizes = {}
    for line in run([os.path.join(bin_, 'objdump'), '-h', elf]).splitlines():
        m = re.match(r'\s*\d+\s+(\.\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            sizes[m.group(1)] = int(m.group(2), 16)
    report = {'product': product, 'version': a.version, 'build_id': build_id, 'runtime_heap_bytes': heap,
              'minimum_runtime_heap_bytes': MIN_HEAP, 'sections': sizes, 'layout': layout}
    json.dump(report, open(os.path.join(build, 'fm1-report.json'), 'w'), indent=1)
    print(json.dumps({'fwsc': fwsc, 'bytes': meta['bytes'], 'heap': heap, 'app_bytes': layout['app_bytes'],
                      'app_limit': layout['app_limit'], 'build_id': build_id[:16]}, indent=1))

if __name__ == '__main__':
    main()
