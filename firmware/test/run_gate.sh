#!/usr/bin/env bash
# Pre-flash gate: real avrdude talks STK500v2 to our protocol core over a
# pty; the mock HAL simulates a straight-wired target chip and decodes the
# actual HVPP waveforms.  Green here (plus explicit user go) is the
# requirement before the first flash erases the stock firmware.
#
# Usage: test/run_gate.sh          (AVRDUDE=path to override binary)

set -u
cd "$(dirname "$0")/.."
AVRDUDE=${AVRDUDE:-avrdude}
BUILD=build
FAILS=0

make -s host || exit 1

echo "== unit tests =="
"$BUILD/test_remap" || exit 1

if ! command -v "$AVRDUDE" >/dev/null 2>&1; then
    echo "GATE INCOMPLETE: avrdude not found (set AVRDUDE=...)"
    exit 2
fi
"$AVRDUDE" -? 2>&1 | head -1

run_case() {
    local profile=$1 part=$2 sig=$3
    shift 3
    local simlog="$BUILD/sim_$profile.log"
    local out="$BUILD/avrdude_$profile.out"
    local ptyfile="$BUILD/pty_$profile"
    local rc ok=1 pty

    rm -f "$ptyfile"
    SIM_PROFILE=$profile "$BUILD/pty_harness" "$ptyfile" >/dev/null 2>"$simlog" &
    local hpid=$!
    for _ in $(seq 50); do [ -s "$ptyfile" ] && break; sleep 0.1; done
    if [ ! -s "$ptyfile" ]; then
        echo "GATE FAIL($profile): harness did not start"
        kill "$hpid" 2>/dev/null
        return 1
    fi
    pty=$(cat "$ptyfile")

    "$AVRDUDE" -c stk500pp -P "$pty" -p "$part" "$@" >"$out" 2>&1
    rc=$?
    kill "$hpid" 2>/dev/null
    wait "$hpid" 2>/dev/null

    if [ $rc -ne 0 ]; then
        echo "GATE FAIL($profile): avrdude exit $rc; last output:"
        tail -25 "$out" | sed 's/^/    /'
        ok=0
    fi
    if ! grep -qi "$sig" "$out"; then
        echo "GATE FAIL($profile): signature $sig not confirmed"
        ok=0
    fi
    if grep -q "SIM: WARN" "$simlog"; then
        echo "GATE FAIL($profile): simulator warnings:"
        grep "SIM: WARN" "$simlog" | sed 's/^/    /'
        ok=0
    fi
    if grep -qE "SIM: (WRITE|CHIP ERASE)" "$simlog"; then
        echo "GATE FAIL($profile): write/erase during a read-only session"
        ok=0
    fi
    [ $ok -eq 1 ] && echo "GATE PASS($profile: $part, sig ok, no warnings, no writes)"
    [ $ok -eq 1 ]
}

expect_value() {
    # expect_value <profile> <name> <hexvalue> — checks avrdude -U ...:r:-:h output
    local out="$BUILD/avrdude_$1.out"
    if grep -qiE "^$3\$" "$out"; then
        echo "  value ok($1): $2 = $3"
    else
        echo "GATE FAIL($1): expected $2 = $3; got:"
        grep -iE "^0x" "$out" | sed 's/^/    /'
        FAILS=$((FAILS+1))
    fi
}

echo "== gate case: t461a (x61 crossed stack, auto-remap) =="
run_case t461a t461a "1e9208" \
    -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h -U lock:r:-:h \
    || FAILS=$((FAILS+1))
expect_value t461a lfuse 0x62
expect_value t461a hfuse 0xdf
expect_value t461a efuse 0xff
expect_value t461a lock  0xff

echo "== gate case: t2313 (20-pin standard stack, identity) =="
run_case t2313 t2313 "1e910a" -U lfuse:r:-:h -U hfuse:r:-:h \
    || FAILS=$((FAILS+1))
expect_value t2313 lfuse 0x64
expect_value t2313 hfuse 0xdf

echo "== gate case: m8 (28-pin standard stack, identity) =="
run_case m8 m8 "1e9307" -U lfuse:r:-:h -U hfuse:r:-:h -U lock:r:-:h \
    || FAILS=$((FAILS+1))
expect_value m8 lfuse 0xe1
expect_value m8 hfuse 0xd9
expect_value m8 lock  0xff

echo
if [ $FAILS -eq 0 ]; then
    echo "==== PRE-FLASH GATE: ALL GREEN ===="
    echo "(first flash still requires the explicit user go/no-go)"
else
    echo "==== PRE-FLASH GATE: $FAILS FAILURE(S) ===="
fi
exit $((FAILS != 0))
