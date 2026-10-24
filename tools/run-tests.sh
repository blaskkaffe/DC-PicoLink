#!/bin/bash
# Host-side tests: tools/run-tests.sh [firmware-build-dir]
# Without a build dir only the C model tests run; with one, the PIO simulations also
# run against that build's assembled W5500 program (w5500_spi.pio.h).
set -eu
cd "$(dirname "$0")/../firmware"
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
fail=0
for t in test_w5500 test_w5500_recv test_sd test_sd_detect test_sense test_baud; do
    if gcc -O1 -Wall -Wno-unused-variable -Wno-unused-function -I. "tests/$t.c" -lm -o "$out/$t" && "$out/$t" > "$out/$t.log"; then
        echo "PASS $t: $(tail -1 "$out/$t.log")"
    else
        echo "FAIL $t"; tail -20 "$out/$t.log" 2>/dev/null; fail=1
    fi
done
# dctest: Dreamcast test tool: syntax check of the KOS path against stub headers, then the Python
# host tool against the host build of the DC program (serial, baud switching, UDP, TCP; ~1 min)
if gcc -fsyntax-only -Wall -I../tools/dctest/stub ../tools/dctest/dctest.c 2> "$out/dc.log"; then echo "PASS dctest KOS-path syntax"; else echo "FAIL dctest syntax"; cat "$out/dc.log"; fail=1; fi
if python3 -I ../tools/dctest/dctest.py selftest --log "$out/dctest.log" --tmp "$out" > "$out/dctest.out" 2>&1; then echo "PASS dctest selftest: $(grep -m1 'tests,' "$out/dctest.out" | sed 's/^\[[^]]*\] //')"; else echo "FAIL dctest selftest"; tail -20 "$out/dctest.out"; fail=1; fi
if [ -n "${1:-}" ]; then
    hdr="$1/w5500_spi.pio.h"
    [ -f "$hdr" ] || { echo "no $hdr: build the firmware first"; exit 1; }
    sim=$(python3 -I tests/pio_sim.py "$hdr")
    pass=$(grep -c '^PASS' <<<"$sim" || true); bad=$(grep -ci '^FAIL\|Traceback' <<<"$sim" || true)
    if [ "$bad" = 0 ] && [ "$pass" -gt 0 ]; then echo "PASS pio_sim: $pass checks"; else echo "FAIL pio_sim"; echo "$sim" | tail -20; fail=1; fi
    if python3 -I tests/kos_replay.py "$hdr" > "$out/kos.log" 2>&1; then echo "PASS kos_replay"; else echo "FAIL kos_replay"; tail "$out/kos.log"; fail=1; fi
fi
exit $fail
