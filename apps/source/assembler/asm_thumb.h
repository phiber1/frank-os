/*
 * asm_thumb.h — portable Thumb-16 assembler core for FRANK OS
 *
 * A two-pass assembler for the ARMv7-M Thumb 16-bit instruction subset
 * (plus BL, which is a 32-bit halfword pair).  It is deliberately free
 * of any OS/host dependency: it takes assembly source as a string and
 * produces a machine-code byte image, a symbol table, and a list of
 * diagnostics.  The FRANK OS app wraps this with an editor and file I/O;
 * the host test harness (tools/asm) wraps it with a main() and diffs the
 * output against arm-none-eabi-as.
 *
 * Output targets are layered on top of the raw image by the caller:
 *   - CSUB text  (hex words + entry offset, format per tools/csub)
 *   - ELF32      (minimal, with relocations for literal pools)
 *
 * Only freestanding libc is assumed (memcpy/memset/strlen/strcmp and the
 * ctype-ish checks are provided internally so this compiles anywhere).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASM_THUMB_H
#define ASM_THUMB_H

#include <stdint.h>
#include <stddef.h>

#define ASM_MAX_SYMBOLS     512
#define ASM_MAX_ERRORS       64
#define ASM_MAX_LABELLEN     31
#define ASM_MAX_CODE     (64 * 1024)   /* output image cap */
#define ASM_MAX_LITERALS    256
#define ASM_MAX_RELOCS      256

typedef struct {
    char     name[ASM_MAX_LABELLEN + 1];
    uint32_t value;          /* address (in .org space) or .equ constant */
    uint8_t  defined;        /* resolved in pass 1 */
    uint8_t  is_equ;         /* from .equ (a constant, not an address) */
} asm_symbol;

typedef struct {
    int  line;               /* 1-based source line */
    char msg[80];
} asm_error;

typedef struct {
    /* output */
    uint8_t   code[ASM_MAX_CODE];
    uint32_t  code_len;      /* bytes emitted */
    uint32_t  org;           /* base address (default 0) */
    uint32_t  entry;         /* byte offset of the `main`/entry label, or 0 */

    /* symbol table */
    asm_symbol sym[ASM_MAX_SYMBOLS];
    int        nsym;

    /* diagnostics */
    asm_error  err[ASM_MAX_ERRORS];
    int        nerr;

    /* internal pass state (not for callers) */
    int        pass;
    uint32_t   pc;           /* current address during a pass */
    int        cur_line;
    /* pending literal pool ("LDR Rd,=imm" / "=label") */
    struct { uint32_t value; int is_sym; char sym[ASM_MAX_LABELLEN + 1];
             uint32_t patch_at; } lit[ASM_MAX_LITERALS];
    int        nlit;

    /* R_ARM_ABS32 relocations for ELF output: a .text byte offset whose
     * 32-bit word holds a section-relative address that must be fixed up
     * by the load base.  All target the single .text section symbol; the
     * in-place word already holds the addend (target offset, org 0). */
    uint32_t   reloc[ASM_MAX_RELOCS];
    int        nreloc;
    int        expr_used_sym;   /* eval() sets this if an address symbol
                                   (not .equ constant) was referenced */
} asm_ctx;

/* Assemble `src` (NUL-terminated).  Returns 0 on success, -1 if any
 * error was recorded (see ctx->err / ctx->nerr).  On success the image
 * is in ctx->code[0..code_len) and ctx->entry names the entry offset. */
int asm_assemble(asm_ctx *ctx, const char *src);

/* Convenience: look up a resolved symbol by name (NULL if absent). */
const asm_symbol *asm_find_symbol(const asm_ctx *ctx, const char *name);

/* Emit the assembled image as a CSUB text block into `out` (caller
 * buffer of `cap` bytes).  `name`/`types` become the `CSUB <name>
 * <types>` header.  Returns the number of bytes written, or -1 if the
 * buffer is too small.  Format matches tools/csub/mkcsub.sh. */
int asm_emit_csub(const asm_ctx *ctx, const char *name, const char *types,
                  char *out, size_t cap);

/* Emit the assembled image as a relocatable ELF32 object (ET_REL,
 * EM_ARM) that the FRANK OS loader can relocate and run.  A global FUNC
 * symbol named `main` (or `_start`) marks the entry.  Writes into `out`
 * (cap bytes); returns bytes written, or -1 if the buffer is too small.
 * `entry_name` may be NULL to default to "main". */
int asm_emit_elf(const asm_ctx *ctx, const char *entry_name,
                 uint8_t *out, size_t cap);

#endif /* ASM_THUMB_H */
