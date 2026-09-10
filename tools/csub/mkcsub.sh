#!/bin/bash
# mkcsub.sh <src.c> <csubname> [typelist]
# Compile a C file into a PicoMite CSUB block for FRANKOS MMBASIC.
# Rules for the C code:
#   - no globals/statics with initialisers (.data forbidden), no libc
#   - firmware services go through the CallTable pointer passed as an arg
#   - the entry function may be anywhere; its offset is emitted properly
set -e
SRC=$1; NAME=$2; TYPES=${3:-}
B=$(dirname "$SRC")/$(basename "$SRC" .c)
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2 -ffreestanding -nostdlib -nostartfiles \
  -fno-jump-tables -Wl,-Ttext=0 -Wl,--entry=main -o "$B.elf" "$SRC"
if arm-none-eabi-objdump -h "$B.elf" | grep -qE '\.(data|got)[[:space:]]' ; then
  arm-none-eabi-size -A "$B.elf" | grep -E '\.(data|got)' | awk '$2>0{exit 1}' || {
    echo "ERROR: .data/.got in CSUB — code and rodata only" >&2; exit 1; }
fi
arm-none-eabi-objcopy -O binary "$B.elf" "$B.bin"
ENTRY=$(arm-none-eabi-nm "$B.elf" | awk '$3=="main"{print strtonum("0x"$1)}')
printf 'CSUB %s %s\n' "$NAME" "$TYPES"
printf '  %08X\n' $((ENTRY / 4))
hexdump -v -e '1/4 "%08X\n"' "$B.bin" | paste -d' ' - - - - | sed 's/\t/ /g; s/^/  /; s/ *$//'
echo "END CSUB"
