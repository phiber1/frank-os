/*
 * ff.h — Frank OS port: forward to the OS FatFS header.
 *
 * GCC resolves #include "ff.h" from any file in this directory to THIS
 * file (the including file's directory is searched before all -I paths),
 * so the frankos/ include-dir override can never intercept it.  This
 * file therefore forwards to frankos/ff.h, which includes the OS driver
 * tree's ff.h + ffconf.h: every FatFS struct (FIL, DIR, FILINFO, FATFS)
 * gets the exact layout of the OS engine that frankos_ff.c routes to
 * through the sys_table.
 *
 * PicoMite's original FatFS R0.14b header (config: FS_TINY=0, LFN=1/127,
 * EXFAT=0 — all different from the OS build, and the cause of the
 * original PLAY WAV stack-smash panics) is preserved unmodified in
 * ff.h.picomite_orig.  ffconf.h and ff.c in this directory are unused.
 */
#include "frankos/ff.h"
