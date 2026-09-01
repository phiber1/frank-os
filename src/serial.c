/*
 * FRANK OS — PIO UART Serial Driver (ESP-01 Netcard)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * ESP-01 wiring (pins from ESP-01's perspective):
 *   ESP-01 RX <- GPIO39 (our TX)
 *   ESP-01 TX -> GPIO38 (our RX)
 *
 * We use PIO-based UART on PIO2 with GPIO base = 16 so it can reach pins
 * 38/39 (RP2350B 48-pin package). PIO0 is used by PS/2 (pins 0-3) and
 * PIO1 by I2S audio (pins 9-11), both of which require GPIO base = 0 and
 * therefore cannot address pins ≥ 32.
 *
 * Uses PIO RX interrupt to drain into a 2KB ring buffer so we never
 * lose bytes even when the main loop is busy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "board_config.h"
#include "serial.h"
#include "wifi_config.h"
#include "FreeRTOS.h"
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"

/* PIO UART on PIO2. PIO0 = PS/2 (base 0), PIO1 = I2S (base 0);
 * only PIO2 is free to take the board-specific GPIO base needed to reach
 * the netcard pins (base 16 on M2 for 38/39, base 0 on Fruit Jam for 8/9). */
#define SERIAL_PIO      pio2
#define SERIAL_PIO_IRQ  PIO2_IRQ_0
#define SERIAL_PIO_GPIO_BASE  NETCARD_PIO_GPIO_BASE

/* ESP netcard pins: our TX drives ESP RX, our RX samples ESP TX */
#define PIN_TX          NETCARD_PIN_TX
#define PIN_RX          NETCARD_PIN_RX

#define SERIAL_BAUD     NETCARD_BAUD

/* PIO2 GPIO base window: pins must fall in [base, base+31]. */
#define SERIAL_PIN_MIN  SERIAL_PIO_GPIO_BASE
#define SERIAL_PIN_MAX  (SERIAL_PIO_GPIO_BASE + 31)

/* Interrupt-driven RX ring buffer (must be power of 2).
 * Web pages arrive as multi-KB +SRECV streams — the netcard task must
 * be able to drain the FIFO without losing bytes even under scheduling
 * jitter.  2KB comfortably holds several +SRECV events. */
/* Must ride out the longest scheduler-suspended window: the FatFS engine
 * wraps SD operations in vTaskSuspendAll, and SD erase-block stalls can
 * reach ~500ms — during which netcard_task cannot drain this ring while
 * the C6 streams on at 115200 (11.5KB/s, NO flow control on the wire).
 * 2KB (178ms) silently dropped bytes mid-+SRECV, desyncing the binary
 * framing and killing large transfers at random offsets.  32KB ≈ 2.8s. */
#define RX_BUF_SIZE     32768
#define RX_BUF_MASK     (RX_BUF_SIZE - 1)

static uint tx_offset, rx_offset;
static uint tx_sm, rx_sm;

static bool    serial_inited;       /* true after first serial_init() */
static uint8_t cur_tx_pin, cur_rx_pin;


static inline uint8_t pio_uart_read_byte_raw(void) {
    return (uint8_t)(pio_sm_get(SERIAL_PIO, rx_sm) >> 24);
}

/* TEMP DIAG: byte accounting for the RX path (large-transfer stalls). */
volatile uint32_t serial_rx_total_bytes   = 0;
volatile uint32_t serial_rx_dropped_bytes = 0;

/* ── Two-stage DMA-driven RX ────────────────────────────────────────────────
 * Stage 1: a DREQ-paced DMA channel drains the PIO RX FIFO into an 8KB
 * SRAM ring with ZERO per-byte CPU work.  The FIFO physically cannot
 * overflow no matter how long interrupts are masked — the previous
 * per-byte ISR overflowed at 921600 baud whenever a FreeRTOS critical
 * section exceeded ~87 us (and raising the ISR to priority 0 broke
 * DispHSTX scanline timing).  This ring MUST live in SRAM: DMA writes
 * through the QMI PSRAM window contend with flash XIP and glitch the
 * DispHSTX scanline engine (and corrupt the received data).
 *
 * Stage 2: a priority-4 pump task copies the SRAM ring into a 128KB
 * PSRAM ring the consumer reads.  8KB is only ~89 ms at 921600 baud,
 * and the consumer (netcard task, priority 1) can stall far longer than
 * that — its data callback writes received blocks to SD, and a FAT
 * allocation spike easily exceeds 89 ms.  The pump never blocks on
 * anything and outranks every consumer, so the SRAM ring stays nearly
 * empty; the PSRAM ring gives ~1.4 s of slack.  CPU (not DMA) writes to
 * the uncached PSRAM window are the OS-wide norm and display-safe.
 *
 * The DMA byte lane is rxf+3 because the 8-bit UART program leaves data
 * in bits 31:24. */
#include "hardware/dma.h"
#include "task.h"
#include "../drivers/psram/psram.h"

#define RXDMA_RING_BITS  13                     /* 8KB, ring-aligned */
#define RXDMA_RING_SIZE  (1u << RXDMA_RING_BITS)
#define RXDMA_RING_MASK  (RXDMA_RING_SIZE - 1)
#define RXDMA_ARM_COUNT  0x0FFFFFFFu            /* ~45 min @ 92KB/s */

#define RXPS_RING_SIZE   (1024u * 1024u)   /* ~11 s @ 921600 — a whole
                                              1MB transfer fits even if
                                              the consumer stalls */
#define RXPS_RING_MASK   (RXPS_RING_SIZE - 1)

static int      rx_dma_chan = -1;
static uint8_t *rx_ring;                        /* ring-aligned in SRAM */
static bool     rx_dma_armed;
static uint32_t rxdma_base;      /* absolute bytes produced at last arm */
static uint32_t sram_rd;         /* absolute bytes pumped out (pump-private) */

static uint8_t *rxps_ring;                      /* big ring in PSRAM */
static volatile uint32_t rxps_wr, rxps_rd;      /* absolute SPSC counters */
static volatile uint32_t rx_overrun_events;

/* High-water marks (diagnostics): worst backlog seen in each stage. */
static volatile uint32_t rx_sram_hwm, rx_psram_hwm;

/* Absolute byte count the DMA has written since boot (monotonic across
 * re-arms).  Only valid between arm and abort. */
static inline uint32_t rxdma_produced(void) {
    return rxdma_base + (RXDMA_ARM_COUNT -
                         dma_channel_hw_addr(rx_dma_chan)->transfer_count);
}

/* (Re)start the RX DMA.  Must be called with the channel idle (never
 * armed yet, or after dma_channel_abort). */
static void rxdma_arm(void) {
    if (rx_dma_armed)
        rxdma_base += RXDMA_ARM_COUNT -
                      dma_channel_hw_addr(rx_dma_chan)->transfer_count;
    dma_channel_config c = dma_channel_get_default_config(rx_dma_chan);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_ring(&c, true, RXDMA_RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(SERIAL_PIO, rx_sm, false));
    dma_channel_configure(rx_dma_chan, &c,
        rx_ring + (rxdma_base & RXDMA_RING_MASK),
        (const volatile uint8_t *)&SERIAL_PIO->rxf[rx_sm] + 3,
        RXDMA_ARM_COUNT, true);
    rx_dma_armed = true;
}

/* Move everything the DMA has landed in SRAM over to the PSRAM ring.
 * Pump-task context only (single producer of rxps_wr / owner of
 * sram_rd). */
static void serial_rx_pump(void) {
    uint32_t produced = rxdma_produced();
    uint32_t avail = produced - sram_rd;
    if (avail > rx_sram_hwm) rx_sram_hwm = avail;
    if (avail > RXDMA_RING_SIZE) {      /* DMA lapped the pump: data lost */
        rx_overrun_events++;
        sram_rd = produced - RXDMA_RING_SIZE;
        avail = RXDMA_RING_SIZE;
    }
    if (avail > 4096) avail = 4096;     /* bound time at priority 4; at
                                           ~184 bytes/tick inflow the
                                           backlog drains next wake */
    while (avail--) {
        if ((uint32_t)(rxps_wr - rxps_rd) < RXPS_RING_SIZE) {
            rxps_ring[rxps_wr & RXPS_RING_MASK] =
                rx_ring[sram_rd & RXDMA_RING_MASK];
            rxps_wr++;
        } else {
            serial_rx_dropped_bytes++;  /* consumer >1.4 s behind */
        }
        sram_rd++;
    }
    {
        uint32_t backlog = rxps_wr - rxps_rd;
        if (backlog > rx_psram_hwm) rx_psram_hwm = backlog;
    }
    /* Re-arm long before the transfer count runs out (~45 min away). */
    if (dma_channel_hw_addr(rx_dma_chan)->transfer_count < 0x100000u) {
        dma_channel_abort(rx_dma_chan);
        rxdma_arm();
    }
}

/* Diagnostics: reset / report the RX stage counters. */
void serial_rx_stats_reset(void) {
    rx_sram_hwm = rx_psram_hwm = 0;
    rx_overrun_events = 0;
    serial_rx_dropped_bytes = 0;
}

void serial_rx_stats(uint32_t *sram_hwm, uint32_t *psram_hwm,
                     uint32_t *overruns, uint32_t *dropped) {
    if (sram_hwm)  *sram_hwm  = rx_sram_hwm;
    if (psram_hwm) *psram_hwm = rx_psram_hwm;
    if (overruns)  *overruns  = rx_overrun_events;
    if (dropped)   *dropped   = serial_rx_dropped_bytes;
}

static void serial_rx_pump_task(void *arg) {
    (void)arg;
    for (;;) {
        serial_rx_pump();
        vTaskDelay(1);                  /* one 2 ms tick */
    }
}

/* (Re)assign the TX/RX state machines to the given pins. Disables the SMs,
 * releases any previously-assigned pins back to inputs, resets the RX ring
 * buffer, then re-initialises both SM programs on the new pins. */
static void serial_apply_pins(uint8_t rx_pin, uint8_t tx_pin) {
    if (rx_dma_chan >= 0)
        dma_channel_abort(rx_dma_chan);
    pio_sm_set_enabled(SERIAL_PIO, tx_sm, false);
    pio_sm_set_enabled(SERIAL_PIO, rx_sm, false);

    /* Return the old pins to plain inputs so they stop driving the bus. */
    if (serial_inited) {
        gpio_init(cur_tx_pin);
        gpio_set_dir(cur_tx_pin, GPIO_IN);
        gpio_init(cur_rx_pin);
        gpio_set_dir(cur_rx_pin, GPIO_IN);
    }

    uart_tx_program_init(SERIAL_PIO, tx_sm, tx_offset, tx_pin, SERIAL_BAUD);
    uart_rx_program_init(SERIAL_PIO, rx_sm, rx_offset, rx_pin, SERIAL_BAUD);

    cur_tx_pin = tx_pin;
    cur_rx_pin = rx_pin;

    if (rx_dma_chan >= 0) {
        rxdma_arm();
        sram_rd = rxdma_produced();     /* discard in-flight garbage */
        rxps_rd = rxps_wr;
    }
}

void serial_init(void) {
    /* RX ring: aligned inside a double-size SRAM block (DMA ring wrap
     * requires the base aligned to the ring size).  Comes from the
     * FreeRTOS heap — the pre-DMA driver spent 32KB there on the ISR
     * ring, so 16KB is a net saving.  NOT PSRAM: DMA writes through the
     * QMI window fight flash XIP and glitch the display. */
    {
        uint8_t *blk = (uint8_t *)pvPortMalloc(RXDMA_RING_SIZE * 2);
        if (!blk) {
            printf("PIO UART: FATAL — RX ring alloc failed\n");
            return;
        }
        rx_ring = (uint8_t *)(((uintptr_t)blk + RXDMA_RING_MASK) &
                              ~(uintptr_t)RXDMA_RING_MASK);
    }

    /* PIO2 gpio base must be set BEFORE claiming SMs or initialising
     * programs so pin offsets resolve against the right window. Skip
     * when it already matches — the SDK call fails (harmlessly) once
     * another driver (audio) has claimed SMs on this PIO. */
    if (pio_get_gpio_base(SERIAL_PIO) != SERIAL_PIO_GPIO_BASE) {
        int base_rc = pio_set_gpio_base(SERIAL_PIO, SERIAL_PIO_GPIO_BASE);
        if (base_rc != 0)
            printf("PIO UART: pio_set_gpio_base failed (%d)\n", base_rc);
    }

    tx_sm = pio_claim_unused_sm(SERIAL_PIO, true);
    rx_sm = pio_claim_unused_sm(SERIAL_PIO, true);

    tx_offset = pio_add_program(SERIAL_PIO, &uart_tx_program);
    rx_offset = pio_add_program(SERIAL_PIO, &uart_rx_program);

    /* Pick pins: saved config (if valid) overrides the board defaults. */
    uint8_t rx = PIN_RX, tx = PIN_TX;
    wifi_config_t *cfg = wifi_config_get();
    if (cfg && cfg->rx_pin >= SERIAL_PIN_MIN && cfg->rx_pin <= SERIAL_PIN_MAX &&
        cfg->tx_pin >= SERIAL_PIN_MIN && cfg->tx_pin <= SERIAL_PIN_MAX &&
        cfg->rx_pin != cfg->tx_pin) {
        rx = cfg->rx_pin;
        tx = cfg->tx_pin;
    }

    serial_apply_pins(rx, tx);
    serial_inited = true;

    printf("PIO UART: TX sm=%u pin=%d, RX sm=%u pin=%d, baud=%u\n",
           (unsigned)tx_sm, cur_tx_pin,
           (unsigned)rx_sm, cur_rx_pin,
           SERIAL_BAUD);

    rxps_ring = (uint8_t *)psram_alloc(RXPS_RING_SIZE);
    if (!rxps_ring) {
        printf("PIO UART: FATAL — PSRAM RX ring alloc failed\n");
        return;
    }

    rx_dma_chan = dma_claim_unused_channel(true);
    rxdma_arm();

    xTaskCreate(serial_rx_pump_task, "serialrx", 512, NULL, 4, NULL);

    printf("PIO UART: ready (DMA %uKB SRAM + pump %uKB PSRAM)\n",
           RXDMA_RING_SIZE / 1024, RXPS_RING_SIZE / 1024);
}

void serial_set_pins(uint8_t rx_pin, uint8_t tx_pin) {
    if (!serial_inited)
        return;
    if (rx_pin < SERIAL_PIN_MIN || rx_pin > SERIAL_PIN_MAX ||
        tx_pin < SERIAL_PIN_MIN || tx_pin > SERIAL_PIN_MAX ||
        rx_pin == tx_pin)
        return;
    if (rx_pin == cur_rx_pin && tx_pin == cur_tx_pin)
        return;

    serial_apply_pins(rx_pin, tx_pin);

    printf("PIO UART: reconfigured TX pin=%d, RX pin=%d\n", cur_tx_pin, cur_rx_pin);
}

uint8_t serial_get_rx_pin(void) { return cur_rx_pin; }
uint8_t serial_get_tx_pin(void) { return cur_tx_pin; }

/* Runtime baud switching (netcard fast-baud negotiation).  Re-inits both
 * state machines on the current pins at the new rate and resets the RX
 * ring — any in-flight bytes at the old rate are garbage anyway. */
static uint32_t cur_baud = SERIAL_BAUD;

void serial_set_baud(uint32_t baud) {
    if (!serial_inited || baud == cur_baud) return;
    /* Critical section: the pump task (priority 4) must not run while
     * the DMA is down or while we reset its cursors.  Abort the RX DMA
     * before re-initing the SM: pio_sm_init clears the FIFOs, and a
     * stale DREQ credit would make the channel pop an empty FIFO
     * (phantom garbage byte, possible credit desync). */
    taskENTER_CRITICAL();
    dma_channel_abort(rx_dma_chan);
    pio_sm_set_enabled(SERIAL_PIO, tx_sm, false);
    pio_sm_set_enabled(SERIAL_PIO, rx_sm, false);
    uart_tx_program_init(SERIAL_PIO, tx_sm, tx_offset, cur_tx_pin, baud);
    uart_rx_program_init(SERIAL_PIO, rx_sm, rx_offset, cur_rx_pin, baud);
    rxdma_arm();
    sram_rd = rxdma_produced();    /* discard old-baud garbage */
    rxps_rd = rxps_wr;
    cur_baud = baud;
    taskEXIT_CRITICAL();
    printf("PIO UART: baud -> %u\n", (unsigned)baud);
}

uint32_t serial_get_baud(void) { return cur_baud; }

void serial_send_char(char c) {
    pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)c);
}

void serial_send_string(const char *s) {
    while (*s)
        pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)*s++);
}

void serial_send_data(const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++)
        pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)data[i]);
}

bool serial_readable(void) {
    static uint32_t overruns_reported;
    if (rx_overrun_events != overruns_reported) {   /* consumer context */
        overruns_reported = rx_overrun_events;
        printf("PIO UART: RX OVERRUN x%u (pump lapped by DMA)\n",
               (unsigned)rx_overrun_events);
    }
    return rxps_wr != rxps_rd;
}

uint8_t serial_read_byte(void) {
    /* Pump runs at priority 4 and preempts every consumer on the next
     * tick, so a tight wait here cannot starve it. */
    while (rxps_wr == rxps_rd)
        tight_loop_contents();
    uint8_t c = rxps_ring[rxps_rd & RXPS_RING_MASK];
    rxps_rd++;
    serial_rx_total_bytes++;
    return c;
}
