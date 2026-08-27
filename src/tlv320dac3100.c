/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TLV320DAC3100 codec init for the Adafruit Fruit Jam.
 *
 * The I2S stream from PIO1 is 16-bit stereo at 44100 Hz, so BCLK runs at
 * 32 x 44100 = 1.4112 MHz. The codec has no MCLK wired (GPIO 25 unused),
 * so all internal clocks are PLL-derived from BCLK:
 *
 *   PLL_CLK   = BCLK x R x J / P = 1.4112 MHz x 2 x 32 / 1 = 90.3168 MHz
 *   DAC_FS    = PLL_CLK / (NDAC x MDAC x DOSR)
 *             = 90.3168 MHz / (8 x 2 x 128)  =  44100 Hz exactly
 *
 * (PLL output within the required 80-110 MHz; MDAC=2 satisfies PRB_P1.)
 *
 * I2C is bit-banged on the shared STEMMA QT pins using the same
 * open-drain style as the DS3231 RTC driver (rtc.c), so no pin function
 * is permanently claimed — the pins are left as pulled-up SIO inputs,
 * which is exactly the idle state rtc.c expects.
 */

#include "board_config.h"
#include "tlv320dac3100.h"

static bool dac_ok = false;
bool tlv320dac3100_ok(void) { return dac_ok; }

#ifdef FRANK_BOARD_FRUITJAM

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define DAC_SDA  AUDIO_DAC_PIN_SDA
#define DAC_SCL  AUDIO_DAC_PIN_SCL

/* ~100 kHz bit-banged I2C, open-drain via direction switching */
static inline void dac_i2c_delay(void) { busy_wait_us_32(4); }

static inline void sda_release(void) { gpio_set_dir(DAC_SDA, GPIO_IN); }
static inline void sda_drive0(void)  { gpio_set_dir(DAC_SDA, GPIO_OUT); }
static inline void scl_release(void) { gpio_set_dir(DAC_SCL, GPIO_IN); }
static inline void scl_drive0(void)  { gpio_set_dir(DAC_SCL, GPIO_OUT); }
static inline bool sda_read(void)    { return gpio_get(DAC_SDA); }

static void scl_release_wait(void) {
    scl_release();
    /* allow for clock stretching */
    for (int i = 0; i < 1000 && !gpio_get(DAC_SCL); i++) busy_wait_us_32(1);
}

static void dac_i2c_pin_init(uint pin) {
    gpio_init(pin);
    gpio_put(pin, 0);           /* output register low: dir OUT == drive 0 */
    gpio_set_dir(pin, GPIO_IN); /* released (input) by default */
    gpio_pull_up(pin);
}

static void dac_i2c_start(void) {
    sda_release(); scl_release_wait(); dac_i2c_delay();
    sda_drive0();  dac_i2c_delay();
    scl_drive0();  dac_i2c_delay();
}

static void dac_i2c_stop(void) {
    sda_drive0();  dac_i2c_delay();
    scl_release_wait(); dac_i2c_delay();
    sda_release(); dac_i2c_delay();
}

/* Write one byte, return true on ACK */
static bool dac_i2c_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_release(); else sda_drive0();
        dac_i2c_delay();
        scl_release_wait(); dac_i2c_delay();
        scl_drive0(); dac_i2c_delay();
    }
    sda_release(); dac_i2c_delay();
    scl_release_wait(); dac_i2c_delay();
    bool ack = !sda_read();
    scl_drive0(); dac_i2c_delay();
    return ack;
}

/* Write a single codec register: START, addr, reg, val, STOP */
static bool dac_write_reg(uint8_t reg, uint8_t val) {
    dac_i2c_start();
    bool ok = dac_i2c_write_byte((uint8_t)(AUDIO_DAC_I2C_ADDR << 1))
           && dac_i2c_write_byte(reg)
           && dac_i2c_write_byte(val);
    dac_i2c_stop();
    return ok;
}

bool tlv320dac3100_init(void) {
    /* Release the shared peripheral reset (also resets the ESP32-C6 —
     * called once at boot, before the netcard is brought up). */
    gpio_init(PERIPH_RESET_PIN);
    gpio_put(PERIPH_RESET_PIN, 0);
    gpio_set_dir(PERIPH_RESET_PIN, GPIO_OUT);
    busy_wait_ms(5);
    gpio_put(PERIPH_RESET_PIN, 1);
    busy_wait_ms(10);

    dac_i2c_pin_init(DAC_SDA);
    dac_i2c_pin_init(DAC_SCL);

    /* Probe: page select to 0 — NACK means no codec present */
    if (!dac_write_reg(0x00, 0x00)) {
        printf("TLV320DAC3100: no ACK at 0x%02x\n", AUDIO_DAC_I2C_ADDR);
        return false;
    }

    dac_write_reg(0x01, 0x01);      /* software reset */
    busy_wait_ms(10);

    /* --- Page 0: clocking (PLL from BCLK) --- */
    dac_write_reg(0x00, 0x00);
    dac_write_reg(0x04, 0x07);      /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
    dac_write_reg(0x06, 32);        /* J = 32 */
    dac_write_reg(0x07, 0x00);      /* D = 0 (MSB) */
    dac_write_reg(0x08, 0x00);      /* D = 0 (LSB) */
    dac_write_reg(0x05, 0x92);      /* PLL power up, P = 1, R = 2 */
    busy_wait_ms(15);               /* PLL lock */
    dac_write_reg(0x0B, 0x80 | 8);  /* NDAC = 8, powered */
    dac_write_reg(0x0C, 0x80 | 2);  /* MDAC = 2, powered */
    dac_write_reg(0x0D, 0x00);      /* DOSR = 128 (MSB) */
    dac_write_reg(0x0E, 0x80);      /* DOSR = 128 (LSB) */
    dac_write_reg(0x1B, 0x00);      /* I2S, 16-bit, BCLK/WCLK are inputs */
    dac_write_reg(0x3C, 0x01);      /* processing block PRB_P1 */

    /* --- Page 1: analog outputs (headphone + speaker) --- */
    dac_write_reg(0x00, 0x01);
    dac_write_reg(0x21, 0x4E);      /* HP de-pop: soft ramp on power-up */
    dac_write_reg(0x1F, 0xC4);      /* HPL + HPR drivers up, CM 1.35V */
    dac_write_reg(0x20, 0x86);      /* class-D speaker amp up */
    dac_write_reg(0x23, 0x44);      /* DAC_L -> HPL mixer, DAC_R -> HPR mixer */
    dac_write_reg(0x24, 0x92);      /* HPL analog vol: routed, -9 dB */
    dac_write_reg(0x25, 0x92);      /* HPR analog vol: routed, -9 dB */
    dac_write_reg(0x26, 0x92);      /* SPK analog vol: routed, -9 dB */
    dac_write_reg(0x28, 0x06);      /* HPL driver 0 dB, unmute */
    dac_write_reg(0x29, 0x06);      /* HPR driver 0 dB, unmute */
    dac_write_reg(0x2A, 0x04);      /* SPK driver 6 dB (min), unmute */

    /* --- Page 0: DAC power up and unmute --- */
    dac_write_reg(0x00, 0x00);
    dac_write_reg(0x3F, 0xD4);      /* both DACs on, L->L / R->R, soft-step */
    dac_write_reg(0x40, 0x00);      /* unmute both channels */
    dac_write_reg(0x41, 0x00);      /* left digital vol 0 dB */
    if (!dac_write_reg(0x42, 0x00)) /* right digital vol 0 dB */
        return false;

    printf("TLV320DAC3100: initialized (BCLK PLL, 44100 Hz)\n");
    dac_ok = true;
    return true;
}

#else /* !FRANK_BOARD_FRUITJAM */

bool tlv320dac3100_init(void) { dac_ok = true; return true; }

#endif
