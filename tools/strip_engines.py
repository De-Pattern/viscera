#!/usr/bin/env python3
"""Leave M-VAVE's stock sound engines out of Viscera's firmware.

The FM1 SDK's frozen library (sdk/lib/libfm1_engine.a, one object) carries the
stock instrument: the DX7, 303, PSG, 808, pluck and sampler engines. Viscera
never plays them (board/audio.c calls vsc_fm1_render instead of the library's
fm1_engine_render), but the library's own audio code still calls them, so the
linker keeps them: about 26 KB of RAM code, plus their tables and state.

This rewrites a copy of the library object so that every engine function symbol
points at one tiny function that returns 0. Nothing that stays in the firmware
can reach the engines' code any more, and --gc-sections drops it with
everything only it used. The stub is only ever reached from the stock engine's
render and note paths, which Viscera doesn't run; if anything did call it, it
returns 0 and does nothing.

  python3 tools/strip_engines.py LIB.a OUT.a --toolchain BIN [--report OUT.json]

Steps:
  1. take the one object out of the archive
  2. compile the stub, and put its code in the smallest engine function's section
     (that section's relocations go: it holds only the stub now)
  3. point every function symbol of the engines' sections at it (offset 0)
  4. archive the result
"""
import argparse, json, os, re, struct, subprocess, sys, tempfile

ENGINE = re.compile(r'^\.(volatile_ram_code\.fm1_|text\.)(synth_dx7|dx7|bass303|bass_|psg|PSG|drum808|pluck|sampledrum|fm1_dx7)', re.I)
# Functions that hand back a pointer (parameter tables, names, the DX7 instance) keep
# their own code: a caller may read through what they return, and 0 would not do.
KEEP = re.compile(r'(_param|_name|_label|_create)$')

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout.decode(errors='replace') + r.stderr.decode(errors='replace'))
        raise SystemExit(f'{os.path.basename(cmd[0])} failed')
    return r.stdout

class Elf:
    """Just enough ELF32 little-endian to read the section and symbol tables."""
    def __init__(self, data):
        self.d = bytearray(data)
        if self.d[:4] != b'\x7fELF' or self.d[4] != 1 or self.d[5] != 1:
            raise SystemExit('expected a 32-bit little-endian ELF object')
        shoff, = struct.unpack_from('<I', self.d, 32)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', self.d, 46)
        self.sh = [struct.unpack_from('<IIIIIIIIII', self.d, shoff + i * shentsize) for i in range(shnum)]
        strtab = self.sh[shstrndx]
        self.names = [self.cstr(strtab[4] + s[0]) for s in self.sh]
    def cstr(self, at):
        end = self.d.index(0, at)
        return self.d[at:end].decode()
    def index(self, name):
        return self.names.index(name)
    def symbols(self):
        symtab = next(i for i, s in enumerate(self.sh) if s[1] == 2)   # SHT_SYMTAB
        off, size, link, entsize = self.sh[symtab][4], self.sh[symtab][5], self.sh[symtab][6], self.sh[symtab][9]
        strbase = self.sh[link][4]
        for i in range(size // entsize):
            at = off + i * entsize
            name, value, sz, info, other, shndx = struct.unpack_from('<IIIBBH', self.d, at)
            yield at, self.cstr(strbase + name), value, sz, info, shndx

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lib'); ap.add_argument('out')
    ap.add_argument('--toolchain', required=True, help="JieLi pi32v2 bin directory")
    ap.add_argument('--report')
    a = ap.parse_args()
    tc = a.toolchain
    with tempfile.TemporaryDirectory() as tmp:
        members = run([os.path.join(tc, 'ar'), 't', a.lib]).decode().split()
        if len(members) != 1:
            raise SystemExit('expected one object in the library, found: ' + ' '.join(members))
        run([os.path.join(tc, 'ar'), 'x', os.path.abspath(a.lib)], cwd=tmp)
        obj = os.path.join(tmp, members[0])

        # the stub: return 0
        src = os.path.join(tmp, 'stub.c')
        open(src, 'w').write('int vsc_engine_stub(void) { return 0; }\n')
        sobj = os.path.join(tmp, 'stub.o')
        run([os.path.join(tc, 'clang'), '-target', 'pi32v2', '-mcpu=r3', '-O2', '-fno-lto', '-ffunction-sections', '-c', src, '-o', sobj])
        sbin = os.path.join(tmp, 'stub.bin')
        run([os.path.join(tc, 'objcopy'), '-O', 'binary', '-j', '.text.vsc_engine_stub', sobj, sbin])
        stub = open(sbin, 'rb').read()
        if not 0 < len(stub) <= 16:
            raise SystemExit(f'unexpected stub size {len(stub)}')

        elf = Elf(open(obj, 'rb').read())
        engine = {i for i, n in enumerate(elf.names) if ENGINE.match(n) and elf.sh[i][1] == 1 and elf.sh[i][2] & 4}   # PROGBITS, exec
        if not engine:
            raise SystemExit('no engine sections found: has the SDK changed?')
        # the host: the smallest engine function that can hold the stub
        host_name = min((elf.names[i] for i in engine if elf.sh[i][5] >= len(stub)), key=lambda n: elf.sh[elf.index(n)][5])
        # its relocations go first (objcopy renumbers sections, so read the ELF again after)
        if '.rela' + host_name in elf.names:
            run([os.path.join(tc, 'objcopy'), '--remove-section', '.rela' + host_name, obj])
        elf = Elf(open(obj, 'rb').read())
        engine = {i for i, n in enumerate(elf.names) if ENGINE.match(n) and elf.sh[i][1] == 1 and elf.sh[i][2] & 4}
        host = elf.index(host_name)
        off, size = elf.sh[host][4], elf.sh[host][5]
        body = (stub * (size // len(stub) + 1))[:size]
        elf.d[off:off + size] = body

        moved, kept_bytes = [], 0
        for at, name, value, sz, info, shndx in elf.symbols():
            if shndx in engine and shndx != host and (info & 15) == 2 and not KEEP.search(name):   # STT_FUNC
                struct.pack_into('<IH', elf.d, at + 4, 0, 0)               # st_value = 0 (st_size left)
                struct.pack_into('<H', elf.d, at + 14, host)              # st_shndx = host
                moved.append(name)
        if not moved:
            raise SystemExit('no engine functions moved')
        open(obj, 'wb').write(elf.d)
        if os.path.exists(a.out):
            os.remove(a.out)
        run([os.path.join(tc, 'ar'), 'rcs', os.path.abspath(a.out), members[0]], cwd=tmp)
        engine_bytes = sum(elf.sh[i][5] for i in engine)
        report = {'host': host_name, 'stub_bytes': len(stub), 'functions': len(moved), 'engine_sections': len(engine),
                  'engine_code_bytes': engine_bytes}
        print('engines stubbed:', json.dumps(report))
        if a.report:
            json.dump(dict(report, moved=sorted(moved)), open(a.report, 'w'), indent=1)

if __name__ == '__main__':
    main()
