/*
 * frankos_libc.c — C library and hardware symbol stubs for Frank OS MMBasic app.
 *
 * PURPOSE:
 *   Frank OS apps are built as relocatable ELFs (-r partial link, no -lc/-lgcc).
 *   Any symbol that is UND (SHN_UNDEF, st_shndx=0) in a loaded section causes
 *   the Frank OS ELF loader to abort loading that entire section, which cascades
 *   to main() never being called.
 *
 *   MMBasic's core .text section (sec#1, ~331 KB) references ~220 external
 *   symbols — C library functions, FreeRTOS primitives, and hardware-specific
 *   variables that the PicoMite hardware layer provides.  All of these must be
 *   DEFINED in the ELF (or provided by frankos_alloc.c / frankos_stubs.c).
 *
 *   This file provides all remaining definitions.  It includes ONLY m-os-api.h
 *   (for sys_table access) plus standard C headers for type definitions.
 *   No MMBasic or pico SDK headers are included to avoid macro conflicts.
 *
 * sys_table slot reference (0x10FFF000 base):
 *   [2]   vTaskDelay              [29]  snprintf
 *   [32]  pvPortMalloc            [33]  vPortFree
 *   [60]  strcpy                  [62]  strlen
 *   [63]  strncpy                 [67]  vsnprintf
 *   [100] atoi                    [108] strcmp
 *   [109] strncmp                 [136] xTaskGetCurrentTaskHandle
 *   [142] memset                  [166] pvPortCalloc
 *   [167] memcpy                  [203] __sqrt (sqrtf/sqrt)
 *   [252] strcat                  [253] memcmp
 *   [279] __aeabi_ldivmod         [303] __realloc
 *   [309] __vsnprintf             [336] __vsscanf
 *   [427] xTimerCreate            [428] xTimerGenericCommandFromTask
 *   [430] xTaskGenericNotify      [431] ulTaskGenericNotifyTake
 *   [438] serial_printf
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* ── sys_table raw access (no m-os-api.h — it provides conflicting inlines) ── */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
#pragma GCC diagnostic ignored "-Wattributes"

/* Raw sys_table pointer at fixed address 0x10FFF000 */
#define _SYS_TABLE_BASE 0x10FFF000UL
static void * const * const _sys_table_ptrs = (void * const *)_SYS_TABLE_BASE;

/* Convenience accessor — compatible with original ST(N,T) usage */
#define ST(N,T)  ((T)_sys_table_ptrs[(N)])


/* ══════════════════════════════════════════════════════════════════════════
 * §1  Character classification table (_ctype_)
 *
 * Newlib's ctype macros use: (_ctype_+1)[c] & mask
 * Bits: _U=0x01 _L=0x02 _N=0x04 _S=0x08 _P=0x10 _C=0x20 _X=0x40 _B=0x80
 * Layout: [0]=EOF placeholder, [1..256]=chars 0x00..0xFF
 * ══════════════════════════════════════════════════════════════════════════ */

#define _U  0x01
#define _L  0x02
#define _N  0x04
#define _S  0x08
#define _P  0x10
#define _C  0x20
#define _X  0x40
#define _B  0x80

const char _ctype_[1 + 256] = {
    0,                                                          /* EOF */
    /* 0x00-0x07 */ _C, _C, _C, _C, _C, _C, _C, _C,
    /* 0x08-0x0F */ _C, _S|_B, _S, _S, _S, _S, _C, _C,
    /* 0x10-0x17 */ _C, _C, _C, _C, _C, _C, _C, _C,
    /* 0x18-0x1F */ _C, _C, _C, _C, _C, _C, _C, _C,
    /* 0x20-0x27 */ _S|_B, _P, _P, _P, _P, _P, _P, _P,
    /* 0x28-0x2F */ _P, _P, _P, _P, _P, _P, _P, _P,
    /* 0x30-0x37 */ _N, _N, _N, _N, _N, _N, _N, _N,
    /* 0x38-0x3F */ _N, _N, _P, _P, _P, _P, _P, _P,
    /* 0x40-0x47 */ _P, _U|_X, _U|_X, _U|_X, _U|_X, _U|_X, _U|_X, _U,
    /* 0x48-0x4F */ _U, _U, _U, _U, _U, _U, _U, _U,
    /* 0x50-0x57 */ _U, _U, _U, _U, _U, _U, _U, _U,
    /* 0x58-0x5F */ _U, _U, _U, _P, _P, _P, _P, _P,
    /* 0x60-0x67 */ _P, _L|_X, _L|_X, _L|_X, _L|_X, _L|_X, _L|_X, _L,
    /* 0x68-0x6F */ _L, _L, _L, _L, _L, _L, _L, _L,
    /* 0x70-0x77 */ _L, _L, _L, _L, _L, _L, _L, _L,
    /* 0x78-0x7F */ _L, _L, _L, _P, _P, _P, _P, _C,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};


/* ══════════════════════════════════════════════════════════════════════════
 * §2  String / memory functions via sys_table
 *
 * NOTE: memset, memcpy, strcpy, strcat, memmove are already provided by
 *       m-os-api-sdtfn.c (compiled via FRANKOS_SOURCES). Only define here
 *       what that file does NOT define.
 * ══════════════════════════════════════════════════════════════════════════ */

size_t strlen(const char *s) {
    typedef size_t(*fn)(const char*); return ST(62,fn)(s); }

char *strncpy(char *d, const char *s, size_t n) {
    typedef char*(*fn)(char*,const char*,size_t); return ST(63,fn)(d,s,n); }

int strcmp(const char *a, const char *b) {
    typedef int(*fn)(const char*,const char*); return ST(108,fn)(a,b); }

int strncmp(const char *a, const char *b, size_t n) {
    typedef int(*fn)(const char*,const char*,size_t); return ST(109,fn)(a,b,n); }

int memcmp(const void *a, const void *b, size_t n) {
    typedef int(*fn)(const void*,const void*,size_t); return ST(253,fn)(a,b,n); }


/* ══════════════════════════════════════════════════════════════════════════
 * §3  String functions not in sys_table — implemented in C
 * ══════════════════════════════════════════════════════════════════════════ */

char *strchr(const char *s, int c) {
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char*)s;
    return (c == '\0') ? (char*)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *found = NULL;
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) found = s;
    return (c == '\0') ? (char*)s : (char*)found;
}

char *strncat(char *d, const char *s, size_t n) {
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = '\0';
    return d;
}

/* Case-insensitive compare helper */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && *b && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b && lc((unsigned char)*a) == lc((unsigned char)*b))
        { a++; b++; n--; }
    return n ? lc((unsigned char)*a) - lc((unsigned char)*b) : 0;
}

char *stpcpy(char *d, const char *s) {
    while ((*d++ = *s++));
    return d - 1;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char*)s;
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; *s; s++) {
        const char *a;
        for (a = accept; *a && *a != *s; a++);
        if (!*a) break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; *s; s++) {
        const char *r;
        for (r = reject; *r && *r != *s; r++);
        if (*r) break;
        n++;
    }
    return n;
}


/* ══════════════════════════════════════════════════════════════════════════
 * §4  Number conversion
 * ══════════════════════════════════════════════════════════════════════════ */

int atoi(const char *s) {
    typedef int(*fn)(const char*); return ST(100,fn)(s); }

long atol(const char *s) { return (long)atoi(s); }

long long atoll(const char *s) {
    long long r = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r*10 + (*s++ - '0');
    return neg ? -r : r;
}

long strtol(const char *s, char **end, int base) {
    long r = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (!base) {
        if (*s == '0' && (s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (*s == '0') { base=8; s++; }
        else base=10;
    } else if (base==16 && *s=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
    while (1) {
        int d = (*s>='0'&&*s<='9') ? *s-'0' : (*s>='a'&&*s<='f') ? *s-'a'+10 :
                (*s>='A'&&*s<='F') ? *s-'A'+10 : -1;
        if (d < 0 || d >= base) break;
        r = r*base + d; s++;
    }
    if (end) *end = (char*)s;
    return neg ? -r : r;
}

long long strtoll(const char *s, char **end, int base) {
    long long r = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (!base) {
        if (*s=='0'&&(s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (*s=='0') { base=8; s++; }
        else base=10;
    } else if (base==16&&*s=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
    while (1) {
        int d = (*s>='0'&&*s<='9') ? *s-'0' : (*s>='a'&&*s<='f') ? *s-'a'+10 :
                (*s>='A'&&*s<='F') ? *s-'A'+10 : -1;
        if (d < 0 || d >= base) break;
        r = r*base + d; s++;
    }
    if (end) *end = (char*)s;
    return neg ? -r : r;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base); }

unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtoll(s, end, base); }

/* Simple strtod: handles sign, integer, fraction, exponent */
double strtod(const char *s, char **end) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    double r = 0.0;
    /* integer part */
    while (*s >= '0' && *s <= '9') r = r*10.0 + (*s++ - '0');
    /* fractional part */
    if (*s == '.') {
        double f = 0.1;
        s++;
        while (*s >= '0' && *s <= '9') { r += (*s++ - '0') * f; f *= 0.1; }
    }
    /* exponent */
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') exp = exp*10 + (*s++ - '0');
        double factor = 1.0;
        while (exp-- > 0) factor *= 10.0;
        if (eneg) r /= factor; else r *= factor;
    }
    if (end) *end = (char*)s;
    return neg ? -r : r;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }


/* ══════════════════════════════════════════════════════════════════════════
 * §5  Formatted I/O
 * ══════════════════════════════════════════════════════════════════════════ */

int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    typedef int(*fn)(char*,size_t,const char*,...);
    va_list ap; va_start(ap, fmt);
    typedef int(*vfn)(char*,size_t,const char*,__builtin_va_list);
    int r = ST(67,vfn)(buf, sz, fmt, ap);
    va_end(ap); return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    typedef int(*fn)(char*,size_t,const char*,__builtin_va_list);
    int r = ST(67,fn)(buf, 4096, fmt, ap);
    va_end(ap); return r;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    typedef int(*fn)(const char*,const char*,__builtin_va_list);
    int r = ST(336,fn)(str, fmt, ap);
    va_end(ap); return r;
}

int printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    typedef int(*vfn)(char*,size_t,const char*,__builtin_va_list);
    int r = ST(67,vfn)(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    typedef void(*gfn)(const char*,...);
    ST(438,gfn)("%s", buf);
    return r;
}


/* ══════════════════════════════════════════════════════════════════════════
 * §6  Math functions
 * ══════════════════════════════════════════════════════════════════════════ */

float  sqrtf(float x) {
    typedef double(*fn)(double); return (float)ST(203,fn)((double)x); }
double sqrt(double x) {
    typedef double(*fn)(double); return ST(203,fn)(x); }

/* ARM EABI 64-bit division (__aeabi_ldivmod / __aeabi_uldivmod) comes
 * from api/m-os-api-math.c, which implements the SPECIAL EABI register
 * convention (quotient r0:r1 AND remainder r2:r3) in naked asm.  A
 * struct-return C wrapper used to live here — it shifted every
 * register and corrupted every MMBasic '\' and MOD on 64-bit values
 * (the tetris "lvl = 0x03333F40xxxxxxxx" bug). */

/* Complex math stubs — MMBasic's complex support rarely used at startup */
#define CSTUB_F(name)  float  name##f(float _Complex z) { (void)z; return 0.f; }
#define CSTUB_D(name)  double name  (double _Complex z) { (void)z; return 0.; }
#define CSTUB_CF(name) float  _Complex name##f(float _Complex z) { (void)z; return 0; }
#define CSTUB_CD(name) double _Complex name  (double _Complex z) { (void)z; return 0; }

CSTUB_F(cabs)  CSTUB_D(cabs)
CSTUB_CF(cacos) CSTUB_CF(cacosh) CSTUB_CF(casin)  CSTUB_CF(casinh)
CSTUB_CF(catan) CSTUB_CF(catanh) CSTUB_CF(ccos)   CSTUB_CF(ccosh)
CSTUB_CF(clog)  CSTUB_CF(cproj)  CSTUB_CF(csin)
CSTUB_CF(csinh) CSTUB_CF(csqrt)  CSTUB_CF(ctan)   CSTUB_CF(ctanh)
CSTUB_CD(cexp)
CSTUB_CF(cexp)

float  cargf(float  _Complex z) { (void)z; return 0.f; }
/* cpowf takes 2 args (base, exponent) */
float _Complex cpowf(float _Complex a, float _Complex b) { (void)a;(void)b; return 0; }

/* GCC soft-float complex multiply/divide stubs */
double _Complex __muldc3(double a, double b, double c, double d)
    { (void)a;(void)b;(void)c;(void)d; return (double _Complex)0; }
double _Complex __divdc3(double a, double b, double c, double d)
    { (void)a;(void)b;(void)c;(void)d; return (double _Complex)0; }
float _Complex  __mulsc3(float a, float b, float c, float d)
    { (void)a;(void)b;(void)c;(void)d; return (float _Complex)0; }
float _Complex  __divsc3(float a, float b, float c, float d)
    { (void)a;(void)b;(void)c;(void)d; return (float _Complex)0; }


/* ══════════════════════════════════════════════════════════════════════════
 * §7  FreeRTOS wrappers
 *
 * MMBasic source files include FreeRTOS headers directly (not m-os-api.h),
 * so they generate real external calls to these symbols.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Types for FreeRTOS primitives */
typedef void*  TimerHandle_t;
typedef void*  TaskHandle_t;
typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;

void vTaskDelay(TickType_t ticks) {
    typedef void(*fn)(TickType_t); ST(2,fn)(ticks); }

TimerHandle_t xTimerCreate(const char *name, TickType_t period,
                            uint32_t autoreload, void *id,
                            void (*cb)(TimerHandle_t)) {
    typedef TimerHandle_t(*fn)(const char*,TickType_t,uint32_t,void*,void(*)(TimerHandle_t));
    return ST(427,fn)(name, period, autoreload, id, cb);
}

/* xTimerGenericCommandFromTask — underlying for Start/Stop/Delete */
static BaseType_t timer_cmd(TimerHandle_t t, BaseType_t cmd,
                             TickType_t optval, TickType_t wait) {
    typedef BaseType_t(*fn)(TimerHandle_t,BaseType_t,TickType_t,BaseType_t*,TickType_t);
    return ST(428,fn)(t, cmd, optval, NULL, wait);
}

/* FreeRTOS timer command IDs (from timers.h) */
#define TMR_START   1
#define TMR_STOP    3
#define TMR_DELETE  5

BaseType_t xTimerStart(TimerHandle_t t, TickType_t wait) {
    typedef TickType_t(*tcfn)(void);
    TickType_t now = ST(17,tcfn)();        /* xTaskGetTickCount = sys_table[17] */
    return timer_cmd(t, TMR_START, now, wait);
}
BaseType_t xTimerStop  (TimerHandle_t t, TickType_t wait) { return timer_cmd(t, TMR_STOP,   0, wait); }
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t wait) { return timer_cmd(t, TMR_DELETE, 0, wait); }

/* ulTaskNotifyTake — route through sys_table[431] ulTaskGenericNotifyTake */
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait) {
    typedef uint32_t(*fn)(uint32_t,BaseType_t,TickType_t);
    return ST(431,fn)(0, clearOnExit, wait);
}

/* xTaskNotifyGive — route through sys_table[430] xTaskGenericNotify */
BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    typedef BaseType_t(*fn)(TaskHandle_t,uint32_t,uint32_t,int,BaseType_t*);
    /* eAction=2 = eIncrement for notify-give equivalent */
    return ST(430,fn)(task, 0, 0, 2, NULL);
}


/* ══════════════════════════════════════════════════════════════════════════
 * §8  setjmp / longjmp (ARM Cortex-M33 Thumb-2 assembly)
 *
 * Saves/restores r4–r11, sp, lr, d8–d15 (VFP callee-saved) in the jmp_buf.
 * Layout (byte offsets):
 *   [0..31]    r4-r11   (8 × 4 = 32 bytes)
 *   [32..35]   sp       (4 bytes)
 *   [36..39]   lr       (4 bytes)
 *   [40..103]  d8-d15   (8 × 8 = 64 bytes)  — VFP callee-saved
 * Total = 104 bytes; fits in newlib jmp_buf (long long[20] = 160 bytes).
 *
 * With -mfloat-abi=softfp and __ARM_FP=4, the compiler generates SP VFP
 * instructions and d8-d15 (aliases for s16-s31) are callee-saved per AAPCS.
 * ══════════════════════════════════════════════════════════════════════════ */

/* jmp_buf = long long[20] in ARM newlib with FP; argument decays to pointer */
__attribute__((naked, noinline)) int setjmp(long long *env)
{
    __asm__ volatile (
        "stmia  r0, {r4-r11}   \n"  /* store r4..r11 → bytes 0..31   */
        "str    sp, [r0, #32]  \n"  /* store sp      → bytes 32..35  */
        "str    lr, [r0, #36]  \n"  /* store lr      → bytes 36..39  */
        "add    r1, r0, #40    \n"
        "vstmia r1, {d8-d15}   \n"  /* store d8..d15 → bytes 40..103 */
        "movs   r0, #0         \n"  /* return 0                      */
        "bx     lr             \n"
        ::: "memory"
    );
}

__attribute__((naked, noinline)) void longjmp(long long *env, int val)
{
    __asm__ volatile (
        /* if val==0, use 1 (setjmp must return non-zero on longjmp) */
        "cmp    r1, #0         \n"
        "it     eq             \n"
        "moveq  r1, #1         \n"
        "ldmia  r0, {r4-r11}   \n"  /* restore r4..r11               */
        "ldr    sp, [r0, #32]  \n"  /* restore sp                    */
        "ldr    lr, [r0, #36]  \n"  /* restore lr                    */
        "add    r2, r0, #40    \n"
        "vldmia r2, {d8-d15}   \n"  /* restore d8..d15               */
        "mov    r0, r1         \n"  /* return val                    */
        "bx     lr             \n"
        ::: "memory"
    );
}


/* ══════════════════════════════════════════════════════════════════════════
 * §9  Time stubs (define struct tm / time_t locally; no time.h included)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Minimal struct tm and time_t — must match newlib layout */
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
typedef long time_t;

struct tm *gmtime(const time_t *t) {
    (void)t;
    static struct tm z = {0};
    return &z;
}

struct tm *localtime_r(const time_t *t, struct tm *result) {
    (void)t;
    if (result) *result = (struct tm){0};
    return result;
}

time_t mktime(struct tm *t) { (void)t; return (time_t)0; }
time_t timegm(struct tm *t) { (void)t; return (time_t)0; }


/* ══════════════════════════════════════════════════════════════════════════
 * §10  FatFS callbacks (disk_ioctl and get_fattime)
 *      Disk I/O is handled by frankos_ff.c via FatFS; these stubs cover
 *      any direct references from compiled-in MMBasic code.
 * ══════════════════════════════════════════════════════════════════════════ */

int disk_ioctl(uint8_t drv, uint8_t cmd, void *buf) {
    (void)drv; (void)cmd; (void)buf; return 1; /* RES_ERROR */ }

uint32_t get_fattime(void) { return 0; }    /* 1980-01-01 00:00:00 */


/* ══════════════════════════════════════════════════════════════════════════
 * §11  mallinfo / memory info stub
 * ══════════════════════════════════════════════════════════════════════════ */

struct mallinfo { size_t arena, ordblks, smblks, hblkhd, hblks,
                  usmblks, fsmblks, uordblks, fordblks, keepcost; };
struct mallinfo mallinfo(void) {
    struct mallinfo z = {0}; return z; }


/* ══════════════════════════════════════════════════════════════════════════
 * §12  C runtime / linker symbol placeholders
 *
 * These are normally provided by the linker script in a native build.
 * For Frank OS apps they have no meaning; define them as empty arrays
 * (size 0, address ≠ NULL) so any code that checks them doesn't crash.
 * ══════════════════════════════════════════════════════════════════════════ */

extern void (*__preinit_array_start [])(void) __attribute__((weak, visibility("hidden")));
extern void (*__preinit_array_end   [])(void) __attribute__((weak, visibility("hidden")));
extern void (*__init_array_start    [])(void) __attribute__((weak, visibility("hidden")));
extern void (*__init_array_end      [])(void) __attribute__((weak, visibility("hidden")));
extern void (*__mutex_array_start   [])(void) __attribute__((weak, visibility("hidden")));
extern void (*__mutex_array_end     [])(void) __attribute__((weak, visibility("hidden")));

/* Actual definitions (zero-length arrays as placeholders): */
void (*__preinit_array_start[])(void) = {};
void (*__preinit_array_end  [])(void) = {};
void (*__init_array_start   [])(void) = {};
void (*__init_array_end     [])(void) = {};
void (*__mutex_array_start  [])(void) = {};
void (*__mutex_array_end    [])(void) = {};

/* Linker/binary layout symbols — not meaningful in PSRAM-loaded apps */
char __StackLimit[0];
char __StackOneBottom[0];
char __bss_end__[0];
char __flash_binary_start[0];
char __flash_binary_end[0];
char __heap_start[0];
char end[0];


/* ══════════════════════════════════════════════════════════════════════════
 * §13  MMBasic hardware variable / function stubs
 *
 * MMBasic's core .text references hardware-specific variables and functions
 * from the PicoMite hardware layer.  On Frank OS these features are not
 * available; stub them with sensible zero / no-op values so that the BASIC
 * interpreter's main loop and text output still work.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Audio — CurrentlyPlaying / PlayingStr / CloseAudio moved to
 * frankos_audio.c (real OS-mixer implementation) */
volatile int  AUDIO_WRAP           = 0;
volatile int  AudioOutput          = 0;
char         *WAVfilename          = NULL;

/* Backlight / display hardware */
volatile int  BacklightChannel     = 0;
volatile int  BacklightSlice       = 0;
volatile int  KeyboardlightSlice   = 0;
volatile int  CameraSlice          = 0;
volatile int  SSD1963data          = 0;
/* Declared `extern void DisplayNotSet(void)` — used as a sentinel ADDRESS
 * in ReadBuffer comparisons AND installed into Draw fn pointers by
 * OPTION LCDPANEL NOCONSOLE, so it must be a real (no-op) function. */
void DisplayNotSet(void)           { }
void SetBacklightSSD1963(int v)    { (void)v; }
void Display_Refresh(void)         { }

/* CFunction / External C function support */
volatile int  CFuncInt1  = 0, CFuncInt2  = 0;
volatile int  CFuncInt3  = 0, CFuncInt4  = 0;
volatile int  CFuncmSec  = 0;
void CallCFuncInt1(void) { }
void CallCFuncInt2(void) { }
void CallCFuncInt3(void) { }
void CallCFuncInt4(void) { }
void CallCFuncmSec(void) { }
void CallCFunction(int n, ...) { (void)n; }
void CallExecuteProgram(void *p) { (void)p; }
void CNInterrupt(void) { }
void CANInterrupt(void) { }

/* Display configuration stubs */
void ConfigDisplay222(void)        { }
void ConfigDisplayI2C(void)        { }
void ConfigDisplaySPI(void)        { }
void ConfigDisplaySSD(void)        { }
void ConfigTouch(void)             { }

/* Draw buffer stubs (hardware LCD) */
typedef void *HSBUF;  /* opaque handle */
void DrawBLITBuffer320(void *a, void *b, int c, int d)   { (void)a;(void)b;(void)c;(void)d; }
void DrawBLITBufferSSD1963(void *a, int b, int c)        { (void)a;(void)b;(void)c; }
void DrawBitmap320(int a, int b, int c, int d, int e, int f, int g, void *h)
                                                          { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }
void DrawBitmapSSD1963(int a, int b, int c, int d, int e, int f, int g, void *h)
                                                          { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }
void DrawBuffer320(int a, int b, int c, int d, void *e)  { (void)a;(void)b;(void)c;(void)d;(void)e; }
void DrawBufferSSD1963(int a, int b, int c, int d, void *e){ (void)a;(void)b;(void)c;(void)d;(void)e; }
void ReadBLITBuffer320(void *a, void *b, int c, int d)   { (void)a;(void)b;(void)c;(void)d; }
void ReadBLITBufferSSD1963(void *a, int b, int c)        { (void)a;(void)b;(void)c; }
void ReadBuffer16(int a, int b, int c, int d, void *e)   { (void)a;(void)b;(void)c;(void)d;(void)e; }
void ReadBuffer320(int a, int b, int c, int d, void *e)  { (void)a;(void)b;(void)c;(void)d;(void)e; }
void ReadBufferSSD1963(int a, int b, int c, int d, void *e){ (void)a;(void)b;(void)c;(void)d;(void)e; }
void DrawFmtBox(int a, int b, int c, int d, int e, int f){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void DrawRectangle320(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void DrawRectangleSSD1963(int a, int b, int c, int d, int e){ (void)a;(void)b;(void)c;(void)d;(void)e; }

void DelayedDrawFmtBox(int a, int b, int c, int d, int e, int f)
                                                          { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void DelayedDrawKeyboard(int a)    { (void)a; }

void ScrollLCDMEM332(int a, int b) { (void)a; (void)b; }
void ScrollLCDSPISCR(int a, int b) { (void)a; (void)b; }
volatile int ScrollStart           = 0;

/* I2C hardware */
volatile int  I2C0locked           = 0;
volatile int  I2C1locked           = 0;
volatile int  I2C2_Status          = 0;
volatile int  I2C_Status           = 0;
void I2C_Send_Command(int cmd)     { (void)cmd; }
void I2C_Slave_Send_IntLine(void)  { }
void i2c_disable(void)             { }
void i2c2_disable(void)            { }
volatile int mmI2Cvalue            = 0;

/* SPI hardware */
volatile int  SPI0locked           = 0;
volatile int  SPI1locked           = 0;
void SPI2Close(void)               { }
void SPIClose(void)                { }
void SPISpeedSet(int a)            { (void)a; }
void spi_write_command(int a)      { (void)a; }
void spi_write_data(int a)         { (void)a; }

/* Serial */
void SerialOpen(int a, int b, int c, int d, int e, int f)
                                   { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
/* These are FUNCTIONS in Serial.h (int fn(int comnbr)) — they were once
 * stubbed as int VARIABLES here, so callers (e.g. cmd_close's TX-drain
 * loop) branched to the variable's address and executed data: blue
 * screen on every CLOSE #n.  This file doesn't include Serial.h, so the
 * compiler never saw the conflict. */
int SerialRxStatus(int comnbr)     { (void)comnbr; return 0; }  /* no RX data    */
int SerialTxStatus(int comnbr)     { (void)comnbr; return 0; }  /* TX buffer idle */
void com1_interrupt(void)          { }
void com2_interrupt(void)          { }
volatile int com1_ilevel           = 0;
volatile int com2_ilevel           = 0;
volatile int com3_ilevel           = 0;
void com3_interrupt(void)          { }

/* RTC */
void RtcGetTime(void)              { }

/* GPIO / DMA */
volatile int  dma_rx_chan          = -1;
volatile int  dma_rx_chan2         = -1;
volatile int  dma_rx_pio           = 0;
volatile int  dma_rx_sm            = 0;
volatile int  dma_tx_chan          = -1;
volatile int  dma_tx_chan2         = -1;
volatile int  dma_tx_pio           = 0;
volatile int  dma_tx_sm            = 0;
void piointerrupt(void)            { }
void *piomap                       = NULL;

/* DS18B20 temperature sensor */
volatile int ds18b20Timers         = 0;

/* PS/2 keyboard */
volatile int PS2code               = 0;
volatile int PS2int                = 0;

/* GUI */
volatile int  GuiIntDownVector     = 0;
volatile int  GuiIntUpVector       = 0;
void gui_int_down(void)           { }
void gui_int_up(void)             { }

/* GPS */
void     GPSadjust(void)           { }
volatile int GPSchannel            = -1;
volatile int GPSfix                = 0;
volatile int GPSsatellites         = 0;
volatile int GPSvalid              = 0;
char      gpsbuf[1]                = {0};
char      gpsbuf1[1]               = {0};
volatile int gpscount              = 0;
volatile int gpscurrent            = 0;
char     *gpsmonitor               = NULL;

/* Wii / Nunchuck */
void WiiReceive(void)             { }
void classic1(void)               { }
void classicread(void)            { }
void cmd_Classic(void)            { }
void cmd_Nunchuck(void)           { }
void nunInterruptc(void)          { }
char  nunbuff[1]                  = {0};
volatile int nunchuck1            = 0;
void nunchuckread(void)           { }
volatile int nunfoundc            = 0;
void *nunstruct                   = NULL;

/* MMBasic colour table (needed by some display routines) */

/* Camera */
void cameraclose(void)            { }
void cmd_camera(void)             { }

/* Backlight control */
void set_kbd_backlight(int v)     { (void)v; }
void set_lcd_backlight(int v)     { (void)v; }

/* Raycaster / misc */
void ray_close(void)              { }
int  read_battery(void)           { return 0; }
int  read_biosversion(void)       { return 0; }

/* Stepper motor */
volatile int stepper_initialized  = 0;

/* Pattern matching / pin search (MMBasic internals) */
/* pattern_matching: real implementation in frankos_ff.c */
void pinsearch(void)              { }

/* dirOK: directory-change flag used by file I/O commands */
volatile int dirOK                = 0;

/* display_details: MMBasic display configuration struct pointer.
 * Pointing to NULL causes the display code to treat it as "no display",
 * which is correct since we provide our own window-based rendering. */
void *display_details             = NULL;

/* mouse */
void mouse0close(void)            { }

/* nextline: used by the BASIC interpreter for file I/O line tracking */
char nextline[1]                  = {0};

/* mmOWvalue: OneWire current value */
volatile int mmOWvalue            = 0;

/* WriteComand / WriteData: low-level LCD SPI writes */
void WriteComand(int cmd)         { (void)cmd; }
void WriteData(int dat)           { (void)dat; }

/* littlefs stubs — not used on Frank OS (SD card via FatFS instead) */
int lfs_dir_close(void *d)        { (void)d; return -1; }
int lfs_dir_open(void *l, void *d, const char *p) { (void)l;(void)d;(void)p; return -1; }
int lfs_dir_read(void *l, void *d, void *e)       { (void)l;(void)d;(void)e; return -1; }
int lfs_file_close(void *l, void *f)              { (void)l;(void)f; return -1; }
int lfs_file_open(void *l, void *f, const char *p, int fl){ (void)l;(void)f;(void)p;(void)fl; return -1; }
int lfs_file_read(void *l, void *f, void *b, size_t s)   { (void)l;(void)f;(void)b;(void)s; return -1; }
int lfs_file_rewind(void *l, void *f)             { (void)l;(void)f; return -1; }
int lfs_file_seek(void *l, void *f, int off, int wh)      { (void)l;(void)f;(void)off;(void)wh; return -1; }
int lfs_file_size(void *l, void *f)               { (void)l;(void)f; return -1; }
int lfs_file_sync(void *l, void *f)               { (void)l;(void)f; return -1; }
int lfs_file_tell(void *l, void *f)               { (void)l;(void)f; return -1; }
int lfs_file_write(void *l, void *f, const void *b, size_t s){ (void)l;(void)f;(void)b;(void)s; return -1; }
int lfs_format(void *l, const void *cfg)          { (void)l;(void)cfg; return -1; }
int lfs_fs_size(void *l)                          { (void)l; return -1; }
int lfs_getattr(void *l, const char *p, uint8_t t, void *b, size_t s){ (void)l;(void)p;(void)t;(void)b;(void)s; return -1; }
int lfs_mkdir(void *l, const char *p)             { (void)l;(void)p; return -1; }
int lfs_mount(void *l, const void *cfg)           { (void)l;(void)cfg; return -1; }
int lfs_remove(void *l, const char *p)            { (void)l;(void)p; return -1; }
int lfs_removeattr(void *l, const char *p, uint8_t t){ (void)l;(void)p;(void)t; return -1; }
int lfs_rename(void *l, const char *op, const char *np){ (void)l;(void)op;(void)np; return -1; }
int lfs_setattr(void *l, const char *p, uint8_t t, const void *b, size_t s){ (void)l;(void)p;(void)t;(void)b;(void)s; return -1; }
int lfs_stat(void *l, const char *p, void *i)     { (void)l;(void)p;(void)i; return -1; }
int lfs_unmount(void *l)                          { (void)l; return -1; }

/* ══════════════════════════════════════════════════════════════════════════
 * §14  Remaining UND stubs — display commands, timer handles, system symbols
 *
 * These are referenced from .rel.rodata (display command dispatch table),
 * .rel.time_critical.*, .rel.reset, and .rel.binary_info_header sections.
 * They must all be defined to prevent the Frank OS ELF loader from aborting
 * those sections due to SHN_UNDEF relocations.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Display / graphics commands (stub: BASIC command handlers) ─────────── */
void cmd_pio(void)       {}
void cmd_ray(void)       {}
void cmd_rtc(void)       {}

/* ── BASIC built-in functions (stub: return nothing) ────────────────────── */
void fun_ray(void)       {}

/* ── Colour look-up table initializers ───────────────────────────────────── */
void init_RGB332_to_RGB565_LUT(void)    {}
void init_RGB332_to_RGB888_LUT(void)    {}
void init_RGB332_to_RGB888_LUT_SSD(void){}

/* ── Touch / GUI event functions ─────────────────────────────────────────── */
void CheckGui(void)             {}
void CheckGuiFlag(void)         {}
void CheckI2CKeyboard(void)     {}
void CheckKbdBacklight(void)    {}
void CheckLcdBacklight(void)    {}
void CheckPicoCalcKeyboard(void){}
void MNInterrupt(void)          {}
void MOUSE_CLOCK(void)          {}
void ProcessTouch(void)         {}
void TOUCH_GETIRQTRIS(void)     {}
volatile int TouchState = 0;
volatile int TouchDown  = 0;
volatile int TouchUp    = 0;

/* ── Timer handles (FreeRTOS TimerHandle_t, NULL = no timer) ────────────── */
void *ClickTimer  = NULL;
void *CursorTimer = NULL;
void *TouchTimer  = NULL;

/* ── I2C slave receive (not needed on Frank OS) ─────────────────────────── */
void I2C2_Slave_Receive_IntLine(void) {}
void I2C2_Slave_Send_IntLine(void)    {}
void I2C_Slave_Receive_IntLine(void)  {}

/* ── Wii / Nunchuck send, controller polling ────────────────────────────── */
void WiiSend(void)       {}
void classicproc(void)   {}
/* checkWAVinput moved to frankos_audio.c */
void nunproc(void)       {}
void processgps(void)    {}
void readcontroller(void){}

/* ── Stepper motor ───────────────────────────────────────────────────────── */
void stepper_abort_to_safe_state_on_error(void) {}
volatile int stepper_gcode_buffer_space = 0;

/* ── Merge-done callback (used by some display refresh paths) ───────────── */

/* ── Pico SDK / linker symbols required by .reset handler ───────────────── */
/* These are normally produced by the pico-sdk linker script. On Frank OS we
   provide zero-width placeholders whose address is used (never the data). */
char __StackTop[0];
char __binary_info_start[0];
char __binary_info_end[0];
char __bss_start__[0];
char __data_start__[0];
char __data_end__[0];
char __etext[0];
char __scratch_x_start__[0];
char __scratch_x_end__[0];
char __scratch_x_source__[0];
char __scratch_y_start__[0];
char __scratch_y_end__[0];
char __scratch_y_source__[0];


/* ── get_rand_32 override ───────────────────────────────────────────────────
 * fun_rnd (RND) and MATHS.c call pico-sdk get_rand_32(), whose pico_rand
 * implementation takes a HARDWARE SIO spinlock and reads ROSC/TRNG entropy
 * registers — physical resources shared with the running OS.  Executed from
 * an app, that spins forever against the OS (silent full-system wedge, the
 * "RUN with RND wedges the box" bug).  This object-file definition takes
 * precedence over the SDK library member at link time, so pico_rand is
 * never pulled in.  xorshift32 seeded from the hardware us-timer. */
unsigned int __wrap_get_rand_32(void)
{
    static unsigned int s;
    if (s == 0) {
        s = *(volatile unsigned int *)0x400B0028u;   /* TIMERAWL */
        if (s == 0) s = 0x6b7ddeadu;                 /* never zero */
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
