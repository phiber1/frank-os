/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "terminal.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "menu.h"
#include "font.h"
#include "display.h"
#include "dialog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "timers.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "psram.h"
#include "lang.h"
#include "cmd.h"
#include "pico/platform.h"

/*==========================================================================
 * Text-mode buffer layout (MOS2-compatible)
 *
 * Each cell is 2 bytes: [character][color_attribute]
 * color_attribute = (bg << 4) | (fg & 0x0F)
 *
 * This is the SAME buffer returned by get_buffer() for MOS2 apps.
 * When MOS2 code calls save_console / restore_console / or writes
 * directly to the buffer, changes are reflected in the terminal.
 *=========================================================================*/

/* Only allocate the actual text content (70*20*2 = 2800 bytes).
 * op_console is clamped to get_buffer_size() so no overrun occurs. */

/* Buffer access macros — use dynamic cols from terminal */
#define TB_OFF(t, row, col)   ((row) * (t)->cols * 2 + (col) * 2)
#define TB_CHAR(t, r, c)   ((t)->textbuf[TB_OFF(t, r, c)])
#define TB_ATTR(t, r, c)   ((t)->textbuf[TB_OFF(t, r, c) + 1])
#define TB_PACK(fg, bg)    (((bg) << 4) | ((fg) & 0x0F))
#define TB_FG(attr)        ((attr) & 0x0F)
#define TB_BG(attr)        ((attr) >> 4)

/*==========================================================================
 * Helpers
 *=========================================================================*/

/* Free the oldest scrollback line, reclaiming its bytes. */
static void sb_free_oldest(terminal_t *t) {
    if (t->sb_count == 0) return;
    int tail = (t->sb_head - t->sb_count + t->sb_lines) % t->sb_lines;
    sb_line_t *e = &t->sb_slot[tail];
    if (e->cells) { vPortFree(e->cells); e->cells = NULL; }
    t->sb_bytes -= (int32_t)e->len * 2;
    e->len = 0;
    t->sb_count--;
}

/* Drop all scrollback history (used on resize/destroy). */
static void sb_clear(terminal_t *t) {
    while (t->sb_count > 0) sb_free_oldest(t);
    t->sb_head = 0;
    t->sb_bytes = 0;
    t->view_offset = 0;
}

/* Push textbuf row 0 (about to be discarded by a scroll) into scrollback.
 * Trims trailing blanks and stores the used cells in a right-sized SRAM
 * buffer; evicts oldest lines to stay within the line-count and byte budgets.
 * Kept out of SRAM (no __not_in_flash_func) since it touches only SRAM/heap —
 * there's no PSRAM here, so no QMI flash-fetch contention to avoid. */
static __attribute__((noinline)) void sb_push_row0(terminal_t *t, int cols) {
    volatile uint8_t *r0 = t->textbuf;                 /* row 0 cells */
    int len = cols;
    while (len > 0 && r0[(len - 1) * 2] == ' ') len--; /* trim trailing */
    int need = len * 2;

    /* Make room: never exceed sb_lines entries or the byte budget. */
    while (t->sb_count > 0 &&
           (t->sb_count >= t->sb_lines ||
            t->sb_bytes + need > SCROLLBACK_BUDGET_BYTES))
        sb_free_oldest(t);

    sb_line_t *e = &t->sb_slot[t->sb_head];
    e->cells = NULL;
    e->len = 0;
    if (need > 0) {
        uint8_t *buf = (uint8_t *)pvPortMalloc(need);
        if (buf) {
            for (int i = 0; i < need; i++) buf[i] = r0[i];
            e->cells = buf;
            e->len = (uint16_t)len;
            t->sb_bytes += need;
        }
        /* If malloc fails the slot stays empty — history degrades gracefully
         * (a blank line) rather than crashing. */
    }
    t->sb_head = (t->sb_head + 1) % t->sb_lines;
    t->sb_count++;

    /* If the user is scrolled back into history, keep the same content
     * anchored on screen as new lines push in (clamp at top of ring). */
    if (t->view_offset > 0) {
        t->view_offset++;
        if (t->view_offset > t->sb_count) t->view_offset = t->sb_count;
    }
}

/* Reconstruct the visible rows into `shadow` when scrolled back into history.
 * Each display row maps to a scrollback line (trimmed cells, padded to width)
 * or a live textbuf row.  Flash helper — touches only SRAM/heap. */
static __attribute__((noinline)) void sb_build_shadow(terminal_t *t,
                            uint8_t *shadow, int term_cols, int term_rows) {
    int rowbytes = term_cols * 2;
    uint8_t blank_attr = TB_PACK(t->fg_color, t->bg_color);
    for (int r = 0; r < term_rows; r++) {
        int combined = t->sb_count - t->view_offset + r;
        uint8_t *dst = shadow + r * rowbytes;
        if (combined < t->sb_count) {
            /* Scrollback line: chronological index `combined` (0 = oldest).
             * Copy the trimmed cells, then pad the row with blanks. */
            int ring = (t->sb_head - t->sb_count + combined + t->sb_lines)
                       % t->sb_lines;
            sb_line_t *e = &t->sb_slot[ring];
            int n = e->cells ? e->len : 0;
            if (n > term_cols) n = term_cols;
            for (int i = 0; i < n * 2; i++) dst[i] = e->cells[i];
            for (int c = n; c < term_cols; c++) {
                dst[c * 2]     = ' ';
                dst[c * 2 + 1] = blank_attr;
            }
        } else {
            /* Live textbuf row */
            volatile uint8_t *src =
                t->textbuf + (combined - t->sb_count) * rowbytes;
            for (int i = 0; i < rowbytes; i++) dst[i] = src[i];
        }
    }
}

static void __not_in_flash_func(terminal_scroll_up)(terminal_t *t) {
    /* Move rows 1..rows-1 up to rows 0..rows-2.
     * Manual loop instead of memmove() because memmove is in flash and
     * calling it on a PSRAM buffer causes CS0 (flash instruction fetch) +
     * CS1 (PSRAM data) QMI bus contention that hangs the system. */
    int cols = t->cols;
    int rows = t->rows;

    /* Push the top row (about to be discarded) into scrollback (flash helper —
     * no PSRAM involved, so it need not live in SRAM). */
    if (t->sb_slot)
        sb_push_row0(t, cols);

    volatile uint8_t *dst = t->textbuf;
    volatile uint8_t *src = t->textbuf + cols * 2;
    int n = cols * 2 * (rows - 1);
    for (int i = 0; i < n; i++) dst[i] = src[i];
    /* Clear last row */
    uint8_t attr = TB_PACK(t->fg_color, t->bg_color);
    uint8_t *last = t->textbuf + cols * 2 * (rows - 1);
    for (int i = 0; i < cols; i++) {
        last[i * 2]     = ' ';
        last[i * 2 + 1] = attr;
    }
}

static void terminal_input_push(terminal_t *t, uint8_t ch) {
    uint8_t next = (t->in_head + 1) & 63;
    if (next == t->in_tail) return; /* full — drop */
    t->input_buf[t->in_head] = ch;
    t->in_head = next;
    xSemaphoreGive(t->input_sem);
}

/* Scrollback view control.  view_offset counts lines back from the live
 * bottom (0 = showing live output).  Positive delta scrolls into history. */
static void terminal_scroll_view(terminal_t *t, int delta_lines) {
    int nv = t->view_offset + delta_lines;
    if (nv < 0) nv = 0;
    if (nv > t->sb_count) nv = t->sb_count;
    if (nv != t->view_offset) {
        t->view_offset = nv;
        wm_invalidate(t->hwnd);
    }
}

/* Snap the view back to live output (bottom).  Called on user input so
 * typing always jumps to where new characters appear. */
static void terminal_snap_bottom(terminal_t *t) {
    if (t->view_offset != 0) {
        t->view_offset = 0;
        wm_invalidate(t->hwnd);
    }
}

/*==========================================================================
 * Terminal ↔ Window association via user_data pointer
 *=========================================================================*/

static bool terminal_event(hwnd_t hwnd, const window_event_t *event);

terminal_t *terminal_from_hwnd(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win || win->event_handler != terminal_event) return NULL;
    return (terminal_t *)win->user_data;
}

hwnd_t terminal_get_hwnd(terminal_t *t) {
    return t ? t->hwnd : HWND_NULL;
}

/*==========================================================================
 * Per-task terminal via FreeRTOS TLS
 *=========================================================================*/

void terminal_set_task_terminal(terminal_t *t) {
    vTaskSetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(),
                                     TERMINAL_TLS_SLOT, t);
}

terminal_t *terminal_get_task_terminal(void) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return NULL;
    return (terminal_t *)pvTaskGetThreadLocalStoragePointer(
        xTaskGetCurrentTaskHandle(), TERMINAL_TLS_SLOT);
}

/*==========================================================================
 * Paint handler — draws the character grid using 8x16 font
 *
 * When the textbuf lives in PSRAM, thousands of individual byte reads
 * from the uncached XIP window (0x15000000) interleave with flash
 * instruction fetches through the shared QMI bus, which can cause
 * bus hangs.  We snapshot the textbuf into a SRAM shadow buffer once
 * via memcpy, then render entirely from SRAM.
 *=========================================================================*/

static uint8_t paint_shadow[TERM_MAX_TEXTBUF_SIZE];

static void __not_in_flash_func(terminal_paint)(hwnd_t hwnd) {
    terminal_t *t = terminal_from_hwnd(hwnd);
    if (!t || !t->textbuf) return;

    int term_cols = t->cols;
    int term_rows = t->rows;
    int bufsize = term_cols * term_rows * 2;

    /* Snapshot the visible content into SRAM — one bulk copy instead of
     * thousands of individual reads during the rendering loop.  Manual loop
     * instead of memcpy() because memcpy is in flash and calling it on a
     * PSRAM source causes QMI bus contention (CS0 instruction fetch + CS1
     * data read simultaneously → bus hang).  volatile source prevents the
     * compiler from converting this back into a memcpy call.
     *
     * The visible window is a slice of the virtual buffer
     *   [ sb_count scrollback lines ] ++ [ term_rows live textbuf lines ]
     * starting at virtual index (sb_count - view_offset).  When view_offset
     * is 0 every display row maps to a live textbuf row (fast common case). */
    if (t->view_offset == 0 || !t->sb_slot) {
        volatile uint8_t *src = t->textbuf;
        for (int i = 0; i < bufsize; i++)
            paint_shadow[i] = src[i];
    } else {
        sb_build_shadow(t, paint_shadow, term_cols, term_rows);
    }

    /* Compute client-area origin in screen coordinates directly,
     * bypassing wd_begin/wd_end to avoid per-pixel clipping overhead. */
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    int ox, oy, client_w;
    if (win->flags & WF_BORDER) {
        point_t origin = theme_client_origin(&win->frame, win->flags);
        rect_t  client = theme_client_rect(&win->frame, win->flags);
        ox = origin.x;
        oy = origin.y;
        client_w = client.w;
    } else {
        ox = win->frame.x;
        oy = win->frame.y;
        client_w = win->frame.w;
    }

    /* Draw character grid using fast glyph blitter */
    for (int row = 0; row < term_rows; row++) {
        int sy = oy + row * TERM_FONT_H;
        if (sy + TERM_FONT_H <= 0 || sy >= display_height) continue;

        for (int col = 0; col < term_cols; col++) {
            int sx = ox + col * TERM_FONT_W;
            if (sx + TERM_FONT_W <= 0 || sx >= display_width) continue;

            int off = (row * term_cols + col) * 2;
            uint8_t ch   = paint_shadow[off];
            uint8_t attr = paint_shadow[off + 1];
            uint8_t fg   = TB_FG(attr);
            uint8_t bg   = TB_BG(attr);

            const uint8_t *glyph = font8x16_get_glyph(ch);

            /* Fast path: even x and fully on-screen */
            if (!(sx & 1) &&
                sx >= 0 && (sx + TERM_FONT_W) <= display_width &&
                sy >= 0 && (sy + TERM_FONT_H) <= display_height) {
                display_blit_glyph_8wide(sx, sy, glyph, TERM_FONT_H, fg, bg);
            } else {
                /* Fallback: per-pixel for partially clipped chars */
                for (int gr = 0; gr < TERM_FONT_H; gr++) {
                    int py = sy + gr;
                    if ((unsigned)py >= (unsigned)display_height) continue;
                    uint8_t bits = glyph[gr];
                    for (int gc = 0; gc < TERM_FONT_W; gc++) {
                        int px = sx + gc;
                        if ((unsigned)px >= (unsigned)display_width) continue;
                        display_set_pixel_fast(px, py,
                                               (bits & (1 << gc)) ? fg : bg);
                    }
                }
            }
        }
    }

    /* Draw blinking DOS-style underline cursor (bottom 2 scanlines).
     * Hidden while scrolled back into history — the cursor belongs to the
     * live output, which isn't on screen there. */
    if (t->cursor_visible && t->view_offset == 0 &&
        t->cursor_col >= 0 && t->cursor_col < term_cols &&
        t->cursor_row >= 0 && t->cursor_row < term_rows) {
        int cx = ox + t->cursor_col * TERM_FONT_W;
        int cy = oy + t->cursor_row * TERM_FONT_H;
        display_hline_safe(cx, cy + TERM_FONT_H - 2, TERM_FONT_W, t->fg_color);
        display_hline_safe(cx, cy + TERM_FONT_H - 1, TERM_FONT_W, t->fg_color);
    }

    /* Vertical scrollbar down the right edge of the text area.  Uses the
     * wd_* client-drawing API, which is valid here because the compositor
     * wraps every paint handler in wd_begin(hwnd)/wd_end() with the client
     * origin already set — the same origin the grid above was drawn at. */
    t->vsb.x = client_w - SCROLLBAR_WIDTH;   /* flush against the right edge */
    t->vsb.y = 0;
    t->vsb.w = SCROLLBAR_WIDTH;
    t->vsb.h = term_rows * TERM_FONT_H;
    scrollbar_set_range(&t->vsb, t->sb_count + term_rows, term_rows);
    scrollbar_set_pos(&t->vsb, t->sb_count - t->view_offset);
    /* The strip is permanently reserved, so always draw the track (it shows
     * a full-height thumb / no thumb when there's nothing to scroll). */
    t->vsb.visible = true;
    scrollbar_paint(&t->vsb);
}

/*==========================================================================
 * Event handler — keyboard input
 *=========================================================================*/

/* Terminal menu command IDs */
#define TCMD_FILE_EXIT    1
#define TCMD_HELP_ABOUT 100

/* Force-close: signal shell, destroy window immediately */
static void terminal_force_close(terminal_t *t, hwnd_t hwnd) {
    /* If fullscreen, restore border/menubar so desktop repaints properly */
    if (wm_is_fullscreen(hwnd)) {
        wm_toggle_fullscreen(hwnd);
    }

    t->closing = true;
    if (t->input_sem) xSemaphoreGive(t->input_sem);
    /* Wake the shell task if it's blocked in ulTaskNotifyTake
     * (waiting for a child ELF app to finish via exec()) */
    if (t->shell_task) xTaskNotifyGive(t->shell_task);
    if (t->blink_timer) {
        xTimerStop(t->blink_timer, 0);
        xTimerDelete(t->blink_timer, 0);
        t->blink_timer = NULL;
    }
    {
        window_t *w = wm_get_window(hwnd);
        if (w) w->user_data = NULL;
    }
    wm_destroy_window(hwnd);
    t->hwnd = HWND_NULL;
}

static bool terminal_event(hwnd_t hwnd, const window_event_t *event) {
    terminal_t *t = terminal_from_hwnd(hwnd);
    if (!t) return false;

    switch (event->type) {
    case WM_CHAR:
        terminal_snap_bottom(t);   /* typing jumps to live output */
        terminal_input_push(t, (uint8_t)event->charev.ch);
        return true;

    case WM_SIZE:
        terminal_resize(t, event->size.w, event->size.h);
        return true;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        /* The terminal's scrollbar is line-measured, but the shared
         * scrollbar_event uses a pixel-sized arrow step.  So handle the
         * arrow buttons here as a 1-line step and let scrollbar_event own
         * the track and thumb-drag. */
        if (event->type == WM_LBUTTONDOWN) {
            int16_t mx = event->mouse.x, my = event->mouse.y;
            if (mx >= t->vsb.x && mx < t->vsb.x + t->vsb.w &&
                my >= t->vsb.y && my < t->vsb.y + t->vsb.h) {
                if (my < t->vsb.y + SCROLLBAR_WIDTH) {
                    terminal_scroll_view(t, 1);   /* up arrow → older */
                    return true;
                }
                if (my >= t->vsb.y + t->vsb.h - SCROLLBAR_WIDTH) {
                    terminal_scroll_view(t, -1);  /* down arrow → newer */
                    return true;
                }
            }
        }
        /* Route to the scrollbar (client-relative coords).  new_pos is the
         * top virtual line; convert back to a view_offset from the bottom. */
        int32_t new_pos;
        if (scrollbar_event(&t->vsb, event, &new_pos)) {
            int off = t->sb_count - (int)new_pos;
            if (off < 0) off = 0;
            if (off > t->sb_count) off = t->sb_count;
            if (off != t->view_offset) {
                t->view_offset = off;
                wm_invalidate(hwnd);
            }
            return true;
        }
        return false;
    }

    case WM_KEYDOWN:
        /* Scrollback view controls (HID usage codes).  These are consumed by
         * the host and never reach the client. */
        switch (event->key.scancode) {
        case 0x4B: terminal_scroll_view(t, t->rows - 1);  return true; /* PgUp */
        case 0x4E: terminal_scroll_view(t, -(t->rows - 1)); return true; /* PgDn */
        case 0x4A: terminal_scroll_view(t, t->sb_count);  return true; /* Home: top */
        case 0x4D: terminal_scroll_view(t, -t->sb_count); return true; /* End: bottom */
        }
        /* Alt+Enter: toggle fullscreen (before Enter→'\n' mapping) */
        if (event->key.scancode == 0x28 && (event->key.modifiers & KMOD_ALT)) {
            wm_toggle_fullscreen(hwnd);
            return true;
        }
        /* F1: about */
        if (event->key.scancode == 0x3A) {
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = TCMD_HELP_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }
        switch (event->key.scancode) {
        case 0x28: terminal_snap_bottom(t); terminal_input_push(t, '\n');  return true;
        case 0x29: terminal_snap_bottom(t); terminal_input_push(t, 0x1B);  return true;
        case 0x2A: terminal_snap_bottom(t); terminal_input_push(t, '\b');  return true;
        case 0x2B: terminal_snap_bottom(t); terminal_input_push(t, '\t');  return true;
        }
        return false;

    case WM_COMMAND:
        switch (event->command.id) {
        case TCMD_FILE_EXIT:
            terminal_force_close(t, hwnd);
            return true;
        case TCMD_HELP_ABOUT:
            dialog_show(hwnd, L(STR_ABOUT_TERMINAL),
                        "Terminal\n\nFRANK OS v" FRANK_VERSION_STR
                        "\n(c) 2026 Mikhail Matveev\n"
                        "<xtreme@rh1.tech>\n"
                        "github.com/rh1tech/frank-os",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;

    case WM_SETFOCUS:
        /* Rebuild menu so language changes take effect immediately */
        terminal_setup_menu(t);
        return false;  /* let default handling continue */

    case WM_CLOSE:
        terminal_force_close(t, hwnd);
        return true;

    default:
        return false;
    }
}

/*==========================================================================
 * Menu setup — called on create and language change
 *=========================================================================*/

void terminal_setup_menu(terminal_t *t) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 2;

    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, L(STR_FILE), sizeof(file->title) - 1);
    file->accel_key = 0x09;
    file->item_count = 1;
    strncpy(file->items[0].text, L(STR_FM_EXIT), sizeof(file->items[0].text) - 1);
    file->items[0].command_id = TCMD_FILE_EXIT;

    menu_def_t *help = &bar.menus[1];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B;
    help->item_count = 1;
    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = TCMD_HELP_ABOUT;
    help->items[0].accel_key = 0x3A;

    menu_set(t->hwnd, &bar);
}

/*==========================================================================
 * Cursor blink timer callback
 *=========================================================================*/

static void blink_callback(TimerHandle_t xTimer) {
    terminal_t *t = (terminal_t *)pvTimerGetTimerID(xTimer);
    if (!t) return;
    t->cursor_visible = !t->cursor_visible;
    wm_invalidate(t->hwnd);
}

/*==========================================================================
 * Stdin waiter notification (per-terminal)
 *=========================================================================*/

void terminal_notify_stdin_ready(terminal_t *t) {
    if (!t) return;
    for (int i = 0; i < t->mos2_num_stdin_waiters; i++) {
        if (t->mos2_stdin_waiters[i]) {
            xTaskNotifyGive(t->mos2_stdin_waiters[i]);
        }
    }
}

/*==========================================================================
 * Public API
 *=========================================================================*/

hwnd_t terminal_create(void) {
    terminal_t *t = (terminal_t *)pvPortMalloc(sizeof(terminal_t));
    if (!t) return HWND_NULL;
    memset(t, 0, sizeof(*t));

    t->fg_color = COLOR_WHITE;
    t->bg_color = COLOR_BLACK;
    t->cursor_visible = true;
    t->cols = TERM_COLS;
    t->rows = TERM_ROWS;

    /* Allocate text-mode buffer.
     * TODO: PSRAM textbufs cause QMI bus hangs when ISRs fire during
     * uncached PSRAM access (write buffer drain + flash fetch contention).
     * Force SRAM until the QMI interleaving issue is resolved. */
    t->textbuf_size = TERM_COLS * TERM_ROWS * 2;
#if 0  /* PSRAM disabled — causes bus hang, see above */
    if (psram_is_available())
        t->textbuf = (uint8_t *)psram_alloc(t->textbuf_size);
#endif
    if (!t->textbuf)
        t->textbuf = (uint8_t *)pvPortMalloc(t->textbuf_size);
    if (!t->textbuf) {
        vPortFree(t);
        return HWND_NULL;
    }
    /* Fill with spaces, white-on-black */
    uint8_t attr = TB_PACK(COLOR_WHITE, COLOR_BLACK);
    for (int i = 0; i < t->cols * t->rows; i++) {
        t->textbuf[i * 2]     = ' ';
        t->textbuf[i * 2 + 1] = attr;
    }

    /* Allocate the scrollback slot ring (small — ~4 KB; line cell buffers are
     * malloc'd on demand as lines scroll off).  Terminal still works if this
     * fails; scrollback is just disabled. */
    t->sb_lines = SCROLLBACK_LINES;
    t->sb_count = 0;
    t->sb_head = 0;
    t->sb_bytes = 0;
    t->view_offset = 0;
    t->sb_slot = (sb_line_t *)pvPortCalloc(t->sb_lines, sizeof(sb_line_t));
    scrollbar_init(&t->vsb, false);

    /* Create input semaphore */
    t->input_sem = xSemaphoreCreateCounting(64, 0);

    /* Forward-declared below — sets up the menu bar with L() strings */
    extern void terminal_setup_menu(terminal_t *t);

    /* Compute outer window size:
     * client = 560 text + 16 scrollbar = 576 wide, 320 tall
     * (70 cols * 8px + SCROLLBAR_WIDTH, 20 rows * 16px)
     * + title bar + menu bar + borders */
    int16_t client_w = TERM_COLS * TERM_FONT_W + SCROLLBAR_WIDTH;  /* 576 */
    int16_t client_h = TERM_ROWS * TERM_FONT_H;  /* 320 */
    int16_t outer_w = client_w + 2 * THEME_BORDER_WIDTH;
    int16_t outer_h = client_h + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT +
                      2 * THEME_BORDER_WIDTH;

    t->hwnd = wm_create_window(
        10, 10, outer_w, outer_h,
        L(STR_TERMINAL),
        WF_CLOSABLE | WF_MOVABLE | WF_RESIZABLE | WF_BORDER | WF_MENUBAR | WF_FULLSCREENABLE,
        terminal_event,
        terminal_paint
    );

    if (t->hwnd == HWND_NULL) {
        vSemaphoreDelete(t->input_sem);
        vPortFree(t->textbuf);
        vPortFree(t);
        return HWND_NULL;
    }

    /* Set black background and store terminal pointer in user_data */
    window_t *win = wm_get_window(t->hwnd);
    if (win) {
        win->bg_color = COLOR_BLACK;
        win->user_data = t;
    }

    /* Attach menu bar */
    terminal_setup_menu(t);

    /* Start cursor blink timer (500ms) — pass terminal_t* as timer ID */
    t->blink_timer = xTimerCreate("tblink", pdMS_TO_TICKS(500),
                                   pdTRUE, (void *)t, blink_callback);
    xTimerStart(t->blink_timer, 0);

    return t->hwnd;
}

void terminal_destroy(terminal_t *t) {
    if (!t) return;

    /* Stop blink timer */
    if (t->blink_timer) {
        xTimerStop(t->blink_timer, portMAX_DELAY);
        xTimerDelete(t->blink_timer, portMAX_DELAY);
        t->blink_timer = NULL;
    }

    /* Clear user_data before destroying window */
    window_t *win = wm_get_window(t->hwnd);
    if (win) win->user_data = NULL;

    /* Destroy the window */
    if (t->hwnd != HWND_NULL) {
        wm_destroy_window(t->hwnd);
        t->hwnd = HWND_NULL;
    }

    /* Delete input semaphore */
    if (t->input_sem) {
        vSemaphoreDelete(t->input_sem);
        t->input_sem = NULL;
    }

    /* Free text buffer (psram_free handles both PSRAM and SRAM pointers) */
    if (t->textbuf) {
        psram_free(t->textbuf);
        t->textbuf = NULL;
    }

    /* Free scrollback: all line cell buffers, then the slot ring (SRAM) */
    if (t->sb_slot) {
        sb_clear(t);
        vPortFree(t->sb_slot);
        t->sb_slot = NULL;
    }

    /* Free the terminal struct itself */
    vPortFree(t);
}

void __not_in_flash_func(terminal_putc)(terminal_t *t, char c) {
    if (!t || !t->textbuf) return;

    switch (c) {
    case '\n':
        t->cursor_col = 0;
        t->cursor_row++;
        break;
    case '\r':
        t->cursor_col = 0;
        break;
    case '\b':
        if (t->cursor_col > 0) {
            t->cursor_col--;
            TB_CHAR(t, t->cursor_row, t->cursor_col) = ' ';
            TB_ATTR(t, t->cursor_row, t->cursor_col) =
                TB_PACK(t->fg_color, t->bg_color);
        }
        break;
    case '\t':
        t->cursor_col = (t->cursor_col + 8) & ~7;
        if (t->cursor_col >= t->cols) {
            t->cursor_col = 0;
            t->cursor_row++;
        }
        break;
    default:
        if (t->cursor_col >= t->cols) {
            t->cursor_col = 0;
            t->cursor_row++;
        }
        if (t->cursor_row >= t->rows) {
            terminal_scroll_up(t);
            t->cursor_row = t->rows - 1;
        }
        TB_CHAR(t, t->cursor_row, t->cursor_col) = (uint8_t)c;
        TB_ATTR(t, t->cursor_row, t->cursor_col) =
            TB_PACK(t->fg_color, t->bg_color);
        t->cursor_col++;
        break;
    }

    /* Handle scroll after newline/tab */
    if (t->cursor_row >= t->rows) {
        terminal_scroll_up(t);
        t->cursor_row = t->rows - 1;
    }

    wm_invalidate(t->hwnd);
}

void terminal_puts(terminal_t *t, const char *s) {
    if (!t || !s) return;
    while (*s) terminal_putc(t, *s++);
}

void terminal_printf(terminal_t *t, const char *fmt, ...) {
    if (!t) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    terminal_puts(t, buf);
}

void __not_in_flash_func(terminal_clear)(terminal_t *t, uint8_t color) {
    if (!t || !t->textbuf) return;
    t->bg_color = color;
    uint8_t attr = TB_PACK(t->fg_color, color);
    for (int i = 0; i < t->cols * t->rows; i++) {
        t->textbuf[i * 2]     = ' ';
        t->textbuf[i * 2 + 1] = attr;
    }
    t->cursor_col = 0;
    t->cursor_row = 0;
    wm_invalidate(t->hwnd);
}

void terminal_set_cursor(terminal_t *t, int col, int row) {
    if (!t) return;
    if (col >= 0 && col < t->cols) t->cursor_col = col;
    if (row >= 0 && row < t->rows) t->cursor_row = row;
}

void terminal_set_color(terminal_t *t, uint8_t fg, uint8_t bg) {
    if (!t) return;
    t->fg_color = fg & 0x0F;
    t->bg_color = bg & 0x0F;
}

/* Run from SRAM to avoid QMI bus contention between PSRAM data writes
 * (CS1) and flash instruction fetches (CS0) through the shared QMI. */
void __not_in_flash_func(terminal_draw_text)(terminal_t *t, const char *str,
                        int col, int row, uint8_t fg, uint8_t bg) {
    if (!t || !t->textbuf || !str) return;
    if (row < 0 || row >= t->rows) return;
    uint8_t attr = TB_PACK(fg & 0x0F, bg & 0x0F);
    for (int i = 0; str[i] != '\0'; i++) {
        int c = col + i;
        if (c < 0) continue;
        if (c >= t->cols) break;
        TB_CHAR(t, row, c) = (uint8_t)str[i];
        TB_ATTR(t, row, c) = attr;
    }
    wm_invalidate(t->hwnd);
}

int terminal_get_cursor_col(terminal_t *t) {
    return t ? t->cursor_col : 0;
}

int terminal_get_cursor_row(terminal_t *t) {
    return t ? t->cursor_row : 0;
}

int terminal_getch(terminal_t *t) {
    if (!t) return -1;
    xSemaphoreTake(t->input_sem, portMAX_DELAY);
    if (t->closing) return -1;
    uint8_t ch = t->input_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) & 63;
    return ch;
}

int terminal_getch_now(terminal_t *t) {
    if (!t) return -1;
    if (xSemaphoreTake(t->input_sem, 0) != pdTRUE) return -1;
    uint8_t ch = t->input_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) & 63;
    return ch;
}

/*==========================================================================
 * Resize — adjust grid to new client area
 *=========================================================================*/

void terminal_resize(terminal_t *t, int client_w, int client_h) {
    if (!t || !t->textbuf) return;

    /* Reserve the right edge for the scrollbar; the rest is text columns. */
    int new_cols = (client_w - SCROLLBAR_WIDTH) / TERM_FONT_W;
    int new_rows = client_h / TERM_FONT_H;

    /* Clamp to valid range */
    if (new_cols < 10) new_cols = 10;
    if (new_cols > TERM_MAX_COLS) new_cols = TERM_MAX_COLS;
    if (new_rows < 4) new_rows = 4;
    if (new_rows > TERM_MAX_ROWS) new_rows = TERM_MAX_ROWS;

    /* No change? */
    if (new_cols == t->cols && new_rows == t->rows) return;

    int old_cols = t->cols;
    int old_rows = t->rows;

    /* Allocate new textbuf */
    size_t new_size = new_cols * new_rows * 2;
    uint8_t *new_buf = (uint8_t *)pvPortMalloc(new_size);
    if (!new_buf) return;

    /* Fill with spaces using current colors */
    uint8_t attr = TB_PACK(t->fg_color, t->bg_color);
    for (int i = 0; i < new_cols * new_rows; i++) {
        new_buf[i * 2]     = ' ';
        new_buf[i * 2 + 1] = attr;
    }

    /* Copy existing content row-by-row (min of old/new dimensions) */
    int copy_cols = old_cols < new_cols ? old_cols : new_cols;
    int copy_rows = old_rows < new_rows ? old_rows : new_rows;
    uint8_t *old_buf = t->textbuf;
    for (int r = 0; r < copy_rows; r++) {
        volatile uint8_t *src = old_buf + r * old_cols * 2;
        uint8_t *dst = new_buf + r * new_cols * 2;
        for (int i = 0; i < copy_cols * 2; i++)
            dst[i] = src[i];
    }

    /* Swap: update textbuf pointer first (atomic on ARM), then dims */
    t->textbuf = new_buf;
    t->cols = new_cols;
    t->rows = new_rows;
    t->textbuf_size = new_size;

    /* Free old buffer */
    psram_free(old_buf);

    /* Scrollback survives resize: each stored line is variable-length and
     * self-describing (length + cells), and sb_build_shadow pads or clamps it
     * to whatever the current column count is — so history is width-
     * independent and needs no adjustment here. */

    /* Clamp cursor */
    if (t->cursor_col >= new_cols) t->cursor_col = new_cols - 1;
    if (t->cursor_row >= new_rows) t->cursor_row = new_rows - 1;

    /* Full repaint — resize is a structural change that needs the frame
     * background refilled to clear stale pixels outside the new grid. */
    wm_force_full_repaint();

    /* Send SIGWINCH to all processes running on this terminal so apps
     * like mc re-query dimensions and redraw at the new size. */
#define SIGWINCH 28
    if (pids) {
        for (size_t i = 1; i < pids->size; i++) {
            cmd_ctx_t *c = (cmd_ctx_t *)pids->p[i];
            if (c && c->term == t && c->task && c->stage < ZOMBIE) {
                c->sig_pending |= (1U << SIGWINCH);
                xTaskNotifyGive(c->task);
            }
        }
    }
#undef SIGWINCH
}

/*==========================================================================
 * Grid dimension queries
 *=========================================================================*/

int terminal_get_cols(terminal_t *t) { return t ? t->cols : TERM_COLS; }
int terminal_get_rows(terminal_t *t) { return t ? t->rows : TERM_ROWS; }

terminal_t *terminal_get_active(void) {
    /* 1. Try TLS slot for current task */
    terminal_t *t = terminal_get_task_terminal();
    if (t) return t;

    /* 2. Fall back to focused window's terminal */
    hwnd_t focus = wm_get_focus();
    if (focus != HWND_NULL) {
        t = terminal_from_hwnd(focus);
        if (t) return t;
    }

    return NULL;
}

uint8_t *terminal_get_textbuf(terminal_t *t) {
    return t ? t->textbuf : NULL;
}

size_t terminal_get_textbuf_size(terminal_t *t) {
    return t ? t->textbuf_size : 0;
}

void terminal_invalidate_active(void) {
    terminal_t *t = terminal_get_active();
    if (t) wm_invalidate(t->hwnd);
}
