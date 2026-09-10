#!/bin/bash
# check_elf.sh — validate our ET_REL ELF output.
shopt -s nullglob
# For each test: emit an ELF object, confirm readelf accepts it as an ARM
# relocatable, and confirm its .text matches our raw image byte-for-byte
# (i.e. the ELF wrapper didn't corrupt the code).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$HERE/../../apps/source/assembler"
BIN="$HERE/test_asm"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cc -O2 -I"$CORE" -o "$BIN" "$HERE/test_asm.c" "$CORE/asm_thumb.c" || exit 2

pass=0 fail=0
for s in "$HERE"/test/*.s "$HERE"/test/*.rs; do
    name="$(basename "$s")"
    "$BIN" "$s" "$TMP/raw.bin"  2>/dev/null || { echo "FAIL $name (asm)"; fail=$((fail+1)); continue; }
    "$BIN" "$s" --elf "$TMP/o.o" 2>/dev/null || { echo "FAIL $name (elf)"; fail=$((fail+1)); continue; }
    if ! arm-none-eabi-readelf -h "$TMP/o.o" 2>/dev/null | grep -q "REL (Relocatable file)"; then
        echo "FAIL $name (not ET_REL)"; fail=$((fail+1)); continue; fi
    if ! arm-none-eabi-readelf -h "$TMP/o.o" 2>/dev/null | grep -q "Machine:.*ARM"; then
        echo "FAIL $name (not ARM)"; fail=$((fail+1)); continue; fi
    arm-none-eabi-objcopy -O binary -j .text "$TMP/o.o" "$TMP/etext.bin" 2>/dev/null
    if cmp -s "$TMP/raw.bin" "$TMP/etext.bin"; then
        nrel=$(arm-none-eabi-readelf -r "$TMP/o.o" 2>/dev/null | grep -c R_ARM_ABS32)
        echo "ok    $name (.text matches, $nrel relocs)"; pass=$((pass+1))
    else
        echo "FAIL  $name (.text mismatch vs raw image)"; fail=$((fail+1))
    fi
done
echo "-----"; echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
