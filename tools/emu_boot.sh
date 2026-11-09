#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Does the package start? It runs in fm1-emu (github.com/joakimeriksson/fm1-emu), an emulator that runs the
# FM-1's firmware as it is, M-VAVE's library included: the check that would have caught 0.2.2 and 0.2.3,
# which never started on a real FM-1 (the library's USB/Bluetooth/MIDI start-up ran out of heap, so audio,
# the control task, the screen and USB never came up).
#
#   tools/emu_boot.sh build/viscera.fwsc [EMU_DIR]
#
# Two runs: the emulator's fast timing, and the FM-1's measured instruction timing (FM1_STRICT=1, 360 MHz).
# Each passes when, five seconds in, the emulated FM-1 hasn't halted, its LEDs are lit and its screen shows
# something (both come only from the control task, which runs only when the library's start-up succeeded).
# M-VAVE's unmodified Open kit, built with tools/build_fm1.py, passes it too.
set -eu
FWSC=$(realpath "$1")
EMU=${2:-build/fm1-emu}
REV=${FM1_EMU_REV:-9fa0215}
if [ ! -d "$EMU/.git" ]; then
    git clone -q https://github.com/joakimeriksson/fm1-emu.git "$EMU"
    git -C "$EMU" checkout -q "$REV"
fi
(cd "$EMU" && RUSTFLAGS=-Awarnings cargo build -q --release -p fm1-live)
LIVE=$(realpath "$EMU/target/release/fm1-live")
OUT=$(realpath -m build/emuboot)
fail=0

run() {   # NAME MS [env...]
    name=$1; ms=$2; shift 2
    mkdir -p "$OUT/$name/work"
    (cd "$OUT/$name" && env FM1_BENCH_NOKEYS=1 "$@" "$LIVE" --fwsc "$FWSC" --mhz 360 --midi none --shot "$ms") \
        > "$OUT/$name.log" 2>&1 || true
    leds=$(sed -n 's/.*LEDs lit: \[\([^]]*\)\].*/\1/p' "$OUT/$name.log" | tail -1)
    lit=$(python3 - "$OUT/$name/work/panel.ppm" <<'EOF' 2>/dev/null || echo 0
import sys
d = open(sys.argv[1], "rb").read()
# binary PPM: P6, width, height, max, then RGB
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
# the screen sits in the panel's upper middle; count bright pixels there
n = 0
for y in range(40, 520, 4):
    for x in range(360, 860, 4):
        i = (y * w + x) * 3
        if i + 2 < len(px) and px[i] + px[i + 1] + px[i + 2] > 200:
            n += 1
print(n)
EOF
)
    if grep -q "^halt" "$OUT/$name.log"; then
        echo "FAIL $name: the emulated FM-1 halted"; sed -n '/^halt/,$p' "$OUT/$name.log" | head -5; fail=1
    elif [ -z "$leds" ] || [ "${lit:-0}" -lt 200 ]; then
        echo "FAIL $name: it didn't start (LEDs lit: [${leds}], bright screen pixels: ${lit:-0})"; tail -5 "$OUT/$name.log"; fail=1
    else
        echo "ok   $name: started (LEDs lit: [$leds], bright screen pixels: $lit)"
    fi
}

run boot 5000
run strict 5000 FM1_STRICT=1
exit $fail
