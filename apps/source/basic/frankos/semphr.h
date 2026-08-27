/*
 * frankos/semphr.h — minimal stub so drivers/fatfs/ffconf.h resolves.
 * FF_FS_REENTRANT=0: FF_SYNC_t (SemaphoreHandle_t) is never referenced.
 */
#ifndef _FRANKOS_FF_SEMPHR_STUB_H
#define _FRANKOS_FF_SEMPHR_STUB_H
typedef void *SemaphoreHandle_t;
#endif
