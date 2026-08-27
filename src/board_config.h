/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/vreg.h"

/*
 * Board Configuration for FRANK OS
 *
 * The board is selected at configure time via PICO_BOARD (see
 * boards/*.h). Each board header defines a FRANK_BOARD_* macro that
 * selects the matching pin layout below.
 *
 * FRANK M2 GPIO Layout:
 *   PS/2 Mouse:  CLK=0, DATA=1
 *   PS/2 Kbd:    CLK=2, DATA=3
 *   SD Card:     MISO=4, CSn=5, SCK=6, MOSI=7
 *   PSRAM:       CS=8
 *   I2S Audio:   DATA=9, BCLK=10, LRCLK=11
 *   HDMI (HSTX): CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
 *   ESP netcard: TX=38, RX=39
 *   DS3231 RTC:  SDA=28, SCL=29
 *
 * Adafruit Fruit Jam GPIO Layout (fixed by board hardware):
 *   Buttons:     BTN1/BOOT=0, BTN2=4, BTN3=5
 *   USB Host:    D+=1, D-=2, 5V enable=11 (PIO-USB, two USB-A jacks)
 *   PS/2 Kbd:    CLK=6, DATA=7      (header D6/D7 — external adapter)
 *   PS/2 Mouse:  CLK=30, DATA=31    (header SCK/MOSI — external adapter)
 *   ESP netcard: TX=8, RX=9         (UART link to ESP32-C6 / header D8/D9)
 *   HDMI (HSTX): CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
 *   I2C0:        SDA=20, SCL=21     (STEMMA QT + TLV320DAC3100 control)
 *   Periph rst:  22 (active low; ESP32-C6 + audio DAC)
 *   I2S Audio:   DATA=24, MCLK=25, BCLK=26, LRCLK=27 (TLV320DAC3100)
 *   LED=29, NeoPixel=32
 *   SD Card:     DETECT=33, SCK=34, MOSI=35, MISO=36, CSn=39 (SPI0)
 *   PSRAM:       CS=47
 */

//=============================================================================
// CPU Speed Defaults
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 504
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_65
#endif

#if defined(FRANK_BOARD_FRUITJAM)

//=============================================================================
// Adafruit Fruit Jam Layout Configuration
//=============================================================================
#define BOARD_FRUITJAM

/* PS/2 is not native on Fruit Jam — these defaults put an externally wired
 * adapter on free header pins. The PIO program requires DATA = CLK + 1.
 * Primary input on this board is USB HID (build with -DUSB_HID_ENABLED=1). */
#define PS2_PIN_CLK  6      /* header D6 */
#define PS2_PIN_DATA 7      /* header D7 */

#define PS2_MOUSE_CLK  30   /* header SCK (shared with unused ESP32-C6 SPI) */
#define PS2_MOUSE_DATA 31   /* header MOSI */

//=============================================================================
// SD Card (SPI0, GPIO 34-36 + CS 39, card-detect 33)
//=============================================================================
#define SDCARD_PIN_CLK     34   /* SPI0 SCK */
#define SDCARD_PIN_CMD     35   /* SPI0 TX / MOSI */
#define SDCARD_PIN_D0      36   /* SPI0 RX / MISO */
#define SDCARD_PIN_D3      39   /* CSn (software-driven GPIO) */
#define SDCARD_PIN_DETECT  33   /* card-detect switch (not used by driver yet) */

//=============================================================================
// Audio — I2S via PIO1 to TLV320DAC3100 (DATA=24, BCLK=26, LRCLK=27)
//
// NOTE: unlike the M2's dumb I2S DAC, the TLV320DAC3100 must be brought out
// of reset (PERIPH_RESET high) and configured over I2C0 (addr 0x18) before
// it outputs sound. MCLK (GPIO 25) can stay unused when the codec is
// programmed to derive its clocks from BCLK.
//=============================================================================
#define I2S_DATA_PIN       24   /* I2S serial data (DAC DIN) */
#define I2S_CLOCK_PIN_BASE 26   /* BCLK=26, LRCLK=27 */

#define AUDIO_DAC_I2C_ADDR 0x18 /* TLV320DAC3100 7-bit address */
#define AUDIO_DAC_PIN_SDA  20   /* codec control I2C (shared with STEMMA QT/RTC) */
#define AUDIO_DAC_PIN_SCL  21
#define PERIPH_RESET_PIN   22   /* active-low reset: audio DAC + ESP32-C6 */

//=============================================================================
// ESP Netcard — PIO UART on GPIO 8/9
//   Board wiring: GP8 (TX/D8) → ESP32-C6 RX (we transmit here)
//                 GP9 (RX/D9) ← ESP32-C6 TX (we receive here)
//   An external ESP-01 can be wired to header pins D8/D9 instead.
//=============================================================================
#define NETCARD_PIN_TX  8    /* our TX (drives ESP RX) */
#define NETCARD_PIN_RX  9    /* our RX (samples ESP TX) */
#define NETCARD_BAUD    115200

/* PIO2 GPIO base for the netcard/serial UART. Netcard pins 8/9 need base 0
 * (window covers GPIO 0-31). */
#define NETCARD_PIO_GPIO_BASE 0

//=============================================================================
// DS3231MZ Real-Time Clock — bit-banged I2C
//   Defaults to the STEMMA QT connector (I2C0 pins; user-configurable).
//=============================================================================
#define RTC_PIN_SCL     21
#define RTC_PIN_SDA     20
#define RTC_I2C_ADDR    0x68   /* DS3231 7-bit address */

//=============================================================================
// Misc Fruit Jam hardware (available to apps / future drivers)
//=============================================================================
#define BOARD_BUTTON1_PIN  0    /* also BOOT */
#define BOARD_BUTTON2_PIN  4
#define BOARD_BUTTON3_PIN  5
#define BOARD_NEOPIXEL_PIN 32
#define USB_HOST_DP_PIN    1    /* PIO-USB D+ (D- must be DP+1 = GPIO 2) */
#define USB_HOST_5V_PIN    11   /* drive high to power the USB-A jacks */

#else /* FRANK_BOARD_M2 (default) */

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#define BOARD_M2

#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

//=============================================================================
// SD Card (SPI0, GPIO 4-7)
//=============================================================================
#define SDCARD_PIN_CLK  6   /* SPI0 SCK */
#define SDCARD_PIN_CMD  7   /* SPI0 TX / MOSI */
#define SDCARD_PIN_D0   4   /* SPI0 RX / MISO */
#define SDCARD_PIN_D3   5   /* SPI0 CSn */

//=============================================================================
// Audio — I2S via PIO1 (GPIO 9/10/11, matching M2 board layout)
//=============================================================================
#define I2S_DATA_PIN       9    /* I2S serial data */
#define I2S_CLOCK_PIN_BASE 10   /* BCLK=10, LRCLK=11 */

//=============================================================================
// ESP-01 Netcard — PIO UART on GPIO 38/39
//   Board wiring: GP38 → ESP's RX pin (we transmit here)
//                 GP39 ← ESP's TX pin (we receive here)
//=============================================================================
#define NETCARD_PIN_TX  38   /* our TX (drives ESP RX) */
#define NETCARD_PIN_RX  39   /* our RX (samples ESP TX) */
#define NETCARD_BAUD    115200

/* PIO2 GPIO base for the netcard/serial UART. Netcard pins 38/39 need base
 * 16 (window covers GPIO 16-47). */
#define NETCARD_PIO_GPIO_BASE 16

//=============================================================================
// DS3231MZ Real-Time Clock — bit-banged I2C
//   Board wiring: SCL=GP29, SDA=GP28 (defaults; user-configurable)
//=============================================================================
#define RTC_PIN_SCL     29
#define RTC_PIN_SDA     28
#define RTC_I2C_ADDR    0x68   /* DS3231 7-bit address */

#endif /* board selection */

//=============================================================================
// PSRAM (QSPI CS1 — pin depends on RP2350 package variant)
//   RP2350A (QFN-60, 30 GPIO): GPIO 8  (M2 board layout)
//   RP2350B (QFN-80, 48 GPIO): GPIO 47 (M2 and Fruit Jam)
//=============================================================================
#define PSRAM_PIN_RP2350A 8
#define PSRAM_PIN_RP2350B 47

#if PICO_RP2350
#include "hardware/structs/sysinfo.h"
static inline uint get_psram_pin(void) {
    uint32_t package_sel = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) {
        return PSRAM_PIN_RP2350A;
    } else {
        return PSRAM_PIN_RP2350B;
    }
}
#endif

#endif // BOARD_CONFIG_H
