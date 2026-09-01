/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// use it to resolve issues like memset and/or memcpy are not found on elf32 obj execution attempt
#include "m-os-api.h"
float __aeabi_fmul(float x, float y) { //         single-precision multiplication
    typedef float (*fn)(float, float);
    return ((fn)_sys_table_ptrs[210])(x, y);
}

float __aeabi_i2f(int x) { //                   integer to float (single precision) conversion
    typedef float (*fn)(int);
    return ((fn)_sys_table_ptrs[211])(x);
}

float __aeabi_fadd(float x, float y) { //         single-precision addition
    typedef float (*fn)(float, float);
    return ((fn)_sys_table_ptrs[212])(x, y);
}

float __aeabi_fsub(float x, float y) { //         single-precision subtraction
    typedef float (*fn)(float, float);
    return ((fn)_sys_table_ptrs[213])(x, y);
}

float __aeabi_fdiv(float n, float d) { //    single-precision division, n / d
    typedef float (*fn)(float, float);
    return ((fn)_sys_table_ptrs[214])(n, d);
}

int __aeabi_fcmpge(float a, float b) { //        result (1, 0) denotes (>=, ?<) [2], use for C >=
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[215])(a, b);
}

/* ── ARM EABI division helpers ──────────────────────────────────────────
 * The EABI divmod helpers use a SPECIAL calling convention that a C
 * function cannot express: __aeabi_idivmod returns quotient in r0 AND
 * remainder in r1; __aeabi_(u)ldivmod returns quotient in r0:r1 AND
 * remainder in r2:r3.  The previous C wrappers returned only one value,
 * leaving the remainder register(s) as garbage — every MOD on the
 * routed type was silently corrupted (this was the MMBASIC tetris
 * "lvl = 0x03333F40xxxxxxxx" bug).  Division needs no OS services:
 * implement it self-contained.  Cortex-M33 has hardware sdiv/udiv for
 * the 32-bit forms; the 64-bit core is a shift-subtract loop (which
 * must not itself use C '/' or '%', or GCC re-emits these helpers). */
__attribute__((naked)) int __aeabi_idivmod(int x, int y) {
    __asm volatile(
        "sdiv r3, r0, r1  \n"
        "mls  r1, r3, r1, r0 \n"
        "mov  r0, r3      \n"
        "bx   lr          \n");
}
__attribute__((naked)) int __aeabi_idiv(int x, int y) {
    __asm volatile(
        "sdiv r0, r0, r1  \n"
        "bx   lr          \n");
}
double __aeabi_f2d(float x) {
    typedef double (*fn)(float);
    return ((fn)_sys_table_ptrs[218])(x);
}
float __aeabi_d2f(double x) {
    typedef float (*fn)(double);
    return ((fn)_sys_table_ptrs[219])(x);
}
int __aeabi_f2iz(float x) { //                     float (single precision) to integer C-style conversion [3]
    typedef int (*fn)(float);
    return ((fn)_sys_table_ptrs[220])(x);
}
int __aeabi_fcmplt(float x, float y) { //        result (1, 0) denotes (<, ?>=) [2], use for C <
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[221])(x, y);
}
double __aeabi_dsub(double x, double y) { //     double-precision subtraction, x - y
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[222])(x, y);
}
int __aeabi_d2iz(double x) { //                     double (double precision) to integer C-style conversion [3]
    typedef int (*fn)(double);
    return ((fn)_sys_table_ptrs[223])(x);
}
int __aeabi_fcmpeq(float x, float y) { //         result (1, 0) denotes (=, ?<>) [2], use for C == and !=
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[224])(x, y);
}
int __aeabi_fcmpun(float x, float y) { //         result (1, 0) denotes (?, <=>) [2], use for C99 isunordered()
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[225])(x, y);
}
int __aeabi_fcmpgt(float x, float y) { //         result (1, 0) denotes (>, ?<=) [2], use for C >
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[226])(x, y);
}
int __aeabi_dcmpge(double x, double y) { //         result (1, 0) denotes (>=, ?<) [2], use for C >=
    typedef int (*fn)(double, double);
    return ((fn)_sys_table_ptrs[227])(x, y);
}
unsigned __aeabi_uidiv(unsigned x, unsigned y) {
    typedef int (*fn)(unsigned, unsigned);
    return ((fn)_sys_table_ptrs[228])(x, y);
}
float __aeabi_ui2f(unsigned x) {
    typedef float (*fn)(unsigned);
    return ((fn)_sys_table_ptrs[229])(x);
}
unsigned __aeabi_f2uiz(float x) { //             float (single precision) to unsigned C-style conversion [3]
    typedef unsigned (*fn)(float);
    return ((fn)_sys_table_ptrs[230])(x);
}
int __aeabi_fcmple(float x, float y) { //         result (1, 0) denotes (<=, ?>) [2], use for C <=
    typedef int (*fn)(float, float);
    return ((fn)_sys_table_ptrs[231])(x, y);
}
/* Quotient r0 AND remainder r1 — see the EABI note above __aeabi_idivmod. */
__attribute__((naked)) unsigned __aeabi_uidivmod(unsigned x, unsigned y) {
    __asm volatile(
        "udiv r3, r0, r1  \n"
        "mls  r1, r3, r1, r0 \n"
        "mov  r0, r3      \n"
        "bx   lr          \n");
}
double __aeabi_dmul(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[245])(x, y);
}
double __aeabi_ddiv(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[246])(x, y);
}
double __aeabi_dadd(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[247])(x, y);
}
double __aeabi_i2d(int x) {
    typedef double (*fn)(int);
    return ((fn)_sys_table_ptrs[248])(x);
}
double __aeabi_dcmpeq(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[249])(x, y);
}
double __aeabi_ui2d(unsigned x) {
    typedef double (*fn)(unsigned);
    return ((fn)_sys_table_ptrs[250])(x);
}
double __aeabi_dcmplt(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[251])(x, y);
}
double __aeabi_dcmpgt(double x, double y) {
    typedef double (*fn)(double, double);
    return ((fn)_sys_table_ptrs[251])(y, x);
}
unsigned __aeabi_d2uiz(double x) {
    typedef unsigned (*fn)(double);
    return ((fn)_sys_table_ptrs[256])(x);
}
int __clzsi2 (unsigned int a ) {
    typedef int (*fn)(unsigned int);
    return ((fn)_sys_table_ptrs[258])(a);
}
long long __aeabi_lmul(long long x, long long y) {
    typedef long long (*fn)(long long, long long);
    return ((fn)_sys_table_ptrs[259])(x, y);
}
/* 64-bit divide core: shift-subtract, no C '/' or '%' (which would
 * re-emit the very helpers we are implementing).  Extern (not static)
 * because the naked shims below reference the symbols from asm. */
unsigned long long __mosapi_udivmod64(unsigned long long n, unsigned long long d,
                                      unsigned long long *rem) {
    unsigned long long q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    if (rem) *rem = r;
    return q;
}

long long __mosapi_divmod64(long long n, long long d, long long *rem) {
    int qneg = (n < 0) != (d < 0);
    int rneg = (n < 0);
    unsigned long long un = (n < 0) ? 0ull - (unsigned long long)n
                                    : (unsigned long long)n;
    unsigned long long ud = (d < 0) ? 0ull - (unsigned long long)d
                                    : (unsigned long long)d;
    unsigned long long ur;
    unsigned long long uq = __mosapi_udivmod64(un, ud, &ur);
    if (rem) *rem = rneg ? -(long long)ur : (long long)ur;
    return qneg ? -(long long)uq : (long long)uq;
}

/* EABI shims: args r0:r1 / r2:r3, quotient OUT in r0:r1 AND remainder
 * OUT in r2:r3.  The core's rem pointer is the 5th argument slot, which
 * AAPCS passes on the stack. */
__attribute__((naked)) unsigned long long
__aeabi_uldivmod(unsigned long long x, unsigned long long y) {
    __asm volatile(
        "push {r4, lr}          \n"
        "sub  sp, #16           \n"
        "add  r4, sp, #8        \n"
        "str  r4, [sp, #0]      \n"
        "bl   __mosapi_udivmod64 \n"
        "ldrd r2, r3, [sp, #8]  \n"
        "add  sp, #16           \n"
        "pop  {r4, pc}          \n");
}

__attribute__((naked)) long long
__aeabi_ldivmod(long long x, long long y) {
    __asm volatile(
        "push {r4, lr}          \n"
        "sub  sp, #16           \n"
        "add  r4, sp, #8        \n"
        "str  r4, [sp, #0]      \n"
        "bl   __mosapi_divmod64 \n"
        "ldrd r2, r3, [sp, #8]  \n"
        "add  sp, #16           \n"
        "pop  {r4, pc}          \n");
}
