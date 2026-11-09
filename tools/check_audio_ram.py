#!/usr/bin/env python3
"""Check that nothing the audio interrupt can reach lives in flash.

The FM1 SDK requires the audio interrupt and every function it calls to be in
internal RAM (fm1.h: "Audio code and every callee must be safe during Flash
programming"). Viscera's render entry is vsc_fm1_render (and fm1_output_mix).
This compiles Viscera's sources once more without LTO and with one section per
function, follows calls (relocations) from those entries, and fails if any
function reached is ours but not placed in .volatile_ram_code, or is someone
else's (SDK, libc, compiler runtime) and sits outside internal RAM in the
linked firmware.

  python3 tools/check_audio_ram.py TREE ELF --toolchain BIN [--flags-json F]
"""
import argparse, json, os, re, subprocess, sys, tempfile, collections

SRAM = (0x01c00000, 0x01c80000)
ENTRIES = ('vsc_fm1_render', 'fm1_output_mix')
# libc's memcpy/memset are in flash; M-VAVE's own audio path calls them too (the SDK
# suspends the audio interrupt while it programs flash), so they're accepted.
ALLOWED = {"memcpy", "memset"}
SOURCES = ('app/viscera_fm1.c', 'app/audio_output.c', 'app/viscera/vsc_engine.c', 'app/viscera/vsc_params.c',
           'app/viscera/vsc_midi.c')

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        raise SystemExit('command failed')
    return r.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tree'); ap.add_argument('elf')
    ap.add_argument('--toolchain', required=True)
    ap.add_argument('--flags', required=True, help='JSON list of the compile flags the build uses')
    a = ap.parse_args()
    flags = [f for f in json.loads(a.flags) if f != '-flto'] + ['-fno-lto', '-ffunction-sections', '-fdata-sections']
    # where every symbol ended up in the firmware
    addr = {}
    for line in run([os.path.join(a.toolchain, 'nm'), a.elf]).splitlines():
        p = line.split()
        if len(p) == 3:
            addr.setdefault(p[2], int(p[0], 16))
    # our functions: which section each is in, and what each section refers to
    section_of, refs = {}, collections.defaultdict(set)
    with tempfile.TemporaryDirectory() as tmp:
        for src in SOURCES:
            obj = os.path.join(tmp, os.path.basename(src) + '.o')
            run([os.path.join(a.toolchain, 'clang')] + flags + ['-c', os.path.join(a.tree, src), '-o', obj])
            secs = {}
            for line in run(['readelf', '-SW', obj]).splitlines():
                m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)', line)
                if m: secs[m.group(1)] = m.group(2)
            local = {}
            for line in run(['readelf', '-sW', obj]).splitlines():
                p = line.split()
                if len(p) >= 8 and p[0].endswith(':') and p[3] == 'FUNC' and p[6].isdigit():
                    local[p[7]] = secs[p[6]]
            for name, sec in local.items():
                section_of[name] = sec
            cur = None
            for line in run(['readelf', '-rW', obj]).splitlines():
                m = re.match(r"Relocation section '\.rela(\S+)'", line)
                if m: cur = m.group(1); continue
                p = line.split()
                if cur and len(p) >= 5 and re.match(r'^[0-9a-f]{8}$', p[0]):
                    name = p[-3] if p[-2] == '+' else p[-1]
                    refs[cur].add(name)
    # walk: from a function to its section's references, functions only
    bad, seen, queue = [], set(ENTRIES), list(ENTRIES)
    while queue:
        f = queue.pop()
        if f in section_of:
            sec = section_of[f]
            if not sec.startswith('.volatile_ram_code'):
                bad.append(f'{f}: ours, in {sec} (flash); mark it VSC_HOT / FM1_AUDIO_CODE')
                continue
            for r in refs.get(sec, ()):
                if r not in seen and (r in section_of or (r in addr and not r.startswith('.'))):
                    seen.add(r); queue.append(r)
        elif f in addr and f not in ALLOWED:
            if not (SRAM[0] <= addr[f] < SRAM[1]):
                # data in flash (const tables) also counts: reading it while flash is programmed is unsafe
                bad.append(f'{f}: at {addr[f]:#x}, outside internal RAM')
    reached = sorted(seen)
    if bad:
        print('the audio interrupt reaches flash:\n  ' + '\n  '.join(sorted(set(bad))))
        raise SystemExit(1)
    print(f'audio path: {len(reached)} functions and tables, all in internal RAM')

if __name__ == '__main__':
    main()
