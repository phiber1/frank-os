/*
 * frankos/FreeRTOS.h — minimal stub so drivers/fatfs/ffconf.h resolves.
 *
 * The OS ffconf.h includes FreeRTOS.h + semphr.h for its FF_SYNC_t /
 * FF_FS_TIMEOUT macros, but with FF_FS_REENTRANT=0 those macros are
 * never expanded — nothing here is actually used at compile time.
 * The app's real task API comes from m-os-api (api/FreeRTOS/task.h).
 */
#ifndef _FRANKOS_FF_FREERTOS_STUB_H
#define _FRANKOS_FF_FREERTOS_STUB_H
/* intentionally empty */
#endif
