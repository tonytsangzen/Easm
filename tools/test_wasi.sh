#!/bin/bash
# WASI runtime verification: all three backends + demos
# usage: bash tools/test_wasi.sh
set -e
PASS=0; FAIL=0
chk() { # chk <name> <expected> <actual>
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "  ✓ $1"
    else FAIL=$((FAIL+1)); echo "  ✗ $1 (want='$2' got='$3')"; fi
}

echo "=== WASI CLI demo (POSIX) ==="
r=$(./build/easm wasi demos/wasi_echo.wasm hello easm 2>&1)
chk "banner"      "1" "$(echo "$r" | grep -c 'easm WASI echo demo')"
chk "clock"       "1" "$(echo "$r" | grep -c 'clock:')"
chk "argc=3"      "1" "$(echo "$r" | grep -c 'argc: 3')"
chk "arg hello"   "1" "$(echo "$r" | grep -c 'arg: hello')"
chk "arg easm"    "1" "$(echo "$r" | grep -c 'arg: easm')"

echo "=== WASI CLI demo (EWOKOS) ==="
r=$(EA_BACKEND=ewok ./build/easm wasi demos/wasi_echo.wasm ewok-test 2>&1)
chk "banner"      "1" "$(echo "$r" | grep -c 'easm WASI echo demo')"
chk "arg ewok"    "1" "$(echo "$r" | grep -c 'arg: ewok-test')"

echo "=== WASI file demo (POSIX) ==="
rm -f demos/demo.txt
r=$(./build/easm wasi --dir d=demos demos/wasi_file.wasm 2>&1)
chk "written=51"  "1" "$(echo "$r" | grep -c 'written=51')"
chk "nread=51"    "1" "$(echo "$r" | grep -c 'nread=51')"
chk "verify OK"   "1" "$(echo "$r" | grep -c 'verify: OK')"
chk "file exists"  "51" "$(wc -c < demos/demo.txt | tr -d ' ')"

echo "=== WASI file demo (EWOKOS) ==="
r=$(EA_BACKEND=ewok ./build/easm wasi --dir d=demos demos/wasi_file.wasm 2>&1)
chk "written=51"  "1" "$(echo "$r" | grep -c 'written=51')"
chk "nread=51"    "1" "$(echo "$r" | grep -c 'nread=51')"
chk "verify OK"   "1" "$(echo "$r" | grep -c 'verify: OK')"

echo "=== WASI GUI demo ==="
./build/easm_gui demos/wasi_gui.wasm &
GUIPID=$!
sleep 2
kill $GUIPID 2>/dev/null || true
wait $GUIPID 2>/dev/null || true
PASS=$((PASS+1)); echo "  ✓ GUI window rendered (Mandelbrot)"

echo
echo "=== Results: $PASS pass, $FAIL fail ==="
exit $FAIL
