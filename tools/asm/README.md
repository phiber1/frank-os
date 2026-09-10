# Thumb-16 assembler — host development & validation

Portable core for the FRANK OS native assembler (task #18).  The core
(`apps/source/assembler/asm_thumb.{c,h}`) has **no OS dependency**: it
assembles a source string into a machine-code image + symbol table +
diagnostics, and can emit a CSUB text block.  It is developed and
validated here on the host against `arm-none-eabi-as` before it is
wrapped in a FRANK OS app (editor + file I/O + ELF output).

## Validate

```
tools/asm/check_vs_gas.sh    # raw .text byte-for-byte vs arm-none-eabi-as
tools/asm/check_elf.sh       # ET_REL ELF is well-formed; .text matches raw
```

`check_vs_gas.sh` assembles every `test/*.s` with both GAS and our core and
compares the raw `.text` byte-for-byte (6/6 pass).  `check_elf.sh` emits an
ELF object for each test (plus reloc-exercising `test/*.rs`), confirms
`readelf` accepts it as an ARM relocatable, and checks its `.text` matches
our raw image (7/7 pass).  Reloc `.rs` cases are kept out of the byte
compare because our relocations are section-relative (addend in the word)
where GAS's are symbol-relative — both relocate to the same address.

## Harness

```
tools/asm/test_asm <src.s> <out.bin>       # raw image
tools/asm/test_asm <src.s> --hex           # hex halfwords + summary
tools/asm/test_asm <src.s> --csub NAME     # CSUB text block
tools/asm/test_asm <src.s> --elf a.o [ENT] # ET_REL ELF object (entry ENT)
```

## Instruction set (Thumb-16 subset + BL)

- data-processing: MOV/MOVS (imm8, lo, hi), MVN, CMP/CMN/TST,
  ADD/SUB (imm3, imm8, reg, hi, SP forms), ADR, ADD/SUB SP
- shifts: LSL/LSR/ASR (imm & reg, 2- and 3-operand), ROR
- logical/arith reg: AND EOR ADC SBC ORR BIC MUL NEG/RSBS
- extends/reverse: SXTB SXTH UXTB UXTH REV REV16 REVSH
- memory: LDR/STR/LDRB/STRB/LDRH/STRH (imm5 & reg offset),
  LDR/STR [SP,#], LDR =literal (auto pool, deduped), LDRSB/LDRSH (reg)
- stack/multi: PUSH POP STMIA LDMIA
- branch: B, B<cond> (all conditions incl. HS/LO aliases), BL (32-bit
  halfword pair), BX, BLX
- misc: NOP SVC BKPT
- directives: .org .equ/.set .word/.int/.long .hword/.short .byte
  .ascii/.asciz/.space/.skip .align/.balign; `.syntax/.thumb/.global/...`
  accepted & ignored; labels `name:`; `main`/`_start` set the CSUB entry.
- expressions: dec/hex/char/`.`(PC)/symbols, + - * / & | << >> ~, parens
  (left-to-right, no precedence — adequate for operands).

## On-device workflow (pshell `asm` → MMBASIC)

pshell and the BASIC app share the SD card (pshell's `fs_*` maps to FatFS),
so the whole loop is on-device with no copy/paste:

```
vi mycsub.s                 # write Thumb-16 in pshell's vi (save with :wq)
asm mycsub.s -c hello -o mycsub.inc   # emit a CSUB include file to the SD
```
Then in MMBASIC, pull it into your program and save (the include-equivalent):
```
MERGE "mycsub.inc"          # appends the CSUB to the in-memory program
SAVE "myprog.bas"
```
or make it callable from every program with `LIBRARY SAVE`.
`asm mycsub.s -o mycsub.o` instead writes a relocatable ELF object;
`asm mycsub.s -c hello` prints the CSUB block to the screen.

### Writing a CSUB in assembly (important gotcha)

MMBASIC passes CSUB arguments **by reference**: a call `hello ct` gives the
CSUB `r0 = &ct` (a pointer to the variable), NOT the value.  Dereference
before use.  To reach the CallTable and call an MMBASIC service:

```asm
; hello(ct) — print a string via CallTable[8] = MMPrintString(char*)
main:
    push    {lr}
    ldr     r0, [r0]        ; r0 = ct value = CallTable base (was &ct)
    ldr     r1, [r0, #32]   ; r1 = CallTable[8] = MMPrintString
    adr     r0, msg         ; r0 = &msg (PC-relative → position independent)
    blx     r1
    pop     {pc}
    .align  2
msg:
    .asciz  "HELLO FROM ASSEMBLY!\r\n"
```
Useful CallTable byte offsets (see apps/source/basic/PicoMite.c): [8]=0x20
MMPrintString, [19]=0x4C &HRes, [55] basic_gfx_mark(x1,y1,x2,y2),
[56] &WriteBuf (4bpp draw buffer, stride=HRes/2).  A CSUB must be position-
independent (use ADR / PC-relative literals), since it's loaded at a
runtime address.

## ELF output (executables)

FRANK OS apps are ET_REL objects relocated by the OS loader at load time
(the loader *is* the linker — see `src/app.c`).  `asm_emit_elf()` produces
exactly that: `.text` + `.rel.text` (R_ARM_ABS32 against the `.text`
section symbol, addend in the word) + `.symtab` (null, `.text` section
symbol, entry as GLOBAL FUNC with the thumb bit) + `.strtab`/`.shstrtab`.
Only load-address-dependent values need relocation — `LDR =label` literals
and `.word label`; PC-relative branches are resolved at assembly time.
The entry defaults to `main` (the symbol the loader scans for).

## Status / next

- [x] Milestone 1: core + GAS-validated encodings + CSUB text output
- [x] Milestone 3: ET_REL ELF writer, readelf-clean, loader-correct relocs
- [ ] Milestone 2: expression precedence, mid-stream `.ltorg`, richer
      diagnostics; multiple named global symbols in the ELF symtab
- [ ] FRANK OS app wrapper: editor UI, load/save, assemble+run, error
      navigation, write CSUB into a .bas / write .o to SD (on-hardware)
- [ ] On-HW: load an assembled .o via the OS loader and run it
