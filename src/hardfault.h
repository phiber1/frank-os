#pragma once

#include <stdint.h>
#include <stdbool.h>

uint32_t get_cpu_ram_size(void);
uint32_t get_cpu_flash_size(void);
void get_cpu_flash_jedec_id(uint8_t rx[4]);
bool cpu_check_address(volatile const char *address);
__attribute__((naked)) void hardfault_handler(void);

/* DWT hardware-watchpoint diagnostics (main.c).  os_watch_set arms a
 * write watchpoint on the 8 bytes at addr; os_watch_get reads the hit
 * ring (returns total hit count so far). */
void os_watch_set(const void *addr);
uint32_t os_watch_get(uint32_t idx, uint32_t *pc, uint32_t *lr);
