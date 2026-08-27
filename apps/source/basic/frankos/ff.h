/*
 * frankos/ff.h — Frank OS override for PicoMite's ff.h (FatFS)
 *
 * The app does NOT compile FatFS.  All f_* calls are routed through the
 * Frank OS sys_table by frankos_ff.c to the OS's FatFS engine, so every
 * struct the app passes across that boundary (FIL, DIR, FILINFO, FATFS)
 * must have the OS engine's exact layout.  We therefore include the OS
 * driver tree's own ff.h (which pulls in the OS ffconf.h from its own
 * directory): FF_FS_TINY=1, FF_USE_LFN=3, FF_MAX_LFN=255, FF_FS_EXFAT=1
 * (FSIZE_t = 64-bit!), FF_STR_VOLUME_ID=0, FF_USE_FIND=0.
 *
 * PicoMite's bundled ff.h/ffconf.h (FS_TINY=0, LFN=1/127, EXFAT=0) must
 * never be used: the layouts differ everywhere and corrupt the OS engine
 * (this was the original PLAY WAV panic).
 *
 * f_findfirst/f_findnext are not built in the OS engine (FF_USE_FIND=0);
 * frankos_ff.c implements them on top of f_opendir/f_readdir with real
 * pattern matching, so declare them here.
 */
#ifndef _FRANKOS_FF_H
#define _FRANKOS_FF_H

#include "../../../../drivers/fatfs/ff.h"

/* Implemented in frankos_ff.c (OS engine has FF_USE_FIND=0). */
FRESULT f_findfirst(DIR *dp, FILINFO *fno, const TCHAR *path, const TCHAR *pattern);
FRESULT f_findnext(DIR *dp, FILINFO *fno);

/* RTC callback: defined by MMBasic (PicoMite.c); the OS ff.h does not
 * declare it (the OS engine supplies its own). */
DWORD get_fattime(void);

/* FatFS-internal name matcher (lived in ff.c, not compiled on this port);
 * real implementation in frankos_ff.c.  MMBasic calls it directly. */
int pattern_matching(const TCHAR *pat, const TCHAR *nam, int skip, int recur);

#endif /* _FRANKOS_FF_H */
