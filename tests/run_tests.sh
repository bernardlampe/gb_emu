#!/usr/bin/env bash
# run_tests.sh — the course's end-to-end harness.
#
# Every check runs the emulator headless and asserts on observable output:
# a string a test ROM printed, a frame hash, or a pixel in a dumped frame.
# Nothing here looks at the emulator's internals, so the same script grades a
# student's emulator and the reference implementation identically.
#
# Usage: tests/run_tests.sh [path/to/gbemu]   (default: build/gbemu)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GBEMU="${1:-$ROOT/build/gbemu}"
ROMS="$ROOT/build/roms"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# check <name> <expected substring> <gbemu args...>
check() {
    local name="$1" want="$2"
    shift 2
    local out="$WORK/$name.log"
    if ! "$GBEMU" "$@" >"$out" 2>&1; then
        echo "FAIL $name: emulator exited non-zero"
        sed 's/^/    /' "$out" | head -20
        fail=$((fail + 1))
        return
    fi
    if grep -qF "$want" "$out"; then
        echo "PASS $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name: expected '$want'"
        sed 's/^/    /' "$out" | head -20
        fail=$((fail + 1))
    fi
}

if [ ! -x "$GBEMU" ]; then
    echo "error: emulator '$GBEMU' not found; build it first" >&2
    exit 1
fi

echo "generating test ROMs"
python3 "$ROOT/tools/make_test_roms.py" "$ROMS" || exit 1
echo

# --- L3-L4: the bus and the serial port (the test harness itself) ---------
check "serial" "SERIAL OK" --headless --frames 5 "$ROMS/serial.gb"

# --- L6-L8: instruction semantics and flags ------------------------------
check "alu flags" "ALU OK" --headless --frames 5 "$ROMS/alu.gb"

# --- L9: DIV, TIMA, TMA, TAC -------------------------------------------
check "timer" "TIMER OK" --headless --frames 10 "$ROMS/timer.gb"

# --- L10: IF/IE, EI, HALT and the vector table --------------------------
check "interrupt" "VBLANK OK" --headless --frames 10 "$ROMS/interrupt.gb"

# --- L13: MBC1 banking, the bank-0 quirk, RAM enable ---------------------
check "mbc1" "MBC1 OK" --headless --frames 5 "$ROMS/mbc1.gb"

# --- L21: OAM DMA, the 640 T-cycle transfer and the HRAM lockout ----------
check "oam dma" "DMA OK" --headless --frames 10 "$ROMS/dma.gb"

# --- L23: the FF00 matrix and the joypad interrupt -----------------------
# The host presses A at frame 60 and holds it for 8 frames.
check "joypad" "JOYPAD OK" --headless --frames 90 --press 60:A "$ROMS/joypad.gb"

# --- L24-L26: APU registers and channel status --------------------------
check "sound registers" "SOUND OK" --headless --frames 30 "$ROMS/sound.gb"

# --- L24-L27: wave RAM is storage of its own, not part of the NR file ------
check "wave RAM isolation" "WAVE RAM OK" --headless --frames 30 "$ROMS/waveram.gb"

# --- L19-L21: the PPU, checked against an independently built image -------
"$GBEMU" --headless --frames 4 --dump-frame "$WORK/ppu.bmp" "$ROMS/ppu.gb" >"$WORK/ppu.log" 2>&1
if grep -qF "PPU OK" "$WORK/ppu.log" && python3 "$ROOT/tests/check_frame.py" "$WORK/ppu.bmp"; then
    echo "PASS ppu frame"
    pass=$((pass + 1))
else
    echo "FAIL ppu frame"
    sed 's/^/    /' "$WORK/ppu.log" | head -20
    fail=$((fail + 1))
fi

# --- L24-L27: the audio path actually produces samples --------------------
"$GBEMU" --headless --frames 30 --audio-stats "$ROMS/sound.gb" >"$WORK/audio.log" 2>&1
peak="$(sed -n 's/.*peak \([0-9]*\).*/\1/p' "$WORK/audio.log" | tail -1)"
if [ -n "${peak:-}" ] && [ "$peak" -gt 1000 ]; then
    echo "PASS audio path (peak sample $peak)"
    pass=$((pass + 1))
else
    echo "FAIL audio path: expected a peak sample above 1000"
    sed 's/^/    /' "$WORK/audio.log" | head -20
    fail=$((fail + 1))
fi

# --- L29: determinism, and save states that capture the whole machine -------
h1="$("$GBEMU" --headless --frames 200 --frame-hash "$ROMS/determinism.gb" 2>/dev/null | tail -1)"
h2="$("$GBEMU" --headless --frames 200 --frame-hash "$ROMS/determinism.gb" 2>/dev/null | tail -1)"
"$GBEMU" --headless --frames 100 --state-out "$WORK/mid.state" "$ROMS/determinism.gb" >/dev/null 2>&1
h3="$("$GBEMU" --headless --frames 100 --state-in "$WORK/mid.state" --frame-hash "$ROMS/determinism.gb" 2>/dev/null | tail -1)"
if [ -n "$h1" ] && [ "$h1" = "$h2" ] && [ "$h1" = "$h3" ]; then
    echo "PASS determinism and save states ($h1)"
    pass=$((pass + 1))
else
    echo "FAIL determinism: $h1 / $h2 / $h3 (two runs and a save-state resume must agree)"
    fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
