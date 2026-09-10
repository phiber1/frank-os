#!/bin/bash
# check_vs_gas.sh — validate the portable assembler against arm-none-eabi-as.
# For each tools/asm/test/*.s: assemble with GAS and with our core, then
# compare the raw .text images byte-for-byte.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$HERE/../../apps/source/assembler"
BIN="$HERE/test_asm"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# build the harness
cc -O2 -Wall -Wextra -I"$CORE" -o "$BIN" "$HERE/test_asm.c" "$CORE/asm_thumb.c" || exit 2

pass=0 fail=0
for s in "$HERE"/test/*.s; do
    [ -e "$s" ] || { echo "no test files"; exit 0; }
    name="$(basename "$s")"
    if ! arm-none-eabi-as -mcpu=cortex-m33 -mthumb "$s" -o "$TMP/gas.o" 2>"$TMP/gaserr"; then
        echo "SKIP  $name (gas rejected: $(head -1 "$TMP/gaserr"))"; continue
    fi
    arm-none-eabi-objcopy -O binary -j .text "$TMP/gas.o" "$TMP/gas.bin"
    if ! "$BIN" "$s" "$TMP/mine.bin" 2>"$TMP/mineerr"; then
        echo "FAIL  $name (our assembler errored):"; sed 's/^/        /' "$TMP/mineerr"; fail=$((fail+1)); continue
    fi
    if cmp -s "$TMP/gas.bin" "$TMP/mine.bin"; then
        echo "ok    $name"; pass=$((pass+1))
    else
        echo "FAIL  $name (byte mismatch):"
        echo "   gas : $(xxd -p "$TMP/gas.bin" | tr -d '\n')"
        echo "   mine: $(xxd -p "$TMP/mine.bin" | tr -d '\n')"
        fail=$((fail+1))
    fi
done
echo "-----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
