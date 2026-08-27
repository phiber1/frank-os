/*
 * frankos_ff.c — FatFS function implementations for the Frank OS port of MMBasic
 *
 * All functions are routed through the Frank OS sys_table (runtime function
 * pointer table at 0x20000000).  Functions not present in the table are
 * stubbed as no-ops / always-success.
 *
 * IMPORTANT: this file must NOT include m-os-api.h or m-os-api-ff.h.  Those
 * headers define macros like  #define fclose(f) f_close(f)  which conflict
 * with <stdio.h> / pico/stdlib.h already included by other translation units.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stddef.h>

/* "ff.h" resolves to frankos/ff.h which includes the OS driver tree's
 * ff.h + ffconf.h — the app-side structs (FIL, DIR, FILINFO, FATFS) are
 * layout-identical to what the OS FatFS engine expects. */
#include "ff.h"
#include <string.h>

/* ── sys_table access ───────────────────────────────────────────────────────
 *
 * Frank OS places its system-call table in flash at 0x10FFF000
 * (M_OS_API_SYS_TABLE_BASE in api/m-os-api.h: 0x10000000 + 16MB - 4KB).
 * Each entry is a function pointer; slot order verified against
 * src/sys_table.c.  NOTE: 0x20000000 is OS SRAM, NOT a table mirror —
 * reading "pointers" there produced wild jumps (the FILES blue screen:
 * PC=0x10000116 via garbage pointer 0x10000111 from slot 54's offset).
 */
static void * const * const _sys_tbl =
    (void * const *)(0x10000000UL + (16UL << 20) - (4UL << 10));

/* sys_table index constants (must match m-os-api-ff.h) */
#define SYS_F_OPEN      46
#define SYS_F_CLOSE     47
#define SYS_F_WRITE     48
#define SYS_F_READ      49
#define SYS_F_STAT      50
#define SYS_F_LSEEK     51
#define SYS_F_TRUNCATE  52
#define SYS_F_SYNC      53
#define SYS_F_OPENDIR   54
#define SYS_F_CLOSEDIR  55
#define SYS_F_READDIR   56
#define SYS_F_MKDIR     57   /* order verified against src/sys_table.c — */
#define SYS_F_UNLINK    58   /* m-os-api-ff.h's comments are NOT the truth */
#define SYS_F_RENAME    59
#define SYS_F_GETFREE   61

/* ── Core file operations ───────────────────────────────────────────────── */

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    typedef FRESULT (*fn_t)(FIL *, const TCHAR *, BYTE);
    return ((fn_t)_sys_tbl[SYS_F_OPEN])(fp, path, mode);
}

FRESULT f_close(FIL *fp)
{
    typedef FRESULT (*fn_t)(FIL *);
    return ((fn_t)_sys_tbl[SYS_F_CLOSE])(fp);
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    typedef FRESULT (*fn_t)(FIL *, void *, UINT, UINT *);
    return ((fn_t)_sys_tbl[SYS_F_READ])(fp, buff, btr, br);
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    typedef FRESULT (*fn_t)(FIL *, const void *, UINT, UINT *);
    return ((fn_t)_sys_tbl[SYS_F_WRITE])(fp, buff, btw, bw);
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    typedef FRESULT (*fn_t)(FIL *, FSIZE_t);
    return ((fn_t)_sys_tbl[SYS_F_LSEEK])(fp, ofs);
}

FRESULT f_truncate(FIL *fp)
{
    typedef FRESULT (*fn_t)(FIL *);
    return ((fn_t)_sys_tbl[SYS_F_TRUNCATE])(fp);
}

FRESULT f_sync(FIL *fp)
{
    typedef FRESULT (*fn_t)(FIL *);
    return ((fn_t)_sys_tbl[SYS_F_SYNC])(fp);
}

/* The OS ff.h declares f_eof as a real function (PicoMite's header had a
 * macro).  Pure struct arithmetic — no engine call needed. */
bool f_eof(FIL *fp)
{
    return fp->fptr == fp->obj.objsize;
}

/* ── Directory operations ───────────────────────────────────────────────── */

FRESULT f_opendir(DIR *dp, const TCHAR *path)
{
    typedef FRESULT (*fn_t)(DIR *, const TCHAR *);
    return ((fn_t)_sys_tbl[SYS_F_OPENDIR])(dp, path);
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    typedef FRESULT (*fn_t)(DIR *, FILINFO *);
    return ((fn_t)_sys_tbl[SYS_F_READDIR])(dp, fno);
}

/* ── f_findfirst / f_findnext — built on opendir/readdir ────────────────────
 *
 * The OS engine is compiled with FF_USE_FIND=0, so its DIR struct has no
 * `pat` member to carry the pattern.  Patterns are tracked here instead,
 * in a small table keyed by DIR pointer (MMBasic runs one directory scan
 * at a time; four slots is generous).  Matching follows FatFS semantics:
 * case-insensitive, '?' = any one character, '*' = any run of characters.
 */
#define FIND_SLOTS 4
static struct { DIR *dp; char pat[64]; } find_slots[FIND_SLOTS];

static int pat_match(const char *pat, const char *nam)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *nam; nam++)
                if (pat_match(pat, nam)) return 1;
            return 0;
        }
        if (!*nam) return 0;
        if (*pat != '?') {
            char a = *pat, b = *nam;
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) return 0;
        }
        pat++; nam++;
    }
    return *nam == 0;
}

/* PicoMite's FILES/COPY/KILL loops filter names with FatFS's internal
 * pattern_matching() (it lived in ff.c, which this port doesn't compile).
 * The skip/recur arguments are only used by FatFS's internal recursion;
 * every MMBasic call site passes 0,0. */
int pattern_matching(const TCHAR *pat, const TCHAR *nam, int skip, int recur)
{
    (void)skip; (void)recur;
    return pat_match(pat, nam);
}

static FRESULT find_scan(DIR *dp, FILINFO *fno, const char *pat)
{
    for (;;) {
        FRESULT res = f_readdir(dp, fno);
        if (res != FR_OK || fno->fname[0] == 0)
            return res;             /* error, or end of directory */
        if (!pat || !pat[0] || pat_match(pat, fno->fname))
            return FR_OK;
    }
}

FRESULT f_findfirst(DIR *dp, FILINFO *fno, const TCHAR *path, const TCHAR *pattern)
{
    FRESULT res = f_opendir(dp, path);
    if (res != FR_OK)
        return res;
    int slot = -1;
    for (int i = 0; i < FIND_SLOTS; i++)
        if (find_slots[i].dp == dp || (slot < 0 && find_slots[i].dp == NULL))
            { slot = i; if (find_slots[i].dp == dp) break; }
    const char *pat = pattern ? (const char *)pattern : "";
    if (slot >= 0) {
        find_slots[slot].dp = dp;
        strncpy(find_slots[slot].pat, pat, sizeof(find_slots[slot].pat) - 1);
        find_slots[slot].pat[sizeof(find_slots[slot].pat) - 1] = 0;
        pat = find_slots[slot].pat;
    }
    return find_scan(dp, fno, pat);
}

FRESULT f_findnext(DIR *dp, FILINFO *fno)
{
    const char *pat = NULL;
    for (int i = 0; i < FIND_SLOTS; i++)
        if (find_slots[i].dp == dp) { pat = find_slots[i].pat; break; }
    return find_scan(dp, fno, pat);
}

FRESULT f_closedir(DIR *dp)
{
    for (int i = 0; i < FIND_SLOTS; i++)
        if (find_slots[i].dp == dp) find_slots[i].dp = NULL;
    typedef FRESULT (*fn_t)(DIR *);
    return ((fn_t)_sys_tbl[SYS_F_CLOSEDIR])(dp);
}

/* ── Filesystem-level operations ────────────────────────────────────────── */

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    typedef FRESULT (*fn_t)(const TCHAR *, FILINFO *);
    return ((fn_t)_sys_tbl[SYS_F_STAT])(path, fno);
}

FRESULT f_unlink(const TCHAR *path)
{
    typedef FRESULT (*fn_t)(const TCHAR *);
    return ((fn_t)_sys_tbl[SYS_F_UNLINK])(path);
}

FRESULT f_mkdir(const TCHAR *path)
{
    typedef FRESULT (*fn_t)(const TCHAR *);
    return ((fn_t)_sys_tbl[SYS_F_MKDIR])(path);
}

FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new)
{
    typedef FRESULT (*fn_t)(const TCHAR *, const TCHAR *);
    return ((fn_t)_sys_tbl[SYS_F_RENAME])(path_old, path_new);
}

FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
{
    typedef FRESULT (*fn_t)(const TCHAR *, DWORD *, FATFS **);
    return ((fn_t)_sys_tbl[SYS_F_GETFREE])(path, nclst, fatfs);
}

/* ── Stubs for operations not available via sys_table ───────────────────── */

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{
    (void)fs; (void)path; (void)opt;
    return FR_OK;   /* Frank OS mounts volumes automatically */
}

FRESULT f_getcwd(TCHAR *buff, UINT len)
{
    if (buff && len > 1) { buff[0] = '/'; buff[1] = '\0'; }
    return FR_OK;
}

FRESULT f_chdir(const TCHAR *path)
{
    (void)path;
    return FR_OK;   /* no per-task CWD in Frank OS FatFS */
}

FRESULT f_chdrive(const TCHAR *path)
{
    (void)path;
    return FR_OK;
}

FRESULT f_chmod(const TCHAR *path, BYTE attr, BYTE mask)
{
    (void)path; (void)attr; (void)mask;
    return FR_OK;
}

FRESULT f_utime(const TCHAR *path, const FILINFO *fno)
{
    (void)path; (void)fno;
    return FR_OK;
}

FRESULT f_getlabel(const TCHAR *path, TCHAR *label, DWORD *vsn)
{
    (void)path;
    if (label) label[0] = '\0';
    if (vsn)   *vsn = 0;
    return FR_OK;
}

FRESULT f_setlabel(const TCHAR *label)
{
    (void)label;
    return FR_OK;
}

FRESULT f_forward(FIL *fp, UINT (*func)(const BYTE *, UINT), UINT btf, UINT *bf)
{
    (void)fp; (void)func; (void)btf;
    if (bf) *bf = 0;
    return FR_NOT_ENABLED;
}

FRESULT f_expand(FIL *fp, FSIZE_t fsz, BYTE opt)
{
    (void)fp; (void)fsz; (void)opt;
    return FR_NOT_ENABLED;
}

/* f_mkfs / f_fdisk: not declared by the OS ff.h (FF_USE_MKFS=0) and not
 * called by MMBasic — omitted entirely. */

FRESULT f_setcp(WORD cp)
{
    (void)cp;
    return FR_OK;
}
