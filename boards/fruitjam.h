/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Adafruit Fruit Jam board definition (RP2350B, 16MB flash, 8MB PSRAM).
 *
 * Pin assignments follow the official pico-sdk adafruit_fruit_jam.h and
 * the Fruit Jam schematic. FRANK OS subsystem pin mapping derived from
 * these lives in src/board_config.h (selected via FRANK_BOARD_FRUITJAM).
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_FRUITJAM_H
#define _BOARDS_FRUITJAM_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection (FRANK OS board_config.h keys off FRANK_BOARD_FRUITJAM)
#define ADAFRUIT_FRUIT_JAM
#define FRANK_BOARD_FRUITJAM 1

// On some samples, the xosc can take longer to stabilize than is usual
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

// --- RP2350 VARIANT ---
// RP2350B (QFN-80, 48 GPIOs)
#define PICO_RP2350A 0

// --- BOARD PINS (from the Fruit Jam schematic) ---

// Buttons (Button 1 doubles as BOOT)
#define FRUITJAM_BUTTON1_PIN 0
#define FRUITJAM_BUTTON2_PIN 4
#define FRUITJAM_BUTTON3_PIN 5

// USB host port (PIO-USB, feeds the two USB-A jacks)
#define FRUITJAM_USB_HOST_DP_PIN       1
#define FRUITJAM_USB_HOST_DM_PIN       2
#define FRUITJAM_USB_HOST_5V_POWER_PIN 11

// 2x16 socket header GPIOs
#define FRUITJAM_D6_PIN  6
#define FRUITJAM_D7_PIN  7
#define FRUITJAM_D8_PIN  8   /* also UART TX to ESP32-C6 */
#define FRUITJAM_D9_PIN  9   /* also UART RX from ESP32-C6 */
#define FRUITJAM_D10_PIN 10
#define FRUITJAM_A0_PIN  40  /* JST PH connector */
#define FRUITJAM_A1_PIN  41
#define FRUITJAM_A2_PIN  42
#define FRUITJAM_A3_PIN  43
#define FRUITJAM_A4_PIN  44
#define FRUITJAM_A5_PIN  45

// DVI via HSTX: CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
// (same order as FRANK M2 — DISPHSTX_DVI_PINOUT 2 in src/config.h)
#define FRUITJAM_DVI_CKN_PIN 12
#define FRUITJAM_DVI_CKP_PIN 13
#define FRUITJAM_DVI_D0N_PIN 14
#define FRUITJAM_DVI_D0P_PIN 15
#define FRUITJAM_DVI_D1N_PIN 16
#define FRUITJAM_DVI_D1P_PIN 17
#define FRUITJAM_DVI_D2N_PIN 18
#define FRUITJAM_DVI_D2P_PIN 19

// Active-low reset shared by ESP32-C6 and TLV320DAC3100 audio DAC
#define FRUITJAM_PERIPH_RESET_PIN 22

// TLV320DAC3100 I2S audio DAC (control interface on I2C0, addr 0x18)
#define FRUITJAM_I2S_IRQ_PIN  23
#define FRUITJAM_I2S_DIN_PIN  24
#define FRUITJAM_I2S_MCLK_PIN 25
#define FRUITJAM_I2S_BCLK_PIN 26
#define FRUITJAM_I2S_WS_PIN   27

// ESP32-C6 co-processor (SPI + control lines)
#define FRUITJAM_WIFI_MISO_PIN  28
#define FRUITJAM_WIFI_SCK_PIN   30
#define FRUITJAM_WIFI_MOSI_PIN  31
#define FRUITJAM_WIFI_CS_PIN    46
#define FRUITJAM_WIFI_ACK_PIN   3
#define FRUITJAM_WIFI_RESET_PIN 22

// micro-SD card (SPI0 in SPI mode; 37/38 are SDIO DAT1/DAT2, unused in SPI mode)
#define FRUITJAM_SD_SCK_PIN         34
#define FRUITJAM_SD_MOSI_PIN        35
#define FRUITJAM_SD_MISO_PIN        36
#define FRUITJAM_SD_CS_PIN          39
#define FRUITJAM_SD_CARD_DETECT_PIN 33

// --- UART ---
// The official board default is UART1 on GPIO 8/9, but FRANK OS uses those
// pins for the ESP netcard (PIO UART). Route stdio UART0 to header pins
// A4/A5 instead so debug output never disturbs the netcard link.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 44
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 45
#endif

// --- LED ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 29
#endif

// --- RGB (NeoPixel) LED ---
#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 32
#endif

// --- I2C --- (STEMMA QT connector + TLV320DAC3100 control)
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 20
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 21
#endif

// --- SPI --- (default SPI is the SD card bus)
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN FRUITJAM_SD_SCK_PIN
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN FRUITJAM_SD_MOSI_PIN
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN FRUITJAM_SD_MISO_PIN
#endif

// --- FLASH ---
// Winbond W25Q128 (16MB)
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM --- (8MB on QSPI CS1)
#ifndef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN 47
#endif

pico_board_cmake_set_default(PICO_PSRAM_SIZE_BYTES, (8 * 1024 * 1024))
#ifndef PICO_PSRAM_SIZE_BYTES
#define PICO_PSRAM_SIZE_BYTES (8 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
