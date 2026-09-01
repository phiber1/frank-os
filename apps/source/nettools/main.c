/*
 * NetTools — Network Diagnostic Utility for FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Two tabs: Ping (ICMP via AT+PING) and DNS (resolve hostname).
 * Uses netcard API via sys_table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"
#include <string.h>

/* App-local translations */
enum { AL_PING, AL_DNS, AL_GET, AL_HOST, AL_URL, AL_COUNT };
static const char *al_en[] = { [AL_PING] = "Ping", [AL_DNS] = "DNS",
                               [AL_GET] = "Get", [AL_HOST] = "Host:",
                               [AL_URL] = "URL [dest]:" };
static const char *al_ru[] = {
    [AL_PING] = "\xD0\x9F\xD0\xB8\xD0\xBD\xD0\xB3",
    [AL_DNS]  = "DNS",
    [AL_GET]  = "Get",
    [AL_HOST] = "\xD0\xA5\xD0\xBE\xD1\x81\xD1\x82:",
    [AL_URL]  = "URL [dest]:",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/* ========================================================================
 * sys_table wrappers for network + debug API
 * ======================================================================== */

static inline bool netcard_wifi_connected(void) {
    typedef bool (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[538])();
}

static inline bool netcard_resolve(const char *hostname, char *ip_out, int ip_out_size) {
    typedef bool (*fn_t)(const char *, char *, int);
    return ((fn_t)_sys_table_ptrs[549])(hostname, ip_out, ip_out_size);
}

static inline int netcard_ping(const char *host, uint16_t port) {
    typedef int (*fn_t)(const char *, uint16_t);
    return ((fn_t)_sys_table_ptrs[550])(host, port);
}

static inline bool netcard_socket_open(uint8_t id, bool tls,
                                       const char *host, uint16_t port) {
    typedef bool (*fn_t)(uint8_t, bool, const char *, uint16_t);
    return ((fn_t)_sys_table_ptrs[540])(id, tls, host, port);
}

static inline void netcard_abort_wait(void) {
    /* Slot 555 is absent (0) on older OS builds — guard before calling. */
    typedef void (*fn_t)(void);
    if (_sys_table_ptrs[555])
        ((fn_t)_sys_table_ptrs[555])();
}

static inline bool netcard_socket_send(uint8_t id, const uint8_t *d, uint16_t n) {
    typedef bool (*fn_t)(uint8_t, const uint8_t *, uint16_t);
    return ((fn_t)_sys_table_ptrs[541])(id, d, n);
}

static inline void netcard_socket_close(uint8_t id) {
    typedef void (*fn_t)(uint8_t);
    ((fn_t)_sys_table_ptrs[542])(id);
}

typedef void (*nc_data_cb_t)(uint8_t id, const uint8_t *data, uint16_t len);
typedef void (*nc_close_cb_t)(uint8_t id);

static inline void *os_psram_alloc(uint32_t size) {
    typedef void *(*fn_t)(uint32_t);
    return ((fn_t)_sys_table_ptrs[491])(size);
}

static inline void os_psram_free(void *p) {
    typedef void (*fn_t)(void *);
    ((fn_t)_sys_table_ptrs[492])(p);
}

static inline void netcard_set_data_callback(nc_data_cb_t cb) {
    typedef void (*fn_t)(nc_data_cb_t);
    ((fn_t)_sys_table_ptrs[543])(cb);
}

static inline void netcard_set_close_callback(nc_close_cb_t cb) {
    typedef void (*fn_t)(nc_close_cb_t);
    ((fn_t)_sys_table_ptrs[544])(cb);
}

#define dbg_printf(...) ((int(*)(const char*,...))_sys_table_ptrs[438])(__VA_ARGS__)

/* ========================================================================
 * Constants
 * ======================================================================== */

#define TAB_PING   0
#define TAB_DNS    1
#define TAB_GET    2

/* Wide enough that a full "get http://host:port/path dest" line fits the
 * input field without the textarea collapsing into scrollbars. */
#define CLIENT_W   560
#define CLIENT_H   260

#define TAB_TOP     8           /* same padding as bottom margin */
#define TAB_H       22
#define TAB_W       60
#define LABEL_Y     (TAB_TOP + TAB_H + 8)
#define INPUT_Y     (LABEL_Y + FONT_UI_HEIGHT + 2)
#define INPUT_H     18
#define INPUT_X     8
#define INPUT_W     (CLIENT_W - 16)

#define BTN_W       70
#define BTN_H       22
#define BTN_Y       (INPUT_Y + INPUT_H + 6)

#define RESULT_Y    (BTN_Y + BTN_H + 6)
#define RESULT_H    (CLIENT_H - RESULT_Y - 8)
#define RESULT_X    8
#define RESULT_W    (CLIENT_W - 16)

#define INPUT_BUF_SIZE   128
#define RESULT_BUF_SIZE  512

/* ========================================================================
 * Application state
 * ======================================================================== */

typedef struct {
    hwnd_t      hwnd;
    uint8_t     tab;
    textarea_t  input_ta;
    char        input_buf[INPUT_BUF_SIZE];
    textarea_t  result_ta;
    char        result_buf[RESULT_BUF_SIZE];
    bool        busy;
    bool        closing;
    int8_t      btn_pressed;     /* -1=none, 0=pressed */
    uint8_t     wait_dots;       /* 0-3 for animation */
    TimerHandle_t blink_timer;
} app_t;

static app_t app;
static TaskHandle_t app_task;

/* ========================================================================
 * Blink timer callback — cursor blink + wait dots animation
 * ======================================================================== */

static void blink_cb(TimerHandle_t t) {
    (void)t;
    app.input_ta.cursor_visible = !app.input_ta.cursor_visible;
    app.result_ta.cursor_visible = false;  /* always hidden */
    if (app.busy)
        app.wait_dots = (app.wait_dots + 1) % 4;
    wm_invalidate(app.hwnd);
}

/* ========================================================================
 * Drawing helpers
 * ======================================================================== */

static void draw_tab(int16_t x, int16_t y, int16_t w, int16_t h,
                     const char *label, bool active) {
    if (active) {
        wd_fill_rect(x, y, w, h + 2, THEME_BUTTON_FACE);
        wd_hline(x, y, w, COLOR_WHITE);
        wd_vline(x, y, h + 2, COLOR_WHITE);
        wd_vline(x + w - 1, y, h + 1, COLOR_DARK_GRAY);
        wd_vline(x + w - 2, y + 1, h, COLOR_BLACK);
    } else {
        wd_fill_rect(x, y + 2, w, h - 2, THEME_BUTTON_FACE);
        wd_hline(x, y + 2, w, COLOR_WHITE);
        wd_vline(x, y + 2, h - 2, COLOR_WHITE);
        wd_vline(x + w - 1, y + 2, h - 2, COLOR_DARK_GRAY);
    }
    int tw = (int)strlen(label) * FONT_UI_WIDTH;
    int tx = x + (w - tw) / 2;
    int ty = y + (active ? (h - FONT_UI_HEIGHT) / 2 : (h - FONT_UI_HEIGHT) / 2 + 2);
    wd_text_ui(tx, ty, label, COLOR_BLACK, THEME_BUTTON_FACE);
}

/* ========================================================================
 * Paint handler
 * ======================================================================== */

static void app_paint(hwnd_t hwnd) {
    (void)hwnd;
    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    /* Tabs */
    draw_tab(4, TAB_TOP, TAB_W, TAB_H, AL(AL_PING), app.tab == TAB_PING);
    draw_tab(4 + TAB_W + 2, TAB_TOP, TAB_W, TAB_H, AL(AL_DNS), app.tab == TAB_DNS);
    draw_tab(4 + 2 * (TAB_W + 2), TAB_TOP, TAB_W, TAB_H, AL(AL_GET), app.tab == TAB_GET);

    /* Tab content border */
    wd_hline(0, TAB_TOP + TAB_H, CLIENT_W, COLOR_WHITE);

    /* Label */
    wd_text_ui(INPUT_X, LABEL_Y, AL(app.tab == TAB_GET ? AL_URL : AL_HOST),
               COLOR_BLACK, THEME_BUTTON_FACE);

    /* Input field — sunken border + textarea */
    wd_bevel_rect(INPUT_X - 2, INPUT_Y - 2, INPUT_W + 4, INPUT_H + 4,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
    textarea_paint(&app.input_ta);

    /* Button */
    {
        const char *btn_label = (app.busy && app.tab == TAB_GET) ? "Stop"
                              : app.tab == TAB_PING ? AL(AL_PING)
                              : app.tab == TAB_DNS  ? AL(AL_DNS) : AL(AL_GET);
        bool enabled = (!app.busy && textarea_get_length(&app.input_ta) > 0) ||
                       (app.busy && app.tab == TAB_GET);
        bool pressed = app.btn_pressed == 0;
        wd_button(INPUT_X, BTN_Y, BTN_W, BTN_H, btn_label, false, pressed);
        if (!enabled && !pressed) {
            int tw = (int)strlen(btn_label) * FONT_UI_WIDTH;
            int tx = INPUT_X + (BTN_W - tw) / 2;
            int ty = BTN_Y + (BTN_H - FONT_UI_HEIGHT) / 2;
            wd_text_ui(tx, ty, btn_label, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        }
    }

    /* Status label with animated dots */
    if (app.busy) {
        const char *dots = &"..."[3 - app.wait_dots];
        char wait_text[24];
        snprintf(wait_text, sizeof(wait_text), "Please wait%s", dots);
        wd_text_ui(INPUT_X + BTN_W + 8, BTN_Y + (BTN_H - FONT_UI_HEIGHT) / 2,
                   wait_text, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }

    /* Result area — sunken border + textarea */
    wd_bevel_rect(RESULT_X - 2, RESULT_Y - 2, RESULT_W + 4, RESULT_H + 4,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
    textarea_paint(&app.result_ta);

    wd_end();
}

/* ========================================================================
 * Operations (run on app task, blocking is OK)
 * ======================================================================== */

static void do_ping(void) {
    const char *host = textarea_get_text(&app.input_ta);
    app.busy = true;
    app.wait_dots = 0;
    textarea_set_text(&app.result_ta, "", 0);
    wm_invalidate(app.hwnd);

    char buf[RESULT_BUF_SIZE];
    buf[0] = '\0';
    int offset = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int ms = netcard_ping(host, 80);
        if (ms >= 0) {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "Reply from %s: time=%dms\n", host, ms);
        } else {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "Request to %s timed out\n", host);
        }
        textarea_set_text(&app.result_ta, buf, offset);
        wm_invalidate(app.hwnd);

        if (i < 3)
            vTaskDelay(pdMS_TO_TICKS(500));
    }

    app.busy = false;
    wm_invalidate(app.hwnd);
}

/* ========================================================================
 * HTTP GET — download a file straight to the SD card.
 *
 * Input line:  http://host[:port]/path [destpath]
 * Dest defaults to "/" + basename of the URL path.  The body streams to
 * dest.part and is renamed into place only on success, so a dropped
 * connection never leaves a truncated file under the real name.
 *
 * The netcard data callback runs on the OS RX task; it only copies into
 * the ring buffer below and notifies the app task, which does all the
 * parsing and SD writes.
 * ======================================================================== */

/* Not provided by the m-os-api environment (string.h declares them, so
 * these definitions must not be static). */
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

#define GET_SOCK_ID    0

/* Monotonic wall-clock ms straight from the RP2350 hardware timer
 * (TIMERAWL, µs).  Immune to the FreeRTOS tick-width/tick-rate traps
 * that broke the transfer watchdog (16-bit app TickType_t vs 32-bit
 * kernel ticks). Unsigned subtraction handles the 71-min wrap. */
#define NOW_MS()  ((*(volatile uint32_t *)0x400B0028u) / 1000u)
#define GET_RING_SZ    (1024u * 1024u)  /* ~11s of buffering at 921600:
                                  SD write-latency and UI-repaint spikes
                                  must not overflow the ring (overflow
                                  aborts the transfer).  Allocated from
                                  PSRAM on first Get. */
#define GET_HDR_MAX    1024
#define GET_TIMEOUT_MS 15000

static uint8_t          *g_ring;
static volatile uint32_t g_rhead, g_rtail;      /* head: cb writes, tail: app reads */
static volatile bool     g_sock_closed;
static volatile bool     g_ring_overflow;
static volatile bool     g_abort;               /* Stop button pressed */
static volatile bool     g_go;                  /* explicit start latch: stray task
                                                   notifications must NOT re-fire an op */

static void get_data_cb(uint8_t id, const uint8_t *data, uint16_t len) {
    if (id != GET_SOCK_ID || !g_ring) return;
    uint32_t head = g_rhead;
    for (uint16_t i = 0; i < len; i++) {
        uint32_t next = (head + 1) % GET_RING_SZ;
        if (next == g_rtail) { g_ring_overflow = true; break; }
        g_ring[head] = data[i];
        head = next;
    }
    g_rhead = head;
    xTaskNotifyGive(app_task);
}

static void get_close_cb(uint8_t id) {
    if (id != GET_SOCK_ID) return;
    g_sock_closed = true;
    xTaskNotifyGive(app_task);
}

/* Append a line to the result textarea (keeps prior lines). */
static void get_status(const char *line) {
    int len = textarea_get_length(&app.result_ta);
    char buf[RESULT_BUF_SIZE];
    const char *cur = textarea_get_text(&app.result_ta);
    int n = snprintf(buf, sizeof(buf), "%.*s%s%s",
                     len, cur, len ? "\n" : "", line);
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    textarea_set_text(&app.result_ta, buf, n);
    wm_invalidate(app.hwnd);
}

/* Progress updates REPLACE the text after g_prog_base instead of
 * appending — an 840KB transfer would otherwise scroll dozens of lines
 * and overflow the result buffer. */
static int g_prog_base;

static void get_progress(const char *line) {
    const char *cur = textarea_get_text(&app.result_ta);
    char buf[RESULT_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "%.*s\n%s", g_prog_base, cur, line);
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    textarea_set_text(&app.result_ta, buf, n);
    wm_invalidate(app.hwnd);
}

static int ci_starts(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void do_get(void) {
    char url[INPUT_BUF_SIZE], host[80], path[128], dest[96], tmp[104];
    char line[96];
    int port = 80;

    app.busy = true;
    app.wait_dots = 0;
    textarea_set_text(&app.result_ta, "", 0);
    wm_invalidate(app.hwnd);

    /* ── Parse "URL [dest]" from the input line ── */
    strncpy(url, textarea_get_text(&app.input_ta), sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;
    dest[0] = 0;
    {
        char *sp = strchr(url, ' ');
        if (sp) {
            *sp++ = 0;
            while (*sp == ' ') sp++;
            if (*sp) {
                if (*sp != '/') { dest[0] = '/'; strncpy(dest + 1, sp, sizeof(dest) - 2); }
                else              strncpy(dest, sp, sizeof(dest) - 1);
                dest[sizeof(dest) - 1] = 0;
            }
        }
    }
    if (strncmp(url, "http://", 7) != 0) {
        get_status("Only http:// URLs are supported");
        goto out_nofile;
    }
    {
        const char *h = url + 7;
        const char *slash = strchr(h, '/');
        const char *colon = strchr(h, ':');
        if (colon && (!slash || colon < slash)) {
            int hl = (int)(colon - h);
            if (hl >= (int)sizeof(host)) hl = sizeof(host) - 1;
            memcpy(host, h, hl); host[hl] = 0;
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) { get_status("Bad port"); goto out_nofile; }
        } else {
            int hl = slash ? (int)(slash - h) : (int)strlen(h);
            if (hl >= (int)sizeof(host)) hl = sizeof(host) - 1;
            memcpy(host, h, hl); host[hl] = 0;
        }
        strncpy(path, slash ? slash : "/", sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        if (!host[0]) { get_status("Bad URL"); goto out_nofile; }
    }
    if (!dest[0]) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (!*base) { get_status("No filename in URL; give a dest path"); goto out_nofile; }
        snprintf(dest, sizeof(dest), "/%s", base);
    }
    snprintf(tmp, sizeof(tmp), "%s.part", dest);

    /* ── Connect and send the request ── */
    if (!g_ring) {
        g_ring = (uint8_t *)os_psram_alloc(GET_RING_SZ);
        if (!g_ring) { get_status("PSRAM ring alloc failed"); goto out_nofile; }
    }
    g_rhead = g_rtail = 0;
    g_sock_closed = false;
    g_ring_overflow = false;
    g_abort = false;
    netcard_set_data_callback(get_data_cb);
    netcard_set_close_callback(get_close_cb);

    snprintf(line, sizeof(line), "Connecting to %s:%d...", host, port);
    get_status(line);
    if (!netcard_socket_open(GET_SOCK_ID, false, host, (uint16_t)port)) {
        get_status(g_abort ? "Aborted" : "Connect failed");
        goto out_nofile;
    }
    {
        char req[300];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\n"
                         "Connection: close\r\nUser-Agent: FrankOS-NetTools\r\n\r\n",
                         path, host);
        if (!netcard_socket_send(GET_SOCK_ID, (const uint8_t *)req, (uint16_t)n)) {
            get_status("Send failed");
            goto out_sock;
        }
    }

    /* ── Receive: header phase, then stream body to dest.part ── */
    {
        static char hdr[GET_HDR_MAX];          /* header accumulator      */
        /* The OS FIL has private tail fields beyond the API's view of
         * the struct — pad the allocation so f_open can't scribble past. */
        static struct { FIL f; uint32_t pad[8]; } fil;
        int hdr_len = 0;
        bool in_body = false, file_open = false, ok = false;
        bool ram_mode = false;          /* body buffered in PSRAM, SD untouched */
        uint8_t *body_buf = NULL;
        long content_len = -1, received = 0, last_prog = 0;
        uint32_t last_data_ms = NOW_MS();
        uint8_t chunk[512];

        for (;;) {
            if (g_abort) {
                get_status("Aborted");
                break;
            }
            /* Drain the ring into chunk[] */
            uint32_t head = g_rhead;
            int n = 0;
            while (g_rtail != head && n < (int)sizeof(chunk)) {
                chunk[n++] = g_ring[g_rtail];
                g_rtail = (g_rtail + 1) % GET_RING_SZ;
            }

            if (g_ring_overflow) {
                snprintf(line, sizeof(line), "Overrun at %ld bytes (%s)",
                         received, ram_mode ? "RAM" : "SD");
                get_status(line);
                break;
            }
            if (n == 0) {
                if (g_sock_closed && g_rtail == g_rhead) {
                    /* Connection done: success if length matched (or unknown) */
                    if (in_body && (content_len < 0 || received >= content_len))
                        ok = true;
                    else
                        get_status(in_body ? "Connection lost mid-transfer"
                                           : "Connection closed before response");
                    break;
                }
                if ((NOW_MS() - last_data_ms) > GET_TIMEOUT_MS) {
                    snprintf(line, sizeof(line), "Timeout at %ld bytes",
                             received);
                    get_status(line);
                    break;
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            last_data_ms = NOW_MS();

            int off = 0;
            if (!in_body) {
                /* Accumulate until the blank line ends the header */
                while (off < n && hdr_len < GET_HDR_MAX - 1) {
                    hdr[hdr_len++] = chunk[off++];
                    if (hdr_len >= 4 && memcmp(&hdr[hdr_len - 4], "\r\n\r\n", 4) == 0) {
                        hdr[hdr_len] = 0;
                        int code = 0;
                        {
                            char *sp = strchr(hdr, ' ');
                            if (sp) code = atoi(sp + 1);
                        }
                        if (code != 200) {
                            snprintf(line, sizeof(line), "HTTP error %d", code);
                            get_status(line);
                            goto out_sock;
                        }
                        for (char *p = hdr; (p = strchr(p, '\n')) != NULL; ) {
                            p++;
                            if (ci_starts(p, "content-length:"))
                                content_len = atoi(p + 15);
                        }
                        /* Buffer the whole body in PSRAM when its size is
                         * known: SD write stalls then can't back up the
                         * receive path (the card is written in one fast
                         * sequential pass after the socket is done). */
                        if (content_len > 0 && content_len <= 6 * 1024 * 1024)
                            body_buf = (uint8_t *)os_psram_alloc((uint32_t)content_len);
                        if (body_buf) {
                            ram_mode = true;
                        } else {
                            if (f_open(&fil.f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
                                snprintf(line, sizeof(line), "Cannot create %s", tmp);
                                get_status(line);
                                goto out_sock;
                            }
                            file_open = true;
                        }
                        in_body = true;
                        snprintf(line, sizeof(line), "-> %s (%ld bytes, %s)",
                                 dest, content_len,
                                 ram_mode ? "RAM" : "SD");
                        get_status(line);
                        g_prog_base = textarea_get_length(&app.result_ta);
                        break;
                    }
                }
                if (!in_body) {
                    if (hdr_len >= GET_HDR_MAX - 1) { get_status("Header too large"); break; }
                    continue;
                }
            }
            if (off < n) {
                int blen = n - off;
                if (ram_mode) {
                    long room = content_len - received;
                    if (blen > room) blen = (int)room;   /* clamp overrun */
                    if (blen > 0)
                        memcpy(body_buf + received, chunk + off, (size_t)blen);
                    received += blen;
                } else {
                    UINT bw = 0;
                    if (f_write(&fil.f, chunk + off, (UINT)blen, &bw) != FR_OK ||
                        bw != (UINT)blen) {
                        get_status("SD write failed");
                        break;
                    }
                    received += blen;
                }
                if (received - last_prog >= 16384 ||
                    (content_len > 0 && received >= content_len)) {
                    last_prog = received;
                    snprintf(line, sizeof(line), "%ld / %ld bytes", received,
                             content_len);
                    get_progress(line);
                }
                if (content_len >= 0 && received >= content_len) { ok = true; break; }
            }
        }

        if (file_open) f_close(&fil.f);
        if (ok && ram_mode) {
            /* Transfer complete — now write PSRAM buffer to SD in one
             * fast sequential pass (32KB chunks). */
            get_progress("Writing to SD...");
            if (f_open(&fil.f, tmp, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
                long woff = 0;
                while (woff < received) {
                    UINT wlen = (received - woff > 32768)
                              ? 32768 : (UINT)(received - woff);
                    UINT bw = 0;
                    if (f_write(&fil.f, body_buf + woff, wlen, &bw) != FR_OK ||
                        bw != wlen) {
                        ok = false;
                        break;
                    }
                    woff += bw;
                }
                f_close(&fil.f);
                if (!ok) { f_unlink(tmp); get_progress("SD write failed"); }
            } else {
                ok = false;
                get_progress("Cannot create file on SD");
            }
        }
        if (ok) {
            f_unlink(dest);                    /* ignore error: may not exist */
            if (f_rename(tmp, dest) == FR_OK) {
                snprintf(line, sizeof(line), "Done: %s (%ld bytes)", dest, received);
                get_progress(line);
            } else {
                get_progress("Rename failed; file left as .part");
            }
        } else if (file_open && !ram_mode) {
            f_unlink(tmp);
        }
        if (body_buf) os_psram_free(body_buf);
    }

out_sock:
    if (!g_sock_closed) netcard_socket_close(GET_SOCK_ID);
out_nofile:
    netcard_set_data_callback(NULL);
    netcard_set_close_callback(NULL);
    /* Free the PSRAM ring after every transfer: the OS does not reclaim
     * an app's psram allocations at exit, so keeping it for the session
     * leaked 1MB per NetTools run (a day of transfers exhausted PSRAM
     * and crashed the next big app launch). */
    if (g_ring) { os_psram_free(g_ring); g_ring = NULL; }
    app.busy = false;
    wm_invalidate(app.hwnd);
}

static void do_resolve(void) {
    const char *host = textarea_get_text(&app.input_ta);
    app.busy = true;
    app.wait_dots = 0;
    textarea_set_text(&app.result_ta, "", 0);
    wm_invalidate(app.hwnd);

    char ip[16];
    char result[128];
    if (netcard_resolve(host, ip, sizeof(ip))) {
        snprintf(result, sizeof(result), "%s -> %s", host, ip);
    } else {
        snprintf(result, sizeof(result), "DNS lookup failed for %s", host);
    }
    textarea_set_text(&app.result_ta, result, strlen(result));

    app.busy = false;
    wm_invalidate(app.hwnd);
}

/* ========================================================================
 * Event handler
 * ======================================================================== */

static bool app_event(hwnd_t hwnd, const window_event_t *ev) {
    if (ev->type == WM_CLOSE) {
        app.closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    if (ev->type == WM_SETFOCUS) {
        if (app.blink_timer) xTimerStart(app.blink_timer, 0);
        return true;
    }
    if (ev->type == WM_KILLFOCUS) {
        if (app.blink_timer) xTimerStop(app.blink_timer, 0);
        app.input_ta.cursor_visible = false;
        wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_LBUTTONDOWN) {
        int mx = ev->mouse.x, my = ev->mouse.y;

        /* Button press animation (also active as "Stop" during a Get) */
        if (((!app.busy && textarea_get_length(&app.input_ta) > 0) ||
             (app.busy && app.tab == TAB_GET)) &&
            mx >= INPUT_X && mx < INPUT_X + BTN_W &&
            my >= BTN_Y && my < BTN_Y + BTN_H) {
            app.btn_pressed = 0;
            wm_invalidate(hwnd);
            return true;
        }

        /* Tab clicks (disabled when busy) */
        if (!app.busy && my >= TAB_TOP && my < TAB_TOP + TAB_H) {
            if (mx >= 4 && mx < 4 + TAB_W && app.tab != TAB_PING) {
                app.tab = TAB_PING;
                textarea_set_text(&app.input_ta, "", 0);
                textarea_set_text(&app.result_ta, "", 0);
                wm_invalidate(hwnd);
                return true;
            }
            if (mx >= 4 + TAB_W + 2 && mx < 4 + 2 * TAB_W + 2 && app.tab != TAB_DNS) {
                app.tab = TAB_DNS;
                textarea_set_text(&app.input_ta, "", 0);
                textarea_set_text(&app.result_ta, "", 0);
                wm_invalidate(hwnd);
                return true;
            }
            if (mx >= 4 + 2 * (TAB_W + 2) && mx < 4 + 2 * (TAB_W + 2) + TAB_W &&
                app.tab != TAB_GET) {
                app.tab = TAB_GET;
                textarea_set_text(&app.input_ta, "", 0);
                textarea_set_text(&app.result_ta, "", 0);
                wm_invalidate(hwnd);
                return true;
            }
        }

        /* Let input textarea handle clicks (only when not busy) */
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return true;
    }

    if (ev->type == WM_LBUTTONUP) {
        int mx = ev->mouse.x, my = ev->mouse.y;
        bool was_pressed = (app.btn_pressed == 0);
        app.btn_pressed = -1;

        if (was_pressed &&
            mx >= INPUT_X && mx < INPUT_X + BTN_W &&
            my >= BTN_Y && my < BTN_Y + BTN_H) {
            if (app.busy && app.tab == TAB_GET) {
                g_abort = true;                    /* Stop */
                netcard_abort_wait();              /* snap a blocking connect */
                xTaskNotifyGive(app_task);
            } else if (!app.busy && textarea_get_length(&app.input_ta) > 0) {
                g_go = true;
                xTaskNotifyGive(app_task);
            }
        }
        wm_invalidate(hwnd);

        if (!app.busy && textarea_event(&app.input_ta, ev))
            wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_MOUSEMOVE) {
        if (!app.busy && textarea_event(&app.input_ta, ev))
            wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_KEYDOWN) {
        uint8_t sc = ev->key.scancode;

        /* Enter — trigger operation; Esc — abort a running Get */
        if (sc == 0x28 && !app.busy && textarea_get_length(&app.input_ta) > 0) {
            g_go = true;
            xTaskNotifyGive(app_task);
            return true;
        }
        if (sc == 0x29 && app.busy && app.tab == TAB_GET) {
            g_abort = true;
            netcard_abort_wait();              /* snap a blocking connect */
            xTaskNotifyGive(app_task);
            return true;
        }

        /* Forward to input textarea when not busy */
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return false;
    }

    if (ev->type == WM_CHAR) {
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return true;
    }

    return false;
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

int main(void) {
    memset(&app, 0, sizeof(app));
    app.btn_pressed = -1;
    app_task = xTaskGetCurrentTaskHandle();

    int fw = CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int fh = CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    int wx = (DISPLAY_WIDTH - fw) / 2;
    if (wx < 0) wx = 0;
    app.hwnd = wm_create_window(
        wx, 60, fw, fh,
        "NetTools", WSTYLE_DIALOG,
        app_event, app_paint);

    if (app.hwnd == HWND_NULL)
        return 1;

    window_t *win = wm_get_window(app.hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;

    /* Input textarea — single line height */
    textarea_init(&app.input_ta, app.input_buf, INPUT_BUF_SIZE, app.hwnd);
    textarea_set_rect(&app.input_ta, INPUT_X, INPUT_Y, INPUT_W, INPUT_H);

    /* Result textarea — multi-line, read-only (cursor always hidden) */
    textarea_init(&app.result_ta, app.result_buf, RESULT_BUF_SIZE, app.hwnd);
    textarea_set_rect(&app.result_ta, RESULT_X, RESULT_Y, RESULT_W, RESULT_H);
    app.result_ta.cursor_visible = false;

    /* Cursor blink timer (500ms) */
    app.blink_timer = xTimerCreate("ntblink", pdMS_TO_TICKS(500),
                                   pdTRUE, 0, blink_cb);
    if (app.blink_timer)
        xTimerStart(app.blink_timer, 0);

    wm_show_window(app.hwnd);
    wm_set_focus(app.hwnd);
    taskbar_invalidate();

    /* Main loop */
    while (!app.closing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (app.closing)
            break;

        if (g_go && !app.busy && textarea_get_length(&app.input_ta) > 0) {
            g_go = false;
            if (!netcard_wifi_connected()) {
                textarea_set_text(&app.result_ta, "Not connected to WiFi", 21);
                wm_invalidate(app.hwnd);
                continue;
            }
            if (app.tab == TAB_PING)
                do_ping();
            else if (app.tab == TAB_DNS)
                do_resolve();
            else
                do_get();
        }
        g_go = false;
    }

    if (app.blink_timer) {
        xTimerStop(app.blink_timer, 0);
        xTimerDelete(app.blink_timer, 0);
    }
    wm_destroy_window(app.hwnd);
    taskbar_invalidate();
    return 0;
}
