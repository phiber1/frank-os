/*
 * frankos_main.c — Frank OS entry point for MMBasic
 *
 * Creates an 8×16 monospaced terminal window (79×24 cells) and runs
 * the PicoMite MMBasic BASIC interpreter inside it.  Keyboard events
 * are translated to ANSI/VT100 sequences and fed to the interpreter.
 * Text output is rendered directly into the Frank OS framebuffer.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* configSTACK_DEPTH_TYPE is used in m-os-api-tasks.h (FreeRTOS task.h) but
 * not defined by MMBasic's FreeRTOSConfig.h.  Define it before the include. */
#ifndef configSTACK_DEPTH_TYPE
#define configSTACK_DEPTH_TYPE  uint16_t
#endif
#include "m-os-api.h"
#include "frankos-app.h"

#undef switch
#undef inline
#undef __force_inline
#undef abs

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "font.h"     /* font_8x16[4096]: 256 glyphs × 16 rows, 8 px wide */

/* 632x384 4bpp graphics layer maintained by frankos_gfx.c; composited
 * beneath the text cells (blank default-attribute cells show it). */
extern uint8_t *basic_gfx_buf;

/* ── Terminal geometry ──────────────────────────────────────────────────── */
#define BASIC_COLS          79
#define BASIC_ROWS          24
#define BASIC_FW             8   /* glyph width  in pixels */
#define BASIC_FH            16   /* glyph height in pixels */

#define BASIC_CLIENT_W  (BASIC_COLS * BASIC_FW)   /* 632 */
#define BASIC_CLIENT_H  (BASIC_ROWS * BASIC_FH)   /* 384 */

/* Frank OS window outer dimensions (title + border, no menu bar). */
#define BASIC_WIN_W  (BASIC_CLIENT_W + 2 * THEME_BORDER_WIDTH)
#define BASIC_WIN_H  (BASIC_CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH)

/* ── Text cell format ───────────────────────────────────────────────────── */
/* attr byte: high nibble = background color index (0-15 = CGA/Frank OS COLOR_*)
 *            low  nibble = foreground color index */
typedef struct { uint8_t ch; uint8_t attr; } cell_t;

/* Default attribute: fg = COLOR_LIGHT_GRAY (7), bg = COLOR_BLACK (0). */
#define DEFAULT_ATTR  0x07u

/* ── Text buffer (volatile: written by BASIC task, read by paint task).
 * Declared as pointer-to-row so g_textbuf[r][c] syntax still works.
 * Allocated from PSRAM via calloc() at the start of main(). ──────────── */
static volatile cell_t  (*g_textbuf)[BASIC_COLS] = NULL;
static volatile int       g_cur_col  = 0;
static volatile int       g_cur_row  = 0;
static volatile bool      g_cur_vis  = false;  /* cursor blink state */

/* ── Keyboard ring buffer ───────────────────────────────────────────────── */
#define KBUF_SZ  512
static volatile uint8_t   g_kbuf[KBUF_SZ];
static volatile int       g_kbuf_head = 0;
static volatile int       g_kbuf_tail = 0;

/* ── App-wide state ─────────────────────────────────────────────────────── */
static hwnd_t             g_hwnd    = HWND_NULL;
static TaskHandle_t       g_task    = NULL;

/* Display dirty flag: text output marks it, the 25 ms flush timer turns it
 * into a single wm_invalidate.  Per-character invalidation caused a full-
 * repaint storm (printScreen: ~1900 chars × ~0.5 ms = 1 s per redraw). */
volatile bool             g_disp_dirty = false;

/* ── Dirty-region tracking (cell coordinates) ───────────────────────────────
 * Text writers and the graphics layer accumulate a bounding box; the
 * paint renders only that region when the WM reports the frame itself
 * was clean (sys_table[556]).  Full repaints when the window moved/was
 * uncovered, on fullscreen toggles, and on the first paint.  This is
 * what makes sprite animation affordable: a full composite re-reads the
 * whole 121KB gfx layer from uncached PSRAM (~15-30 ms); a sprite frame
 * touches a few cells. */
#define DIRTY_MAX 4
typedef struct { int r0, c0, r1, c1; } dirty_rect_t;
static volatile dirty_rect_t g_dirty[DIRTY_MAX];
static volatile int  g_dirty_n    = 0;
static volatile bool g_force_full = true;

/* Closing flag and MMAbort — also read by frankos_platform.c */
volatile bool             g_closing = false;

/* Focus state: when another window has focus, display flushes are
 * throttled to ~4 Hz.  Repaints of an overlapped window can cascade
 * (the WM frame-dirties higher windows over freshly painted lower
 * ones); at 40 Hz that starved the interpreter to a standstill while
 * e.g. Calculator sat focused on top. */
static volatile bool      g_focused = true;

/* Autorun path — set from argv[1], consumed by basic_run_interpreter() */
char                      g_autorun_path[256];

/* MMAbort is declared in PicoMite.c and wrapped; updated here on Ctrl+C. */
extern volatile int       MMAbort;

/* Platform init + interpreter entry — in frankos_platform.c */
extern void               basic_platform_init(void);
extern void               basic_run_interpreter(void);

/* ═════════════════════════════════════════════════════════════════════════
 * Keyboard ring buffer
 * ═════════════════════════════════════════════════════════════════════════ */

/* Push one byte into ring buffer and wake BASIC task. */
void basic_kbuf_push(int c)
{
    int next = (g_kbuf_head + 1) % KBUF_SZ;
    if (next != g_kbuf_tail) {
        g_kbuf[g_kbuf_head] = (uint8_t)(c & 0xFF);
        g_kbuf_head = next;
    }
    /* Wake the interpreter if it is sleeping in __wrap_getConsole(). */
    if (g_task)
        xTaskNotifyGive(g_task);
}

/* Pop one byte; returns -1 if empty. Called by __wrap_getConsole(). */
int basic_kbuf_pop(void)
{
    if (g_kbuf_tail == g_kbuf_head)
        return -1;
    int c = g_kbuf[g_kbuf_tail];
    g_kbuf_tail = (g_kbuf_tail + 1) % KBUF_SZ;
    return c;
}

/* Number of bytes waiting in the ring buffer. */
int basic_kbuf_avail(void)
{
    int n = g_kbuf_head - g_kbuf_tail;
    if (n < 0) n += KBUF_SZ;
    return n;
}

/* Yield to FreeRTOS for 1 tick.  Called from PicoMite.c getConsole()
 * when the keyboard buffer is empty, so the BASIC task does not
 * busy-loop and starve other tasks (WM, cursor blink, etc.). */
void basic_yield(void)
{
    vTaskDelay(1);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Text buffer helpers
 * ═════════════════════════════════════════════════════════════════════════ */

/* Merge-or-add into the rect list.  Disjoint dirty areas stay separate
 * rects — a single union rect made the paint cost scale with the SPAN
 * between two sprites, which grew as their phases drifted (the
 * "smoothness decays over minutes, re-RUN fixes it" symptom). */
static void mark_cells(int r0, int c0, int r1, int c1)
{
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= BASIC_ROWS) r1 = BASIC_ROWS - 1;
    if (c1 >= BASIC_COLS) c1 = BASIC_COLS - 1;
    if (r0 > r1 || c0 > c1) return;
    int n = g_dirty_n;
    /* merge into an overlapping/adjacent rect */
    for (int i = 0; i < n; i++) {
        if (r0 <= g_dirty[i].r1 + 1 && r1 >= g_dirty[i].r0 - 1 &&
            c0 <= g_dirty[i].c1 + 1 && c1 >= g_dirty[i].c0 - 1) {
            if (r0 < g_dirty[i].r0) g_dirty[i].r0 = r0;
            if (c0 < g_dirty[i].c0) g_dirty[i].c0 = c0;
            if (r1 > g_dirty[i].r1) g_dirty[i].r1 = r1;
            if (c1 > g_dirty[i].c1) g_dirty[i].c1 = c1;
            g_disp_dirty = true;
            return;
        }
    }
    if (n < DIRTY_MAX) {
        g_dirty[n].r0 = r0; g_dirty[n].c0 = c0;
        g_dirty[n].r1 = r1; g_dirty[n].c1 = c1;
        g_dirty_n = n + 1;
    } else {
        /* overflow: fold into slot 0 */
        if (r0 < g_dirty[0].r0) g_dirty[0].r0 = r0;
        if (c0 < g_dirty[0].c0) g_dirty[0].c0 = c0;
        if (r1 > g_dirty[0].r1) g_dirty[0].r1 = r1;
        if (c1 > g_dirty[0].c1) g_dirty[0].c1 = c1;
    }
    g_disp_dirty = true;
}

/* Pixel-space marking for the graphics layer (called by frankos_gfx.c
 * primitives with each op's bounding rect). */
void basic_gfx_mark(int x1, int y1, int x2, int y2)
{
    mark_cells(y1 / BASIC_FH, x1 / BASIC_FW, y2 / BASIC_FH, x2 / BASIC_FW);
}

static void tbuf_clear(void)
{
    for (int r = 0; r < BASIC_ROWS; r++)
        for (int c = 0; c < BASIC_COLS; c++) {
            g_textbuf[r][c].ch   = ' ';
            g_textbuf[r][c].attr = DEFAULT_ATTR;
        }
    g_cur_col = 0;
    g_cur_row = 0;
    mark_cells(0, 0, BASIC_ROWS - 1, BASIC_COLS - 1);
}

/* Non-static wrapper so frankos_platform.c's ClearScreen() can call it
 * without needing to know the cell_t type or g_textbuf's pointer type. */
void basic_tbuf_clear(void) { tbuf_clear(); }

static void tbuf_scroll_up(void)
{
    for (int r = 0; r < BASIC_ROWS - 1; r++)
        for (int c = 0; c < BASIC_COLS; c++)
            g_textbuf[r][c] = g_textbuf[r + 1][c];
    for (int c = 0; c < BASIC_COLS; c++) {
        g_textbuf[BASIC_ROWS - 1][c].ch   = ' ';
        g_textbuf[BASIC_ROWS - 1][c].attr = DEFAULT_ATTR;
    }
    mark_cells(0, 0, BASIC_ROWS - 1, BASIC_COLS - 1);
}

/*
 * basic_textbuf_putc — emit one character into the terminal buffer.
 * Called from frankos_platform.c's DisplayPutC() stub.
 *
 * Implements a VT100/ANSI subset because the interpreter drives the
 * display through the console character stream: the full-screen editor
 * positions with ESC[r;cH, clears with ESC[J / ESC[K / ESC[2J, renders
 * its status line with ESC[7m inverse, and hides/shows the cursor with
 * ESC[?25l / ESC[?25h.  (Input already spoke VT — arrows and F-keys are
 * pushed as escape sequences — output finally does too.)
 */

/* Cell-range fill for the platform layer's DrawBox/DrawRectangle. */
void basic_tbuf_fill_cells(int r0, int c0, int r1, int c1, uint8_t attr)
{
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= BASIC_ROWS) r1 = BASIC_ROWS - 1;
    if (c1 >= BASIC_COLS) c1 = BASIC_COLS - 1;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            g_textbuf[r][c].ch   = ' ';
            g_textbuf[r][c].attr = attr;
        }
    mark_cells(r0, c0, r1, c1);
}

/* PicoMite display-layer state: the editor (and any display-console
 * code) positions output via MX470Cursor -> CurrentX/CurrentY (pixel
 * coords) and colours via gui_fcolour/gui_bcolour.  These globals are
 * the single source of truth for the text cursor; g_cur_row/col are
 * derived from them after every character. */
extern short CurrentX, CurrentY;
extern int   gui_fcolour, gui_bcolour;

/* Map a 24-bit RGB colour to the nearest CGA/Frank OS palette index. */
uint8_t basic_rgb2cga(int rgb)
{
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int hi = (r > 170 || g > 170 || b > 170);
    int rb = r > 85, gb = g > 85, bb = b > 85;
    /* CGA: bit0=blue, bit1=green, bit2=red, bit3=bright */
    uint8_t idx = (uint8_t)((rb ? 4 : 0) | (gb ? 2 : 0) | (bb ? 1 : 0));
    if (hi && (r > 213 || g > 213 || b > 213) && idx != 7) idx |= 8;
    if (r > 213 && g > 213 && b > 213) idx = 15;
    else if (r > 85 && g > 85 && b > 85 && r <= 213) idx = 7;
    return idx;
}

static uint8_t disp_attr(void)
{
    return (uint8_t)((basic_rgb2cga(gui_bcolour) << 4) |
                     basic_rgb2cga(gui_fcolour));
}

/* current SGR state */
static uint8_t g_vt_attr    = DEFAULT_ATTR;
static bool    g_vt_inverse = false;
volatile bool  g_vt_cursor_on = true;      /* ESC[?25h/l + ShowCursor() */

/* escape parser state */
static enum { VT_GROUND, VT_ESC, VT_CSI } g_vt_state = VT_GROUND;
static int  g_vt_par[8];
static int  g_vt_np;
static bool g_vt_priv;

/* ANSI colour index (0-7) -> CGA palette index */
static const uint8_t vt_ansi2cga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint8_t vt_cur_attr(void) {
    return g_vt_inverse
        ? (uint8_t)((g_vt_attr >> 4) | (g_vt_attr << 4))
        : g_vt_attr;
}

static void vt_put_cell(int r, int c, uint8_t ch, uint8_t attr) {
    g_textbuf[r][c].ch   = ch;
    g_textbuf[r][c].attr = attr;
    mark_cells(r, c, r, c);
}

static void vt_clear_range(int r0, int c0, int r1, int c1) {
    /* inclusive range in reading order, cleared with the current attr */
    uint8_t a = vt_cur_attr();
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : BASIC_COLS - 1;
        for (int c = cs; c <= ce; c++)
            vt_put_cell(r, c, ' ', a);
    }
}

static void vt_sgr(void) {
    if (g_vt_np == 0) { g_vt_par[0] = 0; g_vt_np = 1; }
    for (int i = 0; i < g_vt_np; i++) {
        int p = g_vt_par[i];
        if      (p == 0)  { g_vt_attr = DEFAULT_ATTR; g_vt_inverse = false; }
        else if (p == 7)  g_vt_inverse = true;
        else if (p == 27) g_vt_inverse = false;
        else if (p >= 30 && p <= 37)
            g_vt_attr = (uint8_t)((g_vt_attr & 0xF0) | vt_ansi2cga[p - 30]);
        else if (p >= 90 && p <= 97)
            g_vt_attr = (uint8_t)((g_vt_attr & 0xF0) | (vt_ansi2cga[p - 90] + 8));
        else if (p == 39)
            g_vt_attr = (uint8_t)((g_vt_attr & 0xF0) | (DEFAULT_ATTR & 0x0F));
        else if (p >= 40 && p <= 47)
            g_vt_attr = (uint8_t)((g_vt_attr & 0x0F) | (vt_ansi2cga[p - 40] << 4));
        else if (p >= 100 && p <= 107)
            g_vt_attr = (uint8_t)((g_vt_attr & 0x0F) | ((vt_ansi2cga[p - 100] + 8) << 4));
        else if (p == 49)
            g_vt_attr = (uint8_t)(g_vt_attr & 0x0F);
        /* 1 bold, 4 underline, etc: ignored */
    }
}

static void vt_sync_pixel_cursor(void) {
    CurrentX = (short)(g_cur_col * BASIC_FW);
    CurrentY = (short)(g_cur_row * BASIC_FH);
}

static void vt_csi_final(char fin) {
    int p0 = g_vt_np > 0 ? g_vt_par[0] : 0;
    int p1 = g_vt_np > 1 ? g_vt_par[1] : 0;

    /* Derive the logical cursor from the display layer's pixel cursor —
     * MX470Cursor() may have moved it since the last character. */
    int cx = CurrentX / BASIC_FW, cy = CurrentY / BASIC_FH;
    if (cx >= 0 && cx < BASIC_COLS) g_cur_col = cx;
    if (cy >= 0 && cy < BASIC_ROWS) g_cur_row = cy;

    switch (fin) {
    case 'H': case 'f':
        g_cur_row = (p0 ? p0 : 1) - 1;
        g_cur_col = (p1 ? p1 : 1) - 1;
        if (g_cur_row < 0) g_cur_row = 0;
        if (g_cur_row >= BASIC_ROWS) g_cur_row = BASIC_ROWS - 1;
        if (g_cur_col < 0) g_cur_col = 0;
        if (g_cur_col >= BASIC_COLS) g_cur_col = BASIC_COLS - 1;
        break;
    case 'A': g_cur_row -= p0 ? p0 : 1; if (g_cur_row < 0) g_cur_row = 0; break;
    case 'B': g_cur_row += p0 ? p0 : 1;
              if (g_cur_row >= BASIC_ROWS) g_cur_row = BASIC_ROWS - 1; break;
    case 'C': g_cur_col += p0 ? p0 : 1;
              if (g_cur_col >= BASIC_COLS) g_cur_col = BASIC_COLS - 1; break;
    case 'D': g_cur_col -= p0 ? p0 : 1; if (g_cur_col < 0) g_cur_col = 0; break;
    case 'J':
        if (p0 == 2)
            vt_clear_range(0, 0, BASIC_ROWS - 1, BASIC_COLS - 1);
        else if (p0 == 1)
            vt_clear_range(0, 0, g_cur_row, g_cur_col);
        else
            vt_clear_range(g_cur_row, g_cur_col,
                           BASIC_ROWS - 1, BASIC_COLS - 1);
        break;
    case 'K':
        if (p0 == 2)
            vt_clear_range(g_cur_row, 0, g_cur_row, BASIC_COLS - 1);
        else if (p0 == 1)
            vt_clear_range(g_cur_row, 0, g_cur_row, g_cur_col);
        else
            vt_clear_range(g_cur_row, g_cur_col, g_cur_row, BASIC_COLS - 1);
        break;
    case 'm':
        vt_sgr();
        break;
    case 'h':
        if (g_vt_priv && p0 == 25) g_vt_cursor_on = true;
        break;
    case 'l':
        if (g_vt_priv && p0 == 25) g_vt_cursor_on = false;
        break;
    default:
        break;   /* 't' resize hints and anything else: ignored */
    }
    /* Only cursor-motion finals may write CurrentX/CurrentY back:
     * for SGR/erase, g_cur_row/col may be stale relative to a cursor
     * that MX470Cursor() moved without emitting characters. */
    switch (fin) {
    case 'H': case 'f': case 'A': case 'B': case 'C': case 'D':
        vt_sync_pixel_cursor();
        break;
    default:
        break;
    }
}

void basic_textbuf_putc(int c)
{
    /* ── escape sequence parsing ── */
    if (g_vt_state == VT_ESC) {
        if (c == '[') {
            g_vt_state = VT_CSI;
            g_vt_np = 0;
            g_vt_priv = false;
            for (int i = 0; i < 8; i++) g_vt_par[i] = 0;
        } else {
            g_vt_state = VT_GROUND;   /* unsupported ESC x: swallow */
        }
        return;
    }
    if (g_vt_state == VT_CSI) {
        if (c >= '0' && c <= '9') {
            if (g_vt_np == 0) g_vt_np = 1;
            if (g_vt_np <= 8)
                g_vt_par[g_vt_np - 1] = g_vt_par[g_vt_np - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (g_vt_np == 0) g_vt_np = 1;
            if (g_vt_np < 8) g_vt_np++;
        } else if (c == '?') {
            g_vt_priv = true;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt_csi_final((char)c);
            g_vt_state = VT_GROUND;
            goto done;
        }
        /* other intermediates ignored */
        return;
    }
    if (c == 0x1b) {
        g_vt_state = VT_ESC;
        return;
    }

    /* ── ground state: position comes from CurrentX/CurrentY, which
     * MX470Cursor() (the editor / display layer) sets in pixels.
     * Wrap is DEFERRED: writing the last column leaves CurrentX equal to
     * the client width; the wrap (and any scroll) happens only when the
     * next printable character arrives.  Eager wrap scrolled the screen
     * whenever a string ended exactly in the last column — which the
     * editor's right-aligned status line does on every redraw. ── */
    if (CurrentX < 0) CurrentX = 0;
    if (CurrentY < 0) CurrentY = 0;
    if (CurrentX > BASIC_CLIENT_W)
        CurrentX = BASIC_CLIENT_W;   /* == client width means pending wrap */
    if (CurrentY > (BASIC_ROWS - 1) * BASIC_FH)
        CurrentY = (BASIC_ROWS - 1) * BASIC_FH;

    if (c == '\r') {
        CurrentX = 0;
        g_cur_col = 0;
        goto done;
    }
    if (c == '\n') {
        CurrentX = 0;
        CurrentY += BASIC_FH;
        if (CurrentY + BASIC_FH > BASIC_CLIENT_H) {
            tbuf_scroll_up();
            CurrentY = BASIC_CLIENT_H - BASIC_FH;
        }
        g_cur_col = 0;
        g_cur_row = CurrentY / BASIC_FH;
        goto done;
    }
    if (c == '\b') {
        if (CurrentX >= BASIC_FW)
            CurrentX -= BASIC_FW;
        else if (CurrentY >= BASIC_FH) {
            CurrentY -= BASIC_FH;
            CurrentX = (short)((BASIC_COLS - 1) * BASIC_FW);
        }
        g_cur_col = CurrentX / BASIC_FW;
        g_cur_row = CurrentY / BASIC_FH;
        vt_put_cell(g_cur_row, g_cur_col, ' ', disp_attr());
        goto done;
    }
    if (c < 0x20)
        goto done;   /* ignore other control chars */

    /* pending wrap from the previous character? do it now */
    if (CurrentX + BASIC_FW > BASIC_CLIENT_W) {
        CurrentX = 0;
        CurrentY += BASIC_FH;
    }
    if (CurrentY + BASIC_FH > BASIC_CLIENT_H) {
        tbuf_scroll_up();
        CurrentY = BASIC_CLIENT_H - BASIC_FH;
    }
    g_cur_col = CurrentX / BASIC_FW;
    g_cur_row = CurrentY / BASIC_FH;

    vt_put_cell(g_cur_row, g_cur_col, (uint8_t)c,
                g_vt_inverse ? (uint8_t)((disp_attr() >> 4) | (disp_attr() << 4))
                             : disp_attr());
    CurrentX += BASIC_FW;

done:
    g_disp_dirty = true;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Paint callback — runs on the WM / compositor task
 * ═════════════════════════════════════════════════════════════════════════ */

/*
 * Render the text buffer into the Frank OS window framebuffer.  Windows
 * paint directly into the shared framebuffer (no backing store), so a
 * WM_PAINT can mean "another window was just dragged across you" — every
 * paint must redraw all visible cells.  A full 79x24 render is ~1.6 ms;
 * the previous dirty-cell shadow optimization left white holes after
 * occlusion.
 *
 * Framebuffer is nibble-packed 4bpp: high nibble = left (even) pixel,
 *                                     low  nibble = right (odd) pixel.
 * font_8x16[ch*16 + row] has LSB = leftmost pixel.
 */
static void basic_paint(hwnd_t hwnd)
{
    wd_begin(hwnd);

    /* Query the WM's frame-dirty verdict FIRST: if this dispatch follows
     * a frame fill (focus change, move, uncover) the client was just
     * painted over with the theme background — losing that fact across
     * a hold-skip left the window white with only sprite rects redrawn. */
    bool frame_dirty;
    {
        typedef uint32_t (*fdq_t)(void);
        fdq_t q = (fdq_t)_sys_table_ptrs[556];
        frame_dirty = (!q || q());
    }

    /* Sprite compound (hide-restore + redraw) in flight: painting now
     * would show the sprite half-erased.  Skip and retry on the next
     * 25 ms flush tick — capped so a stuck hold degrades to flicker
     * rather than a frozen display. */
    {
        extern volatile int basic_gfx_hold;
        extern volatile bool g_gfx_paint_skipped;
        static int held_paints;
        if (basic_gfx_hold > 0 && !frame_dirty && held_paints < 8) {
            held_paints++;
            g_disp_dirty = true;
            g_gfx_paint_skipped = true;   /* compound exit repays: sync */
            wd_end();
            return;
        }
        if (basic_gfx_hold <= 0)
            held_paints = 0;
    }

    bool fs = wm_is_fullscreen(hwnd);

    /* In fullscreen (640×480), the 79×24 text grid (632×384) doesn't
     * fill the screen.  Centre it and fill the margins with black. */
    int16_t x_off = 0, y_off = 0;
    if (fs) {
        x_off = (DISPLAY_WIDTH  - BASIC_CLIENT_W) / 2;  /* 4 */
        y_off = (DISPLAY_HEIGHT - BASIC_CLIENT_H) / 2;  /* 48 */
    }

    int16_t stride;
    uint8_t *dst = wd_fb_ptr(x_off, y_off, &stride);
    if (!dst) {
        wd_end();
        return;
    }

    /* Fill black margins in fullscreen on first paint / mode switch */
    static bool prev_fs = false;
    if (fs != prev_fs)
        g_force_full = true;
    if (fs && !prev_fs) {
        /* Top bar */
        wd_fill_rect(0, 0, DISPLAY_WIDTH, y_off, COLOR_BLACK);
        /* Bottom bar */
        wd_fill_rect(0, y_off + BASIC_CLIENT_H, DISPLAY_WIDTH,
                     DISPLAY_HEIGHT - y_off - BASIC_CLIENT_H, COLOR_BLACK);
        /* Left strip */
        wd_fill_rect(0, y_off, x_off, BASIC_CLIENT_H, COLOR_BLACK);
        /* Right strip */
        wd_fill_rect(x_off + BASIC_CLIENT_W, y_off,
                     DISPLAY_WIDTH - x_off - BASIC_CLIENT_W,
                     BASIC_CLIENT_H, COLOR_BLACK);
    }
    prev_fs = fs;

    /* ── Compute visible columns / rows ─────────────────────────────
     * When the window extends past the screen edge, writing the full
     * 79×24 grid would overflow the framebuffer row (stride=320 bytes).
     * Probe wd_fb_ptr at the last pixel of each column/row to find
     * how many complete cells fit.  The checks are monotonic, so we
     * break on the first failure.
     * ─────────────────────────────────────────────────────────────── */
    int vis_cols = BASIC_COLS;
    int vis_rows = BASIC_ROWS;
    if (!fs) {
        int16_t dummy;
        /* Horizontal clipping */
        if (!wd_fb_ptr(x_off + BASIC_CLIENT_W - 1, y_off, &dummy)) {
            vis_cols = 0;
            for (int c = 0; c < BASIC_COLS; c++) {
                if (wd_fb_ptr(x_off + (c + 1) * BASIC_FW - 1, y_off, &dummy))
                    vis_cols = c + 1;
                else
                    break;
            }
        }
        /* Vertical clipping */
        if (!wd_fb_ptr(x_off, y_off + BASIC_CLIENT_H - 1, &dummy)) {
            vis_rows = 0;
            for (int r = 0; r < BASIC_ROWS; r++) {
                if (wd_fb_ptr(x_off, y_off + (r + 1) * BASIC_FH - 1, &dummy))
                    vis_rows = r + 1;
                else
                    break;
            }
        }
    }

    if (vis_cols == 0 || vis_rows == 0) {
        wd_end();
        return;
    }

    /* Cursor position comes straight from the display layer's pixel
     * cursor (CurrentX/CurrentY): the editor moves it via MX470Cursor()
     * without emitting characters, so g_cur_row/col can be stale. */
    int cur_row = CurrentY / BASIC_FH;
    int cur_col = CurrentX / BASIC_FW;
    if (cur_row < 0) cur_row = 0;
    if (cur_row >= BASIC_ROWS) cur_row = BASIC_ROWS - 1;
    if (cur_col < 0) cur_col = 0;
    if (cur_col >= BASIC_COLS) cur_col = BASIC_COLS - 1;

    /* ── Full vs partial repaint ────────────────────────────────────────
     * Full when the WM reports the frame was dirty (moved/uncovered —
     * framebuffer content unreliable; sys_table[556], absent on older
     * OS builds), on fullscreen toggles, and on the first paint.
     * Otherwise render only the accumulated dirty cell region (plus the
     * cursor cell so typing echoes instantly). */
    bool full = g_force_full || frame_dirty;
    /* Snapshot the dirty rect list (plus the cursor cell as its own
     * rect); full repaints use a single all-covering rect. */
    dirty_rect_t rects[DIRTY_MAX + 1];
    int nrect = 0;
    if (full) {
        rects[0].r0 = 0; rects[0].c0 = 0;
        rects[0].r1 = vis_rows - 1; rects[0].c1 = vis_cols - 1;
        nrect = 1;
    } else {
        int n = g_dirty_n;
        if (n > DIRTY_MAX) n = DIRTY_MAX;
        for (int i = 0; i < n; i++) {
            rects[nrect].r0 = g_dirty[i].r0; rects[nrect].c0 = g_dirty[i].c0;
            rects[nrect].r1 = g_dirty[i].r1; rects[nrect].c1 = g_dirty[i].c1;
            nrect++;
        }
        rects[nrect].r0 = rects[nrect].r1 = cur_row;
        rects[nrect].c0 = rects[nrect].c1 = cur_col;
        nrect++;
        for (int i = 0; i < nrect; i++) {
            if (rects[i].r0 < 0) rects[i].r0 = 0;
            if (rects[i].c0 < 0) rects[i].c0 = 0;
            if (rects[i].r1 >= vis_rows) rects[i].r1 = vis_rows - 1;
            if (rects[i].c1 >= vis_cols) rects[i].c1 = vis_cols - 1;
        }
    }
    g_dirty_n    = 0;
    g_force_full = false;

    for (int ri = 0; ri < nrect; ri++) {
    int pr0 = rects[ri].r0, pc0 = rects[ri].c0;
    int pr1 = rects[ri].r1, pc1 = rects[ri].c1;
    if (pr0 > pr1 || pc0 > pc1) continue;

    /* Row staging: when a paint covers a wide span, bulk-copy the gfx
     * layer's rows for this text row into SRAM once, instead of one tiny
     * uncached-PSRAM memcpy per blank cell per glyph row (~30k for a
     * full composite).  Roughly halves full-repaint cost. */
    static uint8_t gfx_rowblk[BASIC_FH * (BASIC_CLIENT_W / 2)];

    for (int row = pr0; row <= pr1; row++) {
        bool staged = false;
        int  spanb0 = pc0 * (BASIC_FW / 2);
        int  spanbn = (pc1 - pc0 + 1) * (BASIC_FW / 2);
        if (basic_gfx_buf && (pc1 - pc0) > 16) {
            for (int gy = 0; gy < BASIC_FH; gy++)
                memcpy(gfx_rowblk + gy * (BASIC_CLIENT_W / 2) + spanb0,
                       basic_gfx_buf
                           + (row * BASIC_FH + gy) * (BASIC_CLIENT_W / 2)
                           + spanb0,
                       (size_t)spanbn);
            staged = true;
        }
        for (int col = pc0; col <= pc1; col++) {
            uint8_t ch   = g_textbuf[row][col].ch;
            uint8_t attr = g_textbuf[row][col].attr;

            /* Cursor: invert fg/bg on current cell when visible.  If the
             * cell's fg==bg (region cleared by DrawBox), inversion would
             * be invisible — fall back to a gray block. */
            bool cursor = (row == cur_row && col == cur_col &&
                           g_cur_vis && g_vt_cursor_on);

            /* Graphics layer: blank default-attribute cells are
             * transparent — their 8x16 block comes from the BASIC
             * graphics buffer.  Both source (col*8/2) and destination
             * are byte-aligned (x_off is 0 or 4), so each glyph row is
             * a straight 4-byte copy. */
            if (!cursor && ch == ' ' && attr == DEFAULT_ATTR &&
                basic_gfx_buf) {
                const uint8_t *src = staged
                    ? gfx_rowblk + (col * BASIC_FW) / 2
                    : basic_gfx_buf
                                   + (row * BASIC_FH) * (BASIC_CLIENT_W / 2)
                                   + (col * BASIC_FW) / 2;
                uint8_t *drow = dst
                              + (row * BASIC_FH) * stride
                              + (col * BASIC_FW) / 2;
                for (int gy = 0; gy < BASIC_FH; gy++) {
                    memcpy(drow, src, BASIC_FW / 2);
                    src  += BASIC_CLIENT_W / 2;
                    drow += stride;
                }
                continue;
            }

            uint8_t eff_attr = attr;
            if (cursor) {
                eff_attr = (uint8_t)((attr >> 4) | (attr << 4));
                if ((eff_attr & 0x0F) == ((eff_attr >> 4) & 0x0F))
                    eff_attr = 0x70;   /* gray bg, black fg */
            }

            uint8_t fg = eff_attr & 0x0Fu;
            uint8_t bg = (eff_attr >> 4) & 0x0Fu;
            const uint8_t *glyph = &font_8x16[(uint8_t)ch * BASIC_FH];

            for (int gy = 0; gy < BASIC_FH; gy++) {
                uint8_t bits = glyph[gy];
                uint8_t *drow = dst
                              + (row * BASIC_FH + gy) * stride
                              + (col * BASIC_FW) / 2;

                for (int bx = 0; bx < BASIC_FW / 2; bx++) {
                    uint8_t left  = (bits & 0x01) ? fg : bg;
                    uint8_t right = (bits & 0x02) ? fg : bg;
                    *drow++ = (left << 4) | right;
                    bits >>= 2;
                }
            }
        }
    }

    }   /* rect loop */

    {   /* completed paint — release any waiting basic_gfx_sync() */
        extern volatile uint32_t g_paint_serial;
        g_paint_serial++;
    }
    wd_end();
}

/* ═════════════════════════════════════════════════════════════════════════
 * Window event callback
 * ═════════════════════════════════════════════════════════════════════════ */

/* ── Sprite frame sync ──────────────────────────────────────────────────────
 * When basic_paint() had to skip because a sprite compound was in flight,
 * the compound's exit calls basic_gfx_sync(): paint the window NOW (in a
 * guaranteed-complete state) before the interpreter starts the next
 * compound.  Free-running sprite loops self-throttle to the paint rate
 * instead of starving the compositor (which produced cap-forced paints
 * mid-compound = flicker). */
volatile bool g_gfx_paint_skipped = false;

/* Bumped at the end of every completed (non-skipped) basic_paint.  The
 * sync waits on it: a blind one-tick delay was not enough — the paint it
 * requested often landed on the NEXT sprite compound, forcing the next
 * sync too (every iteration paid the yield: "PAUSE 2 feels like 4"). */
volatile uint32_t g_paint_serial;

void basic_gfx_sync(void)
{
    extern volatile int basic_gfx_hold;
    if (basic_gfx_hold)                 /* nested compound still open */
        return;
    if (g_gfx_paint_skipped && g_hwnd != HWND_NULL) {
        uint32_t s0 = g_paint_serial;
        g_gfx_paint_skipped = false;
        g_disp_dirty = false;           /* invalidating directly */
        wm_invalidate(g_hwnd);
        /* Wait until the paint actually COMPLETES so it cannot collide
         * with the caller's next compound.  Bounded: ~3 ticks max. */
        for (int i = 0; i < 3 && g_paint_serial == s0; i++)
            vTaskDelay(1);
    }
}

/* Push a NUL-terminated string into the keyboard ring buffer. */
static void push_vt(const char *s)
{
    while (*s)
        basic_kbuf_push((unsigned char)*s++);
}

static bool basic_event(hwnd_t hwnd, const window_event_t *event)
{
    (void)hwnd;

    if (event->type == WM_CLOSE) {
        if (wm_is_fullscreen(hwnd))
            wm_toggle_fullscreen(hwnd);
        g_closing = true;
        MMAbort   = 1;
        if (g_task)
            xTaskNotifyGive(g_task);
        return true;
    }

    if (event->type == WM_SETFOCUS)  { g_focused = true;  return true; }
    if (event->type == WM_KILLFOCUS) { g_focused = false; return true; }

    /* WM_CHAR: printable characters and control chars (Enter, BS, Tab, Esc…) */
    if (event->type == WM_CHAR) {
        unsigned char ch = (unsigned char)event->charev.ch;
        if (ch == '\x03') {   /* Ctrl+C (translated) — break, don't buffer:
                                 the WM_KEYDOWN path already pushed 0x03 */
            MMAbort = 1;
            return true;
        }
        /* Ctrl chords aren't text: the OS also delivers the bare letter
         * of a Ctrl+letter chord through WM_CHAR (Ctrl+C left a stray
         * 'c' at the prompt after a break).  The WM_KEYDOWN handler owns
         * chord semantics. */
        if ((event->charev.modifiers & KMOD_CTRL) && ch >= 0x20)
            return true;
        /* Non-ASCII only: the OS keyboard layer ALSO delivers special
         * keys (arrows etc.) as UTF-8 extended chars (C2 8x) through
         * WM_CHAR.  We already push those keys as VT escape sequences
         * from WM_KEYDOWN, and the stray 0x8x continuation byte lands
         * exactly in PicoMite's arrow-code range — every arrow press
         * executed a second, WRONG arrow ("diagonal" cursor motion). */
        if (ch != '\0' && ch < 0x80)
            basic_kbuf_push(ch);
        return true;
    }

    /* WM_KEYDOWN: navigation and function keys only (they have no WM_CHAR). */
    if (event->type == WM_KEYDOWN) {
        uint8_t sc  = event->key.scancode;
        uint8_t mod = event->key.modifiers;

        /* Ctrl+C via raw scan: HID 'c' = 0x06 */
        if ((mod & KMOD_CTRL) && sc == 0x06) {
            MMAbort = 1;
            basic_kbuf_push(3);
            return true;
        }

        /* Alt+Enter → toggle fullscreen */
        if (sc == 0x28 && (mod & KMOD_ALT)) {
            wm_toggle_fullscreen(hwnd);
            return true;
        }

        /* Enter, Backspace, Tab, Esc — the OS only sends WM_CHAR for
         * printable ASCII (0x20-0x7E), so these must be handled here. */
        switch (sc) {
        case 0x28: basic_kbuf_push('\r');  return true;  /* Enter     */
        case 0x58: basic_kbuf_push('\r');  return true;  /* KP Enter  */
        case 0x2A: basic_kbuf_push('\b');  return true;  /* Backspace */
        case 0x2B: basic_kbuf_push('\t');  return true;  /* Tab       */
        case 0x29: basic_kbuf_push(0x1B);  return true;  /* Esc       */
        case 0x4C: basic_kbuf_push(0x7F);  return true;  /* Delete    */
        }

        /* HID scancodes → VT100 / xterm escape sequences that MMInkey()
         * in PicoMite.c parses back to UP/DOWN/LEFT/RIGHT/HOME/…/F1…F12. */
        switch (sc) {
        case 0x52: push_vt("\x1b[A");   return true;  /* Up       */
        case 0x51: push_vt("\x1b[B");   return true;  /* Down     */
        case 0x4F: push_vt("\x1b[C");   return true;  /* Right    */
        case 0x50: push_vt("\x1b[D");   return true;  /* Left     */
        case 0x4A: push_vt("\x1b[1~");  return true;  /* Home     */
        case 0x4D: push_vt("\x1b[4~");  return true;  /* End      */
        case 0x4B: push_vt("\x1b[5~");  return true;  /* Page Up  */
        case 0x4E: push_vt("\x1b[6~");  return true;  /* Page Dn  */
        case 0x49: push_vt("\x1b[2~");  return true;  /* Insert   */
        case 0x3A: push_vt("\x1bOP");   return true;  /* F1       */
        case 0x3B: push_vt("\x1bOQ");   return true;  /* F2       */
        case 0x3C: push_vt("\x1bOR");   return true;  /* F3       */
        case 0x3D: push_vt("\x1bOS");   return true;  /* F4       */
        case 0x3E: push_vt("\x1b[15~"); return true;  /* F5       */
        case 0x3F: push_vt("\x1b[17~"); return true;  /* F6       */
        case 0x40: push_vt("\x1b[18~"); return true;  /* F7       */
        case 0x41: push_vt("\x1b[19~"); return true;  /* F8       */
        case 0x42: push_vt("\x1b[20~"); return true;  /* F9       */
        case 0x43: push_vt("\x1b[21~"); return true;  /* F10      */
        case 0x44: push_vt("\x1b[23~"); return true;  /* F11      */
        case 0x45: push_vt("\x1b[24~"); return true;  /* F12      */
        default:   break;
        }
        return false;
    }

    return false;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Display flush + cursor blink timer (25 ms period)
 *
 * One repaint per tick at most: text output and cell fills only set
 * g_disp_dirty; this timer turns it into a single wm_invalidate.  Also
 * watches CurrentX/CurrentY and the ShowCursor flag so pure cursor
 * movement (MX470Cursor emits no characters) still repaints, and
 * toggles the blink state every 20 ticks (500 ms).
 * ═════════════════════════════════════════════════════════════════════════ */

static void blink_cb(TimerHandle_t t)
{
    (void)t;
    static int   tick = 0;
    static short last_x = -1, last_y = -1;
    static bool  last_on = true;

    {
        static int mark_r = -1, mark_c = -1;
        bool cursor_evt = false;
        if (++tick >= 20) {
            tick = 0;
            g_cur_vis = !g_cur_vis;
            cursor_evt = true;
        }
        if (CurrentX != last_x || CurrentY != last_y ||
            g_vt_cursor_on != last_on) {
            last_x  = CurrentX;
            last_y  = CurrentY;
            last_on = g_vt_cursor_on;
            cursor_evt = true;
        }
        if (cursor_evt) {
            int cr = CurrentY / BASIC_FH, cc = CurrentX / BASIC_FW;
            if (mark_r >= 0)
                mark_cells(mark_r, mark_c, mark_r, mark_c);
            mark_cells(cr, cc, cr, cc);
            mark_r = cr; mark_c = cc;
        }
    }
    {
        static int unfocused_skip = 0;
        if (g_disp_dirty && g_hwnd != HWND_NULL) {
            if (g_focused || ++unfocused_skip >= 10) {
                unfocused_skip = 0;
                g_disp_dirty = false;
                wm_invalidate(g_hwnd);
            }
        }
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * Frank OS app entry point
 * ═════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    /* ── Singleton: if already running, focus existing window ──── */
    hwnd_t existing = wm_find_window_by_title("MMBasic");
    if (existing != HWND_NULL) {
        wm_set_focus(existing);
        return 0;
    }

    printf("[basic] main() start\n");
    g_task = xTaskGetCurrentTaskHandle();
    printf("[basic] task handle: %p\n", (void*)g_task);

    /* Store autorun path from file association launch */
    g_autorun_path[0] = '\0';
    if (argc > 1 && argv[1] && argv[1][0]) {
        strncpy(g_autorun_path, argv[1], sizeof(g_autorun_path) - 1);
        g_autorun_path[sizeof(g_autorun_path) - 1] = '\0';
    }

    /* Allocate text buffer from PSRAM heap before any display calls. */
    if (!g_textbuf)
        g_textbuf = (volatile cell_t (*)[BASIC_COLS])
                    calloc(BASIC_ROWS, sizeof(*g_textbuf));
    printf("[basic] g_textbuf: %p\n", (void*)g_textbuf);
    if (!g_textbuf) {
        printf("[basic] FATAL: g_textbuf calloc failed\n");
        return 1;  /* fatal: no display buffer */
    }

    tbuf_clear();
    printf("[basic] tbuf_clear done\n");

    /* Centre the window (no taskbar overlap). */
    int16_t fw = (int16_t)BASIC_WIN_W;
    int16_t fh = (int16_t)BASIC_WIN_H;
    int16_t x  = (int16_t)((DISPLAY_WIDTH  - fw) / 2);
    int16_t y  = (int16_t)((DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2);
    if (y < 0) y = 0;
    printf("[basic] wm_create_window %dx%d at (%d,%d)\n", fw, fh, x, y);

    g_hwnd = wm_create_window(x, y, fw, fh, "MMBasic",
                              WSTYLE_DIALOG | WF_FULLSCREENABLE | WF_HIDE_CURSOR |
                              WF_NOCLEAR,
                              basic_event, basic_paint);
    printf("[basic] g_hwnd: %p\n", (void*)(uintptr_t)g_hwnd);
    if (g_hwnd == HWND_NULL) {
        printf("[basic] FATAL: wm_create_window returned NULL\n");
        return 1;
    }

    wm_show_window(g_hwnd);
    wm_set_focus(g_hwnd);
    taskbar_invalidate();

    /* Start display flush / cursor blink tick (25 ms, auto-reload). */
    TimerHandle_t blink_tmr = xTimerCreate("BLINK",
                                            pdMS_TO_TICKS(25),
                                            pdTRUE, NULL, blink_cb);
    if (blink_tmr)
        xTimerStart(blink_tmr, 0);

    /* Initialise platform and run the interpreter (blocks until exit). */
    basic_platform_init();
    basic_run_interpreter();

    /* Tear down. */
    {   /* stop any playback and delete the audio pump task before the
         * app's code/data disappear (frankos_audio.c) */
        extern void frankos_audio_shutdown(void);
        frankos_audio_shutdown();
    }
    if (blink_tmr) {
        xTimerStop(blink_tmr, 0);
        xTimerDelete(blink_tmr, 0);
    }
    wm_destroy_window(g_hwnd);
    g_hwnd = HWND_NULL;
    taskbar_invalidate();

    return 0;
}

/* BACKGROUND: keep executing when another app has focus — without it
 * the OS swap layer suspends the interpreter task on focus loss (a
 * running BASIC program froze whenever e.g. Calculator was focused). */
uint32_t __app_flags(void) { return APPFLAG_SINGLETON | APPFLAG_BACKGROUND; }
