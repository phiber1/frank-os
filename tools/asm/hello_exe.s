; hello_exe.s — a STANDALONE executable (not a CSUB) for the built-in
; Terminal.  Entry = main (the assembler auto-marks it global FUNC).
; Prints via goutf(), which is sys_table index 41 (byte offset 164).
; The sys_table base is fixed at 0x10000000 + 16MB - 4KB = 0x10FFF000,
; so &sys_table[41] = 0x10FFF0A4.  All addressing is position-independent
; (fixed sys_table address + PC-relative string), so no relocations.
.thumb
.global main
main:
    push    {lr}
    ldr     r0, =0x10FFF0A4    ; r0 = &sys_table[41]  (&goutf)
    ldr     r1, [r0]           ; r1 = goutf
    adr     r0, msg            ; r0 = format string (PC-relative)
    blx     r1                 ; goutf(msg)
    movs    r0, #0             ; return 0
    pop     {pc}
    .align  2
msg:
    .asciz  "Hello from a standalone ELF, hand-assembled on FRANKOS!\r\n"
