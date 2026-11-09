#!/usr/bin/env python3
"""Leave M-VAVE's stock sound engines out of Viscera's firmware.

The FM1 SDK's frozen library (sdk/lib/libfm1_engine.a, one object) carries the
stock instrument: the DX7, 303, PSG, 808, pluck and sampler engines. Viscera
never plays them (board/audio.c calls vsc_fm1_render instead of the library's
fm1_engine_render), but the library's own audio code still calls them, so the
linker keeps them: about 26 KB of RAM code, plus their tables and state.

This rewrites a copy of the library object so that the engines' seven render
functions point at one tiny function that returns 0 (see RENDERS below for why
they are unreachable in Viscera); --gc-sections then drops their code and what
only they used. Everything else in the engines keeps its code.

  python3 tools/strip_engines.py LIB.a OUT.a --toolchain BIN [--report OUT.json]

Steps:
  1. take the one object out of the archive
  2. compile the stub, and put its code in the smallest engine function's section
     (that section's relocations go: it holds only the stub now)
  3. point every function symbol of the engines' sections at it (offset 0)
  4. archive the result
"""
import argparse, json, os, re, struct, subprocess, sys, tempfile

# Only the engines' render functions: each is called from one place, the library's own
# render(), which runs only for the stock audio path (fm1_engine_render and the
# second core's render jobs it hands out). Viscera's audio interrupt never calls
# fm1_engine_render, so these are never reached. Everything else (init, notes, MIDI,
# parameters, panic) keeps its code. The script checks the callers before changing
# anything and refuses if any other code calls one of them.
RENDERS = ('synth_dx7_render', 'bass303_bank_render', 'psg_render', 'pluck_render',
           'drum808_kit_render', 'drum808_kit_render_st', 'sampledrum_render')

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
        syms = list(elf.symbols())
        index_of = {}
        for k, (at, name, value, sz, info, shndx) in enumerate(syms):
            if name in RENDERS and (info & 15) == 2:
                if name in index_of:
                    raise SystemExit('two functions named ' + name)
                index_of[name] = (k, shndx, value, sz)
        if set(index_of) != set(RENDERS):
            raise SystemExit('render functions not found: ' + ', '.join(sorted(set(RENDERS) - set(index_of))) + ' (has the SDK changed?)')
        # every reference to them must come from inside render() in the hub section
        hub = elf.index('.volatile_ram_code')
        rnd = [(v, sz) for (at, n, v, sz, info, sh) in syms if n == 'render' and sh == hub and (info & 15) == 2]
        if len(rnd) != 1:
            raise SystemExit("the library's render() not found where expected")
        r0, rsz = rnd[0]
        wanted = {k for (k, _, _, _) in index_of.values()}
        for i, sh in enumerate(elf.sh):
            if sh[1] != 4:   # SHT_RELA
                continue
            target = sh[7]   # sh_info: the section the relocations apply to
            for e in range(sh[5] // 12):
                off, info, add = struct.unpack_from('<IIi', elf.d, sh[4] + e * 12)
                if info >> 8 in wanted and not (target == hub and r0 <= off < r0 + rsz):
                    who = elf.names[target]
                    raise SystemExit(f'{syms[info >> 8][1]} is also called from {who}+{off:#x}: not stubbing')
        # the host: the smallest of their sections that can hold the stub
        sec = {name: shndx for name, (k, shndx, v, sz) in index_of.items()}
        for name, shndx in sec.items():
            if index_of[name][2] != 0:
                raise SystemExit(name + ' is not at the start of its own section')
        host_name = min((elf.names[x] for x in sec.values() if elf.sh[x][5] >= len(stub)), key=lambda n: elf.sh[elf.index(n)][5])
        host = elf.index(host_name)
        # its relocations go: it holds only the stub now (an empty relocation section)
        if '.rela' + host_name in elf.names:
            shoff, = struct.unpack_from('<I', elf.d, 32)
            shentsize, = struct.unpack_from('<H', elf.d, 46)
            struct.pack_into('<I', elf.d, shoff + elf.index('.rela' + host_name) * shentsize + 20, 0)   # sh_size = 0
        off, size = elf.sh[host][4], elf.sh[host][5]
        elf.d[off:off + size] = (stub * (size // len(stub) + 1))[:size]
        moved = []
        for at, name, value, sz, info, shndx in elf.symbols():
            if name in RENDERS and (info & 15) == 2:
                struct.pack_into('<I', elf.d, at + 4, 0)          # st_value = 0
                struct.pack_into('<H', elf.d, at + 14, host)      # st_shndx = host
                moved.append(name)
        engine = set()
        open(obj, 'wb').write(elf.d)
        if os.path.exists(a.out):
            os.remove(a.out)
        run([os.path.join(tc, 'ar'), 'rcs', os.path.abspath(a.out), members[0]], cwd=tmp)
        report = {'host': host_name, 'stub_bytes': len(stub), 'functions': len(moved)}
        print('engines stubbed:', json.dumps(report))
        if a.report:
            json.dump(dict(report, moved=sorted(moved)), open(a.report, 'w'), indent=1)

if __name__ == '__main__':
    main()
