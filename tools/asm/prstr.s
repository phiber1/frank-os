; prstr.s — a CSUB that prints a string to the MMBasic screen.
; Called from BASIC as:  hello ct
; MMBasic passes CSUB args BY REFERENCE, so r0 = &ct (pointer to the
; variable), not the CallTable address — dereference it once first.
; CallTable[8] (byte offset 32) = MMPrintString(char *s).
main:
    push    {lr}
    ldr     r0, [r0]          ; r0 = ct value = CallTable base (was &ct)
    ldr     r1, [r0, #32]     ; r1 = ct[8] = MMPrintString
    adr     r0, msg           ; r0 = &msg  (PC-relative → position independent)
    blx     r1                ; MMPrintString(msg)
    pop     {pc}              ; return
    .align  2                 ; word-align the string for ADR
msg:
    .asciz  "HELLO FROM ASSEMBLY!\r\n"
