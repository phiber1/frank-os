/*
 * frankos_stubs.c — No-op stubs for hardware-specific MMBasic modules
 *
 * On Frank OS, hardware is accessed through the OS API.  The following
 * PicoMite source files are NOT compiled:
 *   Custom.c   — PIO2 / DMA  → cmd_PIOline, fun_pio, …
 *   Audio.c    — I2S audio   → cmd_play, WAVInterrupt, …
 *   I2C.c      — direct I2C  → cmd_i2c, fun_mmi2c, …
 *   SPI.c      — direct SPI  → cmd_spi, fun_spi, …
 *   SPI-LCD.c  — LCD SPI
 *   Serial.c   — UART        → cmd_serial, …
 *   Onewire.c  — 1-wire GPIO
 *   GPS.c      — GPS UART
 *   VS1053.c   — audio SPI
 *   psram.c    — PSRAM SPI
 *   stepper.c  — stepper GPIO
 *   Keyboard.c — PS/2 keyboard hardware
 *   mouse.c    — PS/2 mouse hardware
 *   CFunction.c — ARM machine-code execution
 *   VGA222.c / RGB121.c — VGA display hardware
 *   SSD1963.c / Touch.c — LCD panel hardware
 *
 * This file provides:
 *   • Global variables those files normally define (referenced by core files)
 *   • No-op implementations of every public cmd_xxx / fun_xxx entry point
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* pico/stdlib.h first — establishes <stdio.h> FILE / fclose / etc. before any
 * header that might redefine them (m-os-api-ff.h defines #define fclose(f)
 * f_close(f) which would turn stdio's int fclose(FILE*) into int f_close(FILE*)
 * causing a type conflict with FatFS).  Do NOT include m-os-api.h here. */
#include "pico/stdlib.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

/* ── Global variables (referenced by core MMBasic files) ─────────────────── */

/* Custom.c */
char *pioRXinterrupts[4][3]  = {0};
char    *pioTXinterrupts[4][3]  = {0};
uint8_t  pioTXlast[4][3]        = {0};
char *DMAinterruptRX         = NULL;
char *DMAinterruptTX         = NULL;
bool  PIO0                   = false;
bool  PIO1                   = false;
bool  PIO2                   = false;

/* Audio.c: WAVInterrupt / WAVcomplete / modbuff and the PLAY command are
 * now provided by frankos_audio.c (OS-mixer-backed implementation). */

/* I2C.c */
bool  noRTC                  = false;
bool  noI2C                  = false;

/* mouse.c */
bool  mouse0                 = false;
bool  mouseupdated           = false;
char *mouse0Interruptc       = NULL;

/* Serial.c */
char *DMAinterruptRXS        = NULL;  /* serial DMA RX */
char *DMAinterruptTXS        = NULL;  /* serial DMA TX */

/* GPS.c */
MMFLOAT GPSlatitude   = 0;
MMFLOAT GPSlongitude  = 0;
MMFLOAT GPSaltitude   = 0;
MMFLOAT GPSspeed      = 0;
MMFLOAT GPStrack      = 0;
MMFLOAT GPSdop        = 0;
MMFLOAT GPSgeoid      = 0;
MMFLOAT GPSJulian     = 0;
MMFLOAT GPSSidereal   = 0;
char    GPSdate[11]   = "000-00-2000";
char    GPStime[9]    = "00:00:00";

/* GUI.c */
char   *GUItimer          = NULL;

/* ── Helper: print "not supported" and raise an error ───────────────────── */
static void not_supported(const char *name)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: not supported on Frank OS", name);
    error(buf);
}

/* ── PIO / Custom commands (Custom.c) ────────────────────────────────────── */
void cmd_PIOline(void)    { not_supported("PIO"); }
void cmd_endprogram(void) { not_supported("PIO ENDPROGRAM"); }
void cmd_exec_star(void)  { not_supported("PIO EXEC"); }
void cmd_in(void)         { not_supported("PIO IN"); }
void cmd_irq(void)        { not_supported("PIO IRQ"); }
void cmd_irqclear(void)   { not_supported("PIO IRQCLEAR"); }
void cmd_irqnext(void)    { not_supported("PIO IRQNEXT"); }
void cmd_irqnowait(void)  { not_supported("PIO IRQNOWAIT"); }
void cmd_irqprev(void)    { not_supported("PIO IRQPREV"); }
void cmd_irqset(void)     { not_supported("PIO IRQSET"); }
void cmd_irqwait(void)    { not_supported("PIO IRQWAIT"); }
void cmd_jmp(void)        { not_supported("PIO JMP"); }
void cmd_label(void)      { not_supported("PIO LABEL"); }
void cmd_map(void)        { not_supported("PIO MAP"); }
void cmd_mov(void)        { not_supported("PIO MOV"); }
void cmd_nop(void)        { not_supported("PIO NOP"); }
void cmd_out(void)        { not_supported("PIO OUT"); }
void cmd_program(void)    { not_supported("PIO PROGRAM"); }
void cmd_pull(void)       { not_supported("PIO PULL"); }
void cmd_push(void)       { not_supported("PIO PUSH"); }
void cmd_set(void)        { not_supported("PIO SET"); }
void cmd_sideset(void)    { not_supported("PIO SIDESET"); }
void cmd_star(void)       { not_supported("PIO *"); }
void cmd_wait(void)       { not_supported("PIO WAIT"); }
void cmd_wrap(void)       { not_supported("PIO WRAP"); }
void cmd_wraptarget(void) { not_supported("PIO WRAPTARGET"); }
void fun_pio(void)        { not_supported("PIO()"); }
void fun_map(void)        { not_supported("MAP()"); }

/* ── I2C (I2C.c) ─────────────────────────────────────────────────────────── */
void cmd_i2c(void)        { not_supported("I2C"); }
void cmd_i2c2(void)       { not_supported("I2C2"); }
void fun_mmi2c(void)      { not_supported("I2C()"); }

/* ── SPI (SPI.c / SPI-LCD.c) ─────────────────────────────────────────────── */
void cmd_spi(void)        { not_supported("SPI"); }
void cmd_spi2(void)       { not_supported("SPI2"); }
void fun_spi(void)        { not_supported("SPI()"); }
void fun_spi2(void)       { not_supported("SPI2()"); }

/* ── Onewire (Onewire.c) ─────────────────────────────────────────────────── */
void cmd_onewire(void)    { not_supported("ONEWIRE"); }
void cmd_ds18b20(void)    { not_supported("DS18B20"); }
void fun_mmOW(void)       { not_supported("MM.OW()"); }
void fun_owCRC8(void)     { not_supported("OWCRC8()"); }
void fun_owCRC16(void)    { not_supported("OWCRC16()"); }
void fun_owSearch(void)   { not_supported("OWSEARCH()"); }
void fun_ds18b20(void)    { not_supported("DS18B20()"); }

/* ── GPS (GPS.c) ─────────────────────────────────────────────────────────── */
void fun_GPS(void)        { not_supported("GPS()"); }

/* ── Stepper (stepper.c) ─────────────────────────────────────────────────── */
void cmd_stepper(void)    { not_supported("STEPPER"); }

/* ── Mouse (mouse.c) ─────────────────────────────────────────────────────── */
void cmd_mouse(void)      { not_supported("MOUSE"); }

/* ── Turtle graphics (Turtle.c) ─────────────────────────────────────────── */
void cmd_turtle(void)     { not_supported("TURTLE"); }

/* ── GUI controls (GUI.c) ───────────────────────────────────────────────── */
void cmd_gui(void)        { not_supported("GUI"); }
void cmd_GUIpage(unsigned char *p) { (void)p; not_supported("GUIPAGE"); }
void cmd_ctrlval(void)    { not_supported("CTRLVAL"); }
void cmd_locate(void)     { not_supported("LOCATE"); }
void cmd_mode(void)       { not_supported("MODE"); }
void fun_ctrlval(void)    { not_supported("CTRLVAL()"); }
void fun_mmhpos(void)     { not_supported("MM.HPOS"); }
void fun_mmvpos(void)     { not_supported("MM.VPOS"); }
void fun_msgbox(void)     { not_supported("MSGBOX()"); }
void fun_touch(void)      { not_supported("TOUCH()"); }
void fun_json(void)       { not_supported("JSON()"); }

/* ── Web / TCP (network stubs) ───────────────────────────────────────────── */
void cmd_web(void)        { not_supported("WEB"); }

/* ── CFunction (CFunction.c) ────────────────────────────────────────────── */
/* CheckCfun and LoadCFunction may be called from Commands.c / FileIO.c */
void CheckCfun(void)                            { }
void LoadCFunction(void *p, int n)              { (void)p; (void)n; }
int  CFuncmatch(const char *s)                  { (void)s; return 0; }

/* ── GUI init / tick (GUI.c) ─────────────────────────────────────────────── */
void InitGui(void)                              { }
void CheckGuiTimeouts(void)                     { }
void DrawKeyboard(int mode)                     { (void)mode; }

/* ── Audio (Audio.c) ─────────────────────────────────────────────────────── */
/* initAudio / cmd_play / CloseAudio / StopAudio now live in frankos_audio.c */
void cmd_sound(void)                            { not_supported("SOUND"); }
void fun_mmwave(void)                           { not_supported("MM.WAVE"); }

/* ── Keyboard hardware initialisation (Keyboard.c) ──────────────────────── */
/* initKeyboard / CheckKeyboard already provided in frankos_platform.c */

/* ── LCD display initialisation (SSD1963.c / Touch.c / VGA222.c) ─────────── */
void InitDisplayHardware(int type)              { (void)type; }
void InitTouch(void)                            { }
void CheckTouch(void)                           { }
void cmd_touch(void)                            { not_supported("TOUCH"); }

/* ── PSRAM (psram.c) ─────────────────────────────────────────────────────── */
/* psram_init() already stubbed in frankos_platform.c */

/* ── Serial / UART (Serial.c) ────────────────────────────────────────────── */
void cmd_serial(void)                           { not_supported("SERIAL"); }
void fun_serial(void)                           { not_supported("SERIAL()"); }
unsigned char SerialPutchar(int comnbr, unsigned char c) { (void)comnbr; return c; }
int           SerialGetchar(int port)                    { (void)port; return -1; }
void          SerialClose(int port)                      { (void)port; }

/* ── Raycaster (Raycaster.c) — Raycaster uses Draw.c; keep as no-op ──────── */
void cmd_raycaster(void)                        { not_supported("RAYCASTER"); }
