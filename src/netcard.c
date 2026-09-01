/*
 * FRANK OS — ESP-01 Netcard AT Command Client
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * AT command client for the frank-netcard ESP-01 firmware.
 * Communicates over PIO UART using serial.h.
 *
 * Protocol:
 *   Commands:  AT+CMD=args\r\n
 *   Responses: OK\r\n, ERROR:reason\r\n, +TAG:data\r\n
 *   Boot:      +READY\r\n
 *   Binary RX: +SRECV:id,len\r\n followed by len raw bytes
 *   Binary TX: AT+SSEND=id,len\r\n -> >\r\n -> len raw bytes -> SEND OK/FAIL\r\n
 *   Async:     +SCLOSED:id, +WDISCONN, +WCONN:ip
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "serial.h"
#include "netcard.h"
#include "wifi_config.h"

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

#define NC_MAX_SOCKETS      4
#define NC_LINE_BUF_SIZE    96
#define NC_SRECV_BUF_SIZE   1100    /* 1024 data + header overhead */

#define NC_TIMEOUT_DEFAULT  5000    /* ms */
#define NC_TIMEOUT_LONG     25000   /* ms — ESP firmware has 15-20s internal timeouts */
#define NC_TIMEOUT_TLS      35000   /* ms — TLS handshake on ESP-01 is slow */

#define NC_ACTIVITY_WINDOW_MS 200   /* TX/RX icon flash duration */

/* -------------------------------------------------------------------------- */
/* Parser state machine                                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    NCS_READLINE,
    NCS_READDATA
} nc_state_t;

/* -------------------------------------------------------------------------- */
/* Command response status                                                    */
/* -------------------------------------------------------------------------- */

typedef enum {
    NC_RESP_NONE,
    NC_RESP_OK,
    NC_RESP_ERROR,
    NC_RESP_SEND_OK,
    NC_RESP_SEND_FAIL,
    NC_RESP_PROMPT
} nc_resp_t;

/* -------------------------------------------------------------------------- */
/* Command queue (GUI -> netcard task)                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    CMD_SCAN,
    CMD_JOIN,
    CMD_QUIT,
    CMD_SETPINS
} net_cmd_type_t;

typedef struct {
    net_cmd_type_t type;
    char ssid[33];
    char pass[65];
    uint8_t rx_pin;
    uint8_t tx_pin;
    nc_scan_cb_t  scan_cb;
    nc_cmd_done_cb_t done_cb;
} net_cmd_t;

#define CMD_QUEUE_LEN  4

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

static nc_state_t   state;
static char          line_buf[NC_LINE_BUF_SIZE];
static uint16_t      line_pos;

/* Byte-accounting diagnostics: compare wire bytes (serial layer) against
 * delivered +SRECV payload to localize loss.  wire ≈ payload + ~18/chunk
 * overhead → the C6 never sent the rest; wire ≫ payload + overhead →
 * bytes arrived but the parser mis-framed them. */
static uint32_t nc_payload_total, nc_chunk_total, nc_desync_events;
static bool     nc_in_desync;
static bool     nc_stats_printed;

/* Print the per-connection byte accounting + ring diagnostics.  Called
 * from both close paths (local AT+SCLOSE and async +SCLOSED) — once. */
static void nc_print_stats(void) {
    if (nc_stats_printed || nc_payload_total == 0)
        return;
    nc_stats_printed = true;
    uint32_t shwm, phwm, ovr, drp;
    serial_rx_stats(&shwm, &phwm, &ovr, &drp);
    printf("[NC] stats: wire=%u payload=%u chunks=%u overhead=%d baud=%u\n"
           "[NC] rings: sram_hwm=%u/8192 psram_hwm=%u/1048576 "
           "overruns=%u dropped=%u desyncs=%u\n",
           (unsigned)serial_rx_total_bytes, (unsigned)nc_payload_total,
           (unsigned)nc_chunk_total,
           (int)(serial_rx_total_bytes - nc_payload_total),
           (unsigned)serial_get_baud(),
           (unsigned)shwm, (unsigned)phwm, (unsigned)ovr, (unsigned)drp,
           (unsigned)nc_desync_events);
}

/* Binary receive state (NCS_READDATA) — heap-allocated to save BSS */
static uint8_t      *srecv_buf;
static uint16_t      data_remaining;
static uint16_t      data_pos;
static uint8_t       data_socket_id;

/* Command serialisation */
static volatile nc_resp_t cmd_response;

/* WiFi state */
static volatile bool wifi_connected;
static char          wifi_ip_str[16];

/* Hardware availability */
static volatile bool netcard_available_flag;

/* DNS resolve result (filled by +RESOLVE: response) */
static char resolve_result[16];

/* Ping result (filled by +PING: response) */
static volatile int ping_last_ms;

/* Async callbacks */
static nc_data_cb_t  cb_data;
static nc_close_cb_t cb_close;
static nc_wifi_cb_t  cb_wifi;

/* Scan callback (transient, only valid during netcard_wifi_scan) */
static nc_scan_cb_t  cb_scan;
static int           scan_count;

/* Boot flag */
static volatile bool got_ready;

/* Command queue */
static QueueHandle_t cmd_queue;

/* Activity tracking for tray icon */
static volatile uint32_t last_tx_tick;
static volatile uint32_t last_rx_tick;

/* -------------------------------------------------------------------------- */
/* Simple integer parser (replaces sscanf)                                    */
/* -------------------------------------------------------------------------- */

static unsigned int parse_uint(const char *s, const char **endp) {
    unsigned int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    if (endp) *endp = s;
    return val;
}

/* -------------------------------------------------------------------------- */
/* Low-level serial helpers                                                   */
/* -------------------------------------------------------------------------- */

/* Declared in taskbar.c — trigger tray icon repaint on network activity */
extern void taskbar_invalidate(void);

static void nc_send_cmd(const char *cmd) {
    printf("[NC TX] %s\n", cmd);
    serial_send_string(cmd);
    serial_send_string("\r\n");
    last_tx_tick = xTaskGetTickCount();
    if (wifi_connected)
        taskbar_invalidate();
}

/* -------------------------------------------------------------------------- */
/* Line dispatcher                                                            */
/* -------------------------------------------------------------------------- */

/* Log a received line with control bytes hex-escaped.  Raw line noise
 * (e.g. framing garbage during a baud switch) can contain terminal
 * escape sequences that silently wedge the user's console emulator —
 * output keeps flowing but the terminal stops rendering it. */
static void nc_log_rx_line(const char *line) {
    /* Hard rate-limit: a stream desync can spray hundreds of garbage
     * lines per second, and that printf flood loads the USB console
     * path exactly when the RX machinery is under full-rate stress —
     * the logging itself can then perpetuate the loss cascade. */
    static uint32_t win_start, win_count, suppressed;
    uint32_t now = xTaskGetTickCount();
    if ((uint32_t)(now - win_start) > pdMS_TO_TICKS(1000)) {
        if (suppressed)
            printf("[NC RX] ... %u lines suppressed\n", (unsigned)suppressed);
        win_start = now;
        win_count = 0;
        suppressed = 0;
    }
    if (++win_count > 25) {
        suppressed++;
        return;
    }
    char buf[100];
    unsigned n = 0;
    for (const char *p = line; *p && n < sizeof(buf) - 5; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f)
            buf[n++] = (char)c;
        else
            n += (unsigned)snprintf(&buf[n], 5, "\\x%02x", c);
    }
    buf[n] = 0;
    printf("[NC RX] %s\n", buf);
}

static void nc_process_line(const char *line, uint16_t len) {
    /* Do NOT log per-chunk +SRECV lines: an 840KB transfer emits 800+ of
     * them, and if the USB CDC console backs up, printf BLOCKS this task
     * — the very task that delivers socket data — starving the receiving
     * app into a timeout while the C6 keeps streaming. */
    if (strncmp(line, "+SRECV:", 7) != 0)
        nc_log_rx_line(line);

    /* ---- Final responses ---- */

    if (strcmp(line, "OK") == 0) {
        cmd_response = NC_RESP_OK;
        return;
    }

    if (strncmp(line, "ERROR:", 6) == 0) {
        printf("[NC] %s\n", line);
        cmd_response = NC_RESP_ERROR;
        return;
    }

    if (strcmp(line, "SEND OK") == 0) {
        cmd_response = NC_RESP_SEND_OK;
        return;
    }

    if (strcmp(line, "SEND FAIL") == 0) {
        cmd_response = NC_RESP_SEND_FAIL;
        return;
    }

    if (strcmp(line, ">") == 0) {
        cmd_response = NC_RESP_PROMPT;
        return;
    }

    /* ---- Unsolicited Result Codes (URCs) ---- */

    if (strcmp(line, "+READY") == 0) {
        got_ready = true;
        return;
    }

    /* +SRECV:id,len — switch to binary-read mode.  The marker is also
     * accepted MID-line: a lost byte mid-payload shifts stream framing,
     * so the next real header lands behind payload-tail garbage and a
     * prefix match misses it — without this, one lost byte garbles the
     * whole rest of the connection.  A resynced transfer has a hole;
     * receivers detect that via content-length. */
    {
        const char *sync = (strncmp(line, "+SRECV:", 7) == 0)
                         ? line : strstr(line, "+SRECV:");
        if (sync) {
            const char *p = sync + 7;
            unsigned int id = parse_uint(p, &p);
            if (*p == ',') p++;
            unsigned int dlen = parse_uint(p, NULL);
            if (id < NC_MAX_SOCKETS && dlen > 0) {
                if (dlen > NC_SRECV_BUF_SIZE)
                    dlen = NC_SRECV_BUF_SIZE;
                data_socket_id = (uint8_t)id;
                data_remaining = (uint16_t)dlen;
                data_pos = 0;
                state = NCS_READDATA;
                nc_in_desync = false;
            }
            return;
        }
    }

    /* +SCLOSED:id — peer closed socket */
    if (strncmp(line, "+SCLOSED:", 9) == 0) {
        unsigned int id = parse_uint(line + 9, NULL);
        nc_print_stats();
        if (id < NC_MAX_SOCKETS && cb_close)
            cb_close((uint8_t)id);
        return;
    }

    /* +WJOIN:ip,subnet,gateway — WiFi join response (before OK).
     * Set connected state immediately so the UI reflects it when the
     * blocking netcard_wifi_join() returns. */
    if (strncmp(line, "+WJOIN:", 7) == 0) {
        wifi_connected = true;
        /* Extract IP (first field before comma) */
        const char *p = line + 7;
        int i = 0;
        while (*p && *p != ',' && i < (int)sizeof(wifi_ip_str) - 1)
            wifi_ip_str[i++] = *p++;
        wifi_ip_str[i] = '\0';
        taskbar_invalidate();   /* show network tray icon */
        if (cb_wifi)
            cb_wifi(true, wifi_ip_str);
        return;
    }

    /* +WCONN:ip — WiFi connected (async reconnect event) */
    if (strncmp(line, "+WCONN:", 7) == 0) {
        wifi_connected = true;
        strncpy(wifi_ip_str, line + 7, sizeof(wifi_ip_str) - 1);
        wifi_ip_str[sizeof(wifi_ip_str) - 1] = '\0';
        taskbar_invalidate();   /* show network tray icon */
        if (cb_wifi)
            cb_wifi(true, wifi_ip_str);
        return;
    }

    /* +WDISCONN — WiFi disconnected */
    if (strcmp(line, "+WDISCONN") == 0) {
        wifi_connected = false;
        wifi_ip_str[0] = '\0';
        taskbar_invalidate();   /* hide network tray icon */
        if (cb_wifi)
            cb_wifi(false, NULL);
        return;
    }

    /* +PING:seq,time_ms — individual ping result */
    if (strncmp(line, "+PING:", 6) == 0) {
        const char *p = line + 6;
        parse_uint(p, &p);  /* skip seq */
        if (*p == ',') p++;
        ping_last_ms = atoi(p);
        return;
    }

    /* +PINGSTAT:sent,received,avg_ms — ping summary (ignored, use per-ping) */
    if (strncmp(line, "+PINGSTAT:", 10) == 0)
        return;

    /* +RESOLVE:ip — DNS resolution result */
    if (strncmp(line, "+RESOLVE:", 9) == 0) {
        strncpy(resolve_result, line + 9, sizeof(resolve_result) - 1);
        resolve_result[sizeof(resolve_result) - 1] = '\0';
        return;
    }

    /* +WSCAN:ssid,rssi,enc,ch — scan result line */
    if (strncmp(line, "+WSCAN:", 7) == 0 && cb_scan) {
        char ssid[64];
        int rssi = 0, enc = 0, ch = 0;
        const char *p = line + 7;

        /* Parse from right: find the three rightmost commas */
        const char *commas[32];
        int ncommas = 0;
        const char *scan_p = p;
        while (*scan_p && ncommas < 32) {
            if (*scan_p == ',')
                commas[ncommas++] = scan_p;
            scan_p++;
        }
        if (ncommas >= 3) {
            const char *c1 = commas[ncommas - 3];
            const char *c2 = commas[ncommas - 2];
            const char *c3 = commas[ncommas - 1];

            uint16_t ssid_len = (uint16_t)(c1 - p);
            if (ssid_len >= sizeof(ssid))
                ssid_len = sizeof(ssid) - 1;
            memcpy(ssid, p, ssid_len);
            ssid[ssid_len] = '\0';

            rssi = atoi(c1 + 1);
            enc  = atoi(c2 + 1);
            ch   = atoi(c3 + 1);

            cb_scan(ssid, rssi, enc, ch);
            scan_count++;
        }
        return;
    }

    (void)len;
    /* Unknown lines are silently ignored — but an unknown line while
     * payload has been flowing (and we were in sync) marks the exact
     * moment stream framing was lost.  Timestamp each loss event with
     * the byte accounting; the +SRECV resync above ends the event. */
    if (nc_payload_total > 0 && !nc_in_desync) {
        nc_in_desync = true;
        if (nc_desync_events < 8)
            printf("[NC] DESYNC #%u at wire=%u payload=%u chunks=%u\n",
                   (unsigned)nc_desync_events + 1,
                   (unsigned)serial_rx_total_bytes,
                   (unsigned)nc_payload_total, (unsigned)nc_chunk_total);
        nc_desync_events++;
    }
}

/* -------------------------------------------------------------------------- */
/* netcard_poll — drain RX FIFO, feed through state machine                   */
/* -------------------------------------------------------------------------- */

/* Serializes the RX state machine: netcard_poll is called both by
 * netcard_task (continuously) and by the app task inside nc_wait_response
 * (during commands).  Unserialized, two tasks interleave on line_buf /
 * data_pos / data_remaining — the double-decrement skips the ==0 check,
 * the countdown wraps, and srecv_buf[data_pos++] runs off the heap
 * (observed hardfault: BFAR 0x20FFFEF7 during a transfer abort while
 * both tasks were polling).  Try-lock: a contended caller just returns —
 * the holder is already draining the same FIFO. */
static SemaphoreHandle_t poll_mtx;

static void netcard_poll_locked(void);

static void netcard_poll(void) {
    if (poll_mtx == NULL || xSemaphoreTake(poll_mtx, 0) != pdTRUE)
        return;
    netcard_poll_locked();
    xSemaphoreGive(poll_mtx);
}

static void netcard_poll_locked(void) {
    while (serial_readable()) {
        uint8_t c = serial_read_byte();

        switch (state) {

        case NCS_READLINE:
            if (c == '\n') {
                if (line_pos > 0 && line_buf[line_pos - 1] == '\r')
                    line_pos--;
                line_buf[line_pos] = '\0';

                if (line_pos > 0)
                    nc_process_line(line_buf, line_pos);

                line_pos = 0;
            } else {
                if (line_pos < NC_LINE_BUF_SIZE - 1)
                    line_buf[line_pos++] = (char)c;
            }
            break;

        case NCS_READDATA:
            srecv_buf[data_pos++] = c;
            nc_payload_total++;
            data_remaining--;
            if (data_remaining == 0) {
                nc_chunk_total++;
                last_rx_tick = xTaskGetTickCount();
                if (wifi_connected)
                    taskbar_invalidate();
                if (cb_data)
                    cb_data(data_socket_id, srecv_buf, data_pos);
                state = NCS_READLINE;
                line_pos = 0;
            }
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Blocking helpers — wait for a specific command response                     */
/* -------------------------------------------------------------------------- */

/* Set from another task (e.g. an app's Stop button) to snap the current
 * blocking command wait out of its poll loop.  Cleared at the start of
 * every command so a stale abort can't kill the next one. */
static volatile bool nc_abort_req;

void netcard_abort(void) {
    nc_abort_req = true;
}

static nc_resp_t nc_wait_response(uint32_t timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (cmd_response == NC_RESP_NONE) {
        if (nc_abort_req) {
            nc_abort_req = false;
            return NC_RESP_NONE;   /* caller sees a timeout */
        }
        if (xTaskGetTickCount() >= deadline)
            return NC_RESP_NONE;

        netcard_poll();
        vTaskDelay(1);
    }

    nc_resp_t r = cmd_response;
    cmd_response = NC_RESP_NONE;
    return r;
}

static bool nc_send_and_wait(const char *cmd, uint32_t timeout_ms) {
    netcard_poll();

    nc_abort_req = false;
    cmd_response = NC_RESP_NONE;

    printf("[NC] >> %s\n", cmd);
    nc_send_cmd(cmd);
    nc_resp_t r = nc_wait_response(timeout_ms);

    if (r != NC_RESP_OK)
        printf("[NC] << %s\n", r == NC_RESP_ERROR ? "ERROR" :
                                r == NC_RESP_NONE ? "TIMEOUT" : "OTHER");
    return (r == NC_RESP_OK);
}

/* -------------------------------------------------------------------------- */
/* Public API — WiFi                                                          */
/* -------------------------------------------------------------------------- */

bool netcard_wifi_join(const char *ssid, const char *pass) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+WJOIN=%s,%s", ssid, pass);
    return nc_send_and_wait(cmd, NC_TIMEOUT_LONG);
}

void netcard_wifi_quit(void) {
    nc_send_and_wait("AT+WQUIT", NC_TIMEOUT_DEFAULT);
    wifi_connected = false;
    wifi_ip_str[0] = '\0';
}

bool netcard_wifi_connected(void) {
    return wifi_connected;
}

const char *netcard_wifi_ip(void) {
    return wifi_ip_str;
}

/* -------------------------------------------------------------------------- */
/* Public API — DNS Resolve                                                   */
/* -------------------------------------------------------------------------- */

bool netcard_resolve(const char *hostname, char *ip_out, int ip_out_size) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+RESOLVE=%s", hostname);
    resolve_result[0] = '\0';
    bool ok = nc_send_and_wait(cmd, NC_TIMEOUT_DEFAULT);
    if (ok && resolve_result[0]) {
        strncpy(ip_out, resolve_result, ip_out_size - 1);
        ip_out[ip_out_size - 1] = '\0';
        return true;
    }
    if (ip_out_size > 0) ip_out[0] = '\0';
    return false;
}

/* -------------------------------------------------------------------------- */
/* Public API — ICMP Ping via AT+PING                                         */
/* -------------------------------------------------------------------------- */

int netcard_ping(const char *host, uint16_t port) {
    (void)port;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+PING=%s,1", host);
    ping_last_ms = -1;
    bool ok = nc_send_and_wait(cmd, NC_TIMEOUT_LONG);
    if (ok)
        return ping_last_ms;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API — WiFi Scan                                                     */
/* -------------------------------------------------------------------------- */

int netcard_wifi_scan(nc_scan_cb_t cb) {
    cb_scan = cb;
    scan_count = 0;

    bool ok = nc_send_and_wait("AT+WSCAN", NC_TIMEOUT_LONG);

    cb_scan = NULL;

    if (!ok)
        return -1;

    return scan_count;
}

/* -------------------------------------------------------------------------- */
/* Public API — Sockets                                                       */
/* -------------------------------------------------------------------------- */

bool netcard_socket_open(uint8_t id, bool tls, const char *host, uint16_t port) {
    if (id >= NC_MAX_SOCKETS)
        return false;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+SOPEN=%u,%s,%s,%u",
             (unsigned)id, tls ? "TLS" : "TCP", host, (unsigned)port);

    uint32_t timeout = tls ? NC_TIMEOUT_TLS : NC_TIMEOUT_LONG;

    /* Clear any stale socket with this id first (e.g. the C6 kept one
     * open across an OS reboot/crashed transfer — it is not power-cycled
     * by a BOOTSEL reflash).  Idempotent; ignore the result. */
    {
        char cls[32];
        snprintf(cls, sizeof(cls), "AT+SCLOSE=%u", (unsigned)id);
        nc_send_and_wait(cls, NC_TIMEOUT_DEFAULT);
    }

    /* Fresh byte accounting for this connection. */
    serial_rx_total_bytes = 0;
    nc_payload_total = nc_chunk_total = nc_desync_events = 0;
    nc_in_desync = false;
    nc_stats_printed = false;
    serial_rx_stats_reset();

    for (int attempt = 0; attempt < 3; attempt++) {
        if (nc_send_and_wait(cmd, timeout))
            return true;

        printf("[NC] SOPEN failed, retry %d/3...\n", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        netcard_poll();
    }
    return false;
}

bool netcard_socket_send(uint8_t id, const uint8_t *data, uint16_t len) {
    if (id >= NC_MAX_SOCKETS || len == 0)
        return false;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+SSEND=%u,%u", (unsigned)id, (unsigned)len);

    cmd_response = NC_RESP_NONE;

    nc_send_cmd(cmd);
    nc_resp_t r = nc_wait_response(NC_TIMEOUT_DEFAULT);

    if (r != NC_RESP_PROMPT)
        return false;

    serial_send_data(data, len);
    last_tx_tick = xTaskGetTickCount();

    cmd_response = NC_RESP_NONE;
    r = nc_wait_response(NC_TIMEOUT_DEFAULT);

    return (r == NC_RESP_SEND_OK);
}

void netcard_socket_close(uint8_t id) {
    if (id >= NC_MAX_SOCKETS)
        return;

    nc_print_stats();

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+SCLOSE=%u", (unsigned)id);
    nc_send_and_wait(cmd, NC_TIMEOUT_DEFAULT);
}

/* -------------------------------------------------------------------------- */
/* Public API — Async callbacks                                               */
/* -------------------------------------------------------------------------- */

void netcard_set_data_callback(nc_data_cb_t cb) {
    cb_data = cb;
}

void netcard_set_close_callback(nc_close_cb_t cb) {
    cb_close = cb;
}

void netcard_set_wifi_callback(nc_wifi_cb_t cb) {
    cb_wifi = cb;
}

/* -------------------------------------------------------------------------- */
/* Public API — Status                                                        */
/* -------------------------------------------------------------------------- */

bool netcard_is_available(void) {
    return netcard_available_flag;
}

net_icon_state_t netcard_get_icon_state(void) {
    TickType_t now = xTaskGetTickCount();
    bool tx = (now - last_tx_tick) < pdMS_TO_TICKS(NC_ACTIVITY_WINDOW_MS);
    bool rx = (now - last_rx_tick) < pdMS_TO_TICKS(NC_ACTIVITY_WINDOW_MS);
    if (tx && rx) return NET_ICON_TX_RX;
    if (tx) return NET_ICON_TX;
    if (rx) return NET_ICON_RX;
    return NET_ICON_NOACT;
}

/* -------------------------------------------------------------------------- */
/* Non-blocking command requests (GUI -> netcard task)                         */
/* -------------------------------------------------------------------------- */

void netcard_request_scan(nc_scan_cb_t scan_cb, nc_cmd_done_cb_t done_cb) {
    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SCAN;
    cmd.scan_cb = scan_cb;
    cmd.done_cb = done_cb;
    xQueueSend(cmd_queue, &cmd, 0);
}

void netcard_request_join(const char *ssid, const char *pass, nc_cmd_done_cb_t done_cb) {
    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_JOIN;
    strncpy(cmd.ssid, ssid, 32);
    cmd.ssid[32] = '\0';
    strncpy(cmd.pass, pass, 64);
    cmd.pass[64] = '\0';
    cmd.done_cb = done_cb;
    xQueueSend(cmd_queue, &cmd, 0);
}

void netcard_request_quit(nc_cmd_done_cb_t done_cb) {
    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_QUIT;
    cmd.done_cb = done_cb;
    xQueueSend(cmd_queue, &cmd, 0);
}

void netcard_request_setpins(uint8_t rx_pin, uint8_t tx_pin, nc_cmd_done_cb_t done_cb) {
    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SETPINS;
    cmd.rx_pin = rx_pin;
    cmd.tx_pin = tx_pin;
    cmd.done_cb = done_cb;
    xQueueSend(cmd_queue, &cmd, 0);
}

/* -------------------------------------------------------------------------- */
/* Netcard FreeRTOS task                                                      */
/* -------------------------------------------------------------------------- */

#define NETCARD_FAST_BAUD 921600u

/* After a successful AT handshake, negotiate up to the fast baud.
 * The C6 firmware replies OK at the old rate, flushes, waits 50 ms,
 * then switches.  On any verification failure we fall back to 115200
 * (older C6 firmware without AT+BAUD keeps working untouched). */
static void nc_negotiate_baud(void) {
    if (serial_get_baud() == NETCARD_FAST_BAUD)
        return;
    if (!nc_send_and_wait("AT+BAUD=921600", NC_TIMEOUT_DEFAULT)) {
        printf("[NC] AT+BAUD not supported, staying at %u\n",
               (unsigned)serial_get_baud());
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(120));   /* C6 switches 50 ms after OK */
    serial_set_baud(NETCARD_FAST_BAUD);
    /* Quietly drain line-transition garbage so it can't pollute the
     * verify handshake (or the console). */
    {
        unsigned junk = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
        while (serial_readable()) { serial_read_byte(); junk++; }
        if (junk)
            printf("[NC] drained %u transition bytes\n", junk);
    }
    if (nc_send_and_wait("AT", 1000) || nc_send_and_wait("AT", 1000)) {
        printf("[NC] link at %u baud\n", (unsigned)NETCARD_FAST_BAUD);
        return;
    }
    printf("[NC] fast baud verify failed — falling back to 115200\n");
    serial_set_baud(115200);
}

static bool netcard_probe_ex(int attempts, uint32_t timeout, uint32_t drain_ms) {
    /* Drain stale data */
    printf("[NC] Probe: draining RX for %u ms\n", (unsigned)drain_ms);
    TickType_t settle = xTaskGetTickCount() + pdMS_TO_TICKS(drain_ms);
    unsigned drained = 0;
    while (xTaskGetTickCount() < settle) {
        while (serial_readable()) {
            serial_read_byte();
            drained++;
        }
        vTaskDelay(1);
    }
    printf("[NC] Probe: drained %u bytes, starting AT handshake\n", drained);

    for (int attempt = 0; attempt < attempts; attempt++) {
        printf("[NC] Probe attempt %d/%d\n", attempt + 1, attempts);
        if (nc_send_and_wait("AT", timeout)) {
            nc_negotiate_baud();
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
        netcard_poll();
    }

    /* No answer at the current baud — the two sides can desync in both
     * directions: RP2350 rebooted (boots at 115200) while the C6 stayed
     * fast, or the C6 rebooted (boots at 115200) while we stayed fast.
     * Retry at the other rate; if we land at 115200, negotiate back up. */
    {
        uint32_t prev  = serial_get_baud();
        uint32_t other = (prev == NETCARD_FAST_BAUD) ? 115200u
                                                     : NETCARD_FAST_BAUD;
        printf("[NC] Probe: retrying at %u baud\n", (unsigned)other);
        serial_set_baud(other);
        vTaskDelay(pdMS_TO_TICKS(20));
        for (int attempt = 0; attempt < 2; attempt++) {
            if (nc_send_and_wait("AT", timeout)) {
                nc_negotiate_baud();
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            netcard_poll();
        }
        serial_set_baud(prev);
    }
    return false;
}

/* Boot-time probe: quick (2 attempts, short timeout) — a missing or
 * non-AT modem (e.g. Fruit Jam's stock ESP32-C6 firmware) shouldn't tie
 * up the netcard task for ~25 s at every boot. The user can re-probe
 * from Network settings at any time. */
static bool __attribute__((unused)) netcard_probe(void) {
    return netcard_probe_ex(2, 1500, 500);
}

/* After a successful probe: mark available and auto-reconnect if configured. */
static void netcard_on_modem_ready(void) {
    netcard_available_flag = true;
    printf("[NC] Modem ready\n");

    wifi_config_t *cfg = wifi_config_get();
    if (cfg->auto_connect && cfg->ssid[0]) {
        printf("[NC] Auto-connecting to %s...\n", cfg->ssid);
        if (netcard_wifi_join(cfg->ssid, cfg->password)) {
            printf("[NC] Connected to %s (IP: %s)\n", cfg->ssid, wifi_ip_str);
        } else {
            printf("[NC] Auto-connect failed\n");
        }
    }
}

extern void serial_init(void);

static void netcard_task(void *params) {
    (void)params;

    /* Initialize PIO UART here so its printfs land in the USB CDC log. */
    serial_init();

#ifdef FRANK_BOARD_FRUITJAM
    /* Fruit Jam: the onboard ESP32-C6 runs the frank-netcard firmware
     * (ported to C6, UART0 on GPIO 8/9). The DAC init pulsed the shared
     * PERIPH_RESET just before this task started, so the C6 is booting
     * now: drain its ROM boot chatter and allow three probe attempts.
     * With Adafruit's stock (NINA) firmware still installed the probe
     * fails quickly and harmlessly — flash the C6 and re-probe from
     * Network settings. */
    if (netcard_probe_ex(3, 1500, 800)) {
        netcard_on_modem_ready();
    } else {
        netcard_available_flag = false;
        printf("[NC] No netcard on ESP32-C6 — flash frank-netcard-c6 "
               "and re-probe from Network settings\n");
    }
#else
    /* Quick probe — if no modem, stay in the loop (unavailable) so the user
     * can reconfigure the RX/TX pins from the Network settings app. */
    if (netcard_probe()) {
        netcard_on_modem_ready();
    } else {
        netcard_available_flag = false;
        printf("[NC] No modem detected — awaiting pin reconfiguration\n");
    }
#endif

    /* Main loop: poll UART + process command queue */
    for (;;) {
        netcard_poll();

        /* Check for GUI command requests */
        net_cmd_t cmd;
        if (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CMD_SCAN: {
                if (!netcard_available_flag) {
                    if (cmd.done_cb) cmd.done_cb(false);
                    break;
                }
                int count = netcard_wifi_scan(cmd.scan_cb);
                if (cmd.done_cb)
                    cmd.done_cb(count >= 0);
                break;
            }
            case CMD_JOIN: {
                if (!netcard_available_flag) {
                    if (cmd.done_cb) cmd.done_cb(false);
                    break;
                }
                bool ok = netcard_wifi_join(cmd.ssid, cmd.pass);
                if (ok) {
                    /* Save credentials on successful connect */
                    wifi_config_t *wcfg = wifi_config_get();
                    strncpy(wcfg->ssid, cmd.ssid, 32);
                    wcfg->ssid[32] = '\0';
                    strncpy(wcfg->password, cmd.pass, 64);
                    wcfg->password[64] = '\0';
                    wcfg->auto_connect = 1;
                    wifi_config_save();
                }
                if (cmd.done_cb)
                    cmd.done_cb(ok);
                break;
            }
            case CMD_QUIT:
                netcard_wifi_quit();
                if (cmd.done_cb)
                    cmd.done_cb(true);
                break;
            case CMD_SETPINS: {
                /* Drop any existing connection before re-wiring the UART. */
                if (wifi_connected)
                    netcard_wifi_quit();
                netcard_available_flag = false;

                serial_set_pins(cmd.rx_pin, cmd.tx_pin);

                /* Quick interactive probe so the UI isn't frozen for long. */
                bool ok = netcard_probe_ex(3, 800, 300);
                if (ok) {
                    /* Persist the working pins. */
                    wifi_config_t *wcfg = wifi_config_get();
                    wcfg->rx_pin = cmd.rx_pin;
                    wcfg->tx_pin = cmd.tx_pin;
                    wifi_config_save();
                    netcard_on_modem_ready();
                } else {
                    printf("[NC] No modem on RX=%u TX=%u\n",
                           cmd.rx_pin, cmd.tx_pin);
                }
                if (cmd.done_cb)
                    cmd.done_cb(ok);
                break;
            }
            }
        }

        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------------------- */
/* Public API — Initialization                                                */
/* -------------------------------------------------------------------------- */

void netcard_init_async(void) {
    /* Reset state */
    state = NCS_READLINE;
    line_pos = 0;
    data_remaining = 0;
    cmd_response = NC_RESP_NONE;
    wifi_connected = false;
    wifi_ip_str[0] = '\0';
    cb_data = NULL;
    cb_close = NULL;
    cb_wifi = NULL;
    cb_scan = NULL;
    got_ready = false;
    netcard_available_flag = false;
    last_tx_tick = 0;
    last_rx_tick = 0;

    /* Allocate receive buffer from heap to save BSS space */
    srecv_buf = (uint8_t *)pvPortMalloc(NC_SRECV_BUF_SIZE);

    poll_mtx = xSemaphoreCreateMutex();

    cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(net_cmd_t));

    /* Priority 2 (== compositor): the netcard task is the serial RX
     * consumer, and at 921600 it must keep draining while the
     * compositor repaints transfer-progress UI — at priority 1 it fell
     * ~20KB/s behind under repaint load and overflowed the RX ring.
     * Round-robin with the compositor gives both plenty. */
    xTaskCreate(netcard_task, "netcard", 512, NULL, 2, NULL);
}
