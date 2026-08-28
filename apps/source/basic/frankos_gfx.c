/*
 * frankos_gfx.c — pixel graphics layer for the Frank OS port of MMBasic
 *
 * Draw.c (the full PicoMite graphics engine: primitives, sprites, blits,
 * framebuffer commands) dispatches all pixel output through function
 * pointers (DrawRectangle, DrawPixel, DrawBitmap, Read/DrawBuffer...).
 * On real PicoMite hardware those point at SPI-LCD drivers; here they
 * point at a 632x384 4bpp nibble-packed backing buffer with the same
 * pixel layout as the Frank OS framebuffer.  frankos_main.c's paint
 * composites it as the background layer beneath the text-cell console
 * (blank default-attribute cells are transparent).
 *
 * Interchange formats (matching the SPI-LCD reference drivers):
 *   Read/DrawBuffer: RGB888, 3 bytes/pixel, row-major
 *   DrawBitmap:      MSB-first bitstream, bc == -1 -> transparent bg
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define GFX_W   632
#define GFX_H   384
#define GFX_BPR (GFX_W / 2)         /* bytes per row, 4bpp */

/* Draw.c globals / pointers we hook (types verified against Draw.c) */
extern short HRes, VRes, DisplayHRes, DisplayVRes;
extern short CurrentX, CurrentY;
extern int   gui_fcolour, gui_bcolour;
extern unsigned char *FontTable[];
extern void (*DrawRectangle)(int x1, int y1, int x2, int y2, int c);
extern void (*DrawBitmap)(int x1, int y1, int width, int height, int scale,
                          int fc, int bc, unsigned char *bitmap);
extern void (*ScrollLCD)(int lines);
extern void (*DrawBuffer)(int x1, int y1, int x2, int y2, unsigned char *c);
extern void (*ReadBuffer)(int x1, int y1, int x2, int y2, unsigned char *c);
extern void (*DrawBLITBuffer)(int x1, int y1, int x2, int y2, unsigned char *c);
extern void (*ReadBLITBuffer)(int x1, int y1, int x2, int y2, unsigned char *c);
extern void (*ReadBufferFast)(int x1, int y1, int x2, int y2, unsigned char *c);
extern void (*DrawPixel)(int x1, int y1, int c);
extern void (*DrawBufferFast)(int x1, int y1, int x2, int y2, int blank,
                              unsigned char *c);
extern unsigned char *WriteBuf;
extern void SetFont(int fnt);

/* RGB888 -> Frank OS 16-colour index (frankos_main.c) */
extern uint8_t basic_rgb2cga(int rgb);

/* The graphics backing buffer, composited by basic_paint() */
uint8_t *basic_gfx_buf;

/* Dirty-region reporting (frankos_main.c): each primitive marks its
 * bounding rect so the paint re-composites only what changed; the 25 ms
 * flush timer turns marks into one wm_invalidate. */
extern void basic_gfx_mark(int x1, int y1, int x2, int y2);

/* Sprite compound-operation gate.  SPRITE SHOW/HIDE do "restore old
 * background, redraw at new position" as two buffer passes; a repaint
 * sampling between them shows the sprite half-erased (heavy flicker).
 * Draw.c's BlitShowBuffer/hidesafe/showsafe hold this non-zero for the
 * duration; basic_paint() skips-and-retries while held so only complete
 * frames reach the screen. */
volatile int basic_gfx_hold;

/* Frank OS 16-colour palette as RGB888 (for ReadBuffer round-trips).
 * Standard VGA-style values matching the OS compositor's palette. */
static const uint32_t cga2rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static inline void px_set(int x, int y, uint8_t c)
{
    uint8_t *p = basic_gfx_buf + y * GFX_BPR + (x >> 1);
    if (x & 1)
        *p = (*p & 0xF0) | c;
    else
        *p = (*p & 0x0F) | (c << 4);
}

static inline uint8_t px_get(int x, int y)
{
    uint8_t b = basic_gfx_buf[y * GFX_BPR + (x >> 1)];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

/* Order + clip a rectangle to the buffer; returns 0 if fully outside */
static int clip_rect(int *x1, int *y1, int *x2, int *y2)
{
    int t;
    if (*x2 < *x1) { t = *x1; *x1 = *x2; *x2 = t; }
    if (*y2 < *y1) { t = *y1; *y1 = *y2; *y2 = t; }
    if (*x2 < 0 || *y2 < 0 || *x1 >= GFX_W || *y1 >= GFX_H) return 0;
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 >= GFX_W) *x2 = GFX_W - 1;
    if (*y2 >= GFX_H) *y2 = GFX_H - 1;
    return 1;
}

/* ── Primitives installed into Draw.c's function pointers ───────────────── */

static void gfx_DrawPixel(int x, int y, int c)
{
    if (x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) return;
    px_set(x, y, basic_rgb2cga(c));
    basic_gfx_mark(x, y, x, y);
}

static void gfx_DrawRectangle(int x1, int y1, int x2, int y2, int c)
{
    if (!clip_rect(&x1, &y1, &x2, &y2)) return;
    uint8_t ci  = basic_rgb2cga(c);
    uint8_t two = (uint8_t)((ci << 4) | ci);
    for (int y = y1; y <= y2; y++) {
        int x = x1;
        uint8_t *row = basic_gfx_buf + y * GFX_BPR;
        if (x & 1) { row[x >> 1] = (row[x >> 1] & 0xF0) | ci; x++; }
        while (x + 1 <= x2) { row[x >> 1] = two; x += 2; }
        if (x == x2) row[x >> 1] = (row[x >> 1] & 0x0F) | (ci << 4);
    }
    basic_gfx_mark(x1, y1, x2, y2);
}

/* MSB-first bitstream, row-major, scaled; bc == -1 leaves bg untouched */
static void gfx_DrawBitmap(int x1, int y1, int width, int height, int scale,
                           int fc, int bc, unsigned char *bitmap)
{
    uint8_t fci = basic_rgb2cga(fc);
    uint8_t bci = (bc == -1) ? 0 : basic_rgb2cga(bc);
    int n = 0;
    for (int i = 0; i < height; i++) {             /* source row     */
        for (int j = 0; j < scale; j++) {          /* row scaling    */
            int y = y1 + i * scale + j;
            for (int k = 0; k < width; k++) {      /* source column  */
                int set = (bitmap[((i * width) + k) / 8] >>
                           (((height * width) - ((i * width) + k) - 1) % 8)) & 1;
                for (int m = 0; m < scale; m++) {  /* col scaling    */
                    int x = x1 + k * scale + m;
                    if (x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) continue;
                    if (set)
                        px_set(x, y, fci);
                    else if (bc != -1)
                        px_set(x, y, bci);
                }
            }
        }
        (void)n;
    }
    basic_gfx_mark(x1, y1, x1 + width * scale - 1, y1 + height * scale - 1);
}

static void gfx_ReadBuffer(int x1, int y1, int x2, int y2, unsigned char *p)
{
    if (!clip_rect(&x1, &y1, &x2, &y2)) return;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) {
            uint32_t rgb = cga2rgb[px_get(x, y)];
            *p++ = (uint8_t)(rgb >> 16);
            *p++ = (uint8_t)(rgb >> 8);
            *p++ = (uint8_t)rgb;
        }
}

static void gfx_DrawBuffer(int x1, int y1, int x2, int y2, unsigned char *p)
{
    int t;
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) {
            int rgb = (p[0] << 16) | (p[1] << 8) | p[2];
            p += 3;
            if (x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) continue;
            px_set(x, y, basic_rgb2cga(rgb));
        }
    basic_gfx_mark(x1, y1, x2, y2);
}

/* ── 4bpp sprite-stream buffer pair ("Fast" variants) ───────────────────────
 * PicoMite sprite buffers are nibble STREAMS: one nibble per pixel in
 * reading order, continuous across rows; even stream index = LOW nibble
 * of the stream byte.  This convention is fixed by SPRITE's bounds /
 * collision scanner and LOADARRAY, which parse the buffers directly —
 * only the SCREEN-side nibble order (our even pixel = high nibble)
 * differs from the reference RGB121.c pair.  blank == -1 draws every
 * pixel (opaque background restore); otherwise nibbles equal to
 * sprite_transparent are left undrawn. */
extern uint8_t sprite_transparent;      /* Draw.c, default 0 */

/* Both functions stage each row's touched byte-span through an SRAM
 * scratch buffer: the gfx buffer lives in UNCACHED PSRAM, where every
 * byte access is a full QSPI transaction — per-pixel read-modify-write
 * made a 24x24 sprite compound cost milliseconds.  One memcpy in, nibble
 * work in SRAM, one memcpy out. */

static void gfx_ReadBufferFast(int x1, int y1, int x2, int y2, unsigned char *c)
{
    int t, toggle = 0;
    uint8_t row[GFX_BPR];
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    int cx1 = x1 < 0 ? 0 : x1;
    int cx2 = x2 >= GFX_W ? GFX_W - 1 : x2;
    int b0 = cx1 >> 1;
    int nb = (cx1 <= cx2) ? (cx2 >> 1) - b0 + 1 : 0;
    for (int y = y1; y <= y2; y++) {
        int rowok = (y >= 0 && y < GFX_H && nb > 0);
        if (rowok)
            memcpy(row, basic_gfx_buf + y * GFX_BPR + b0, (size_t)nb);
        for (int x = x1; x <= x2; x++) {
            uint8_t nib = 0;
            if (rowok && x >= cx1 && x <= cx2) {
                uint8_t b = row[(x >> 1) - b0];
                nib = (x & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
            }
            if (toggle)
                *c++ |= (uint8_t)(nib << 4);
            else
                *c = nib;
            toggle ^= 1;
        }
    }
}

static void gfx_DrawBufferFast(int x1, int y1, int x2, int y2, int blank,
                               unsigned char *p)
{
    int t, toggle = 0;
    uint8_t row[GFX_BPR];
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    int cx1 = x1 < 0 ? 0 : x1;
    int cx2 = x2 >= GFX_W ? GFX_W - 1 : x2;
    int b0 = cx1 >> 1;
    int nb = (cx1 <= cx2) ? (cx2 >> 1) - b0 + 1 : 0;
    for (int y = y1; y <= y2; y++) {
        int rowok = (y >= 0 && y < GFX_H && nb > 0);
        int touched = 0;
        if (rowok)
            memcpy(row, basic_gfx_buf + y * GFX_BPR + b0, (size_t)nb);
        for (int x = x1; x <= x2; x++) {
            uint8_t nib = toggle ? (uint8_t)(*p >> 4) : (uint8_t)(*p & 0x0F);
            if (toggle) p++;
            toggle ^= 1;
            if (!rowok || x < cx1 || x > cx2) continue;
            if (nib != sprite_transparent || blank == -1) {
                int i = (x >> 1) - b0;
                row[i] = (x & 1) ? (uint8_t)((row[i] & 0xF0) | nib)
                                 : (uint8_t)((row[i] & 0x0F) | (nib << 4));
                touched = 1;
            }
        }
        if (touched)
            memcpy(basic_gfx_buf + y * GFX_BPR + b0, row, (size_t)nb);
    }
    basic_gfx_mark(cx1, y1, cx2, y2);
}

static void gfx_ScrollLCD(int lines)
{
    if (lines == 0) return;
    if (lines > 0 && lines < GFX_H) {
        memmove(basic_gfx_buf, basic_gfx_buf + lines * GFX_BPR,
                (GFX_H - lines) * GFX_BPR);
        memset(basic_gfx_buf + (GFX_H - lines) * GFX_BPR, 0, lines * GFX_BPR);
    } else if (lines < 0 && -lines < GFX_H) {
        memmove(basic_gfx_buf + (-lines) * GFX_BPR, basic_gfx_buf,
                (GFX_H + lines) * GFX_BPR);
        memset(basic_gfx_buf, 0, (-lines) * GFX_BPR);
    }
    basic_gfx_mark(0, 0, GFX_W - 1, GFX_H - 1);
}

/* ── 8x16 console font in PicoMite format ───────────────────────────────────
 * Draw.c's bundled fonts have no 8x16 face, but the text-cell console's
 * geometry (and the editor's row math) requires gui_font 8x16.  Convert
 * the OS font (16 bytes/glyph, LSB = leftmost pixel) into PicoMite's
 * {w, h, first, count} + MSB-first format and install it as font 1. */
extern const uint8_t font_8x16[4096];
/* Exported: Draw.c's initFonts() re-pins FontTable[1] to this after its
 * stock-font install (SaveProgramToFlash calls initFonts on every program
 * save, which was clobbering our 8x16 with the 12x20 Misc font). */
uint8_t basic_pm_font8x16[4 + 4096];

static void build_font(void)
{
    basic_pm_font8x16[0] = 8;
    basic_pm_font8x16[1] = 16;
    basic_pm_font8x16[2] = 0;      /* first char */
    basic_pm_font8x16[3] = 255;    /* count      */
    for (int i = 0; i < 4096; i++) {
        uint8_t b = font_8x16[i], r = 0;
        for (int k = 0; k < 8; k++)
            if (b & (1u << k)) r |= (uint8_t)(0x80u >> k);
        basic_pm_font8x16[4 + i] = r;
    }
}

/* Clear the whole graphics layer to a colour (hooked into ClearScreen —
 * Draw.c's own clear body is gated on a physical DISPLAY_TYPE). */
void basic_gfx_clear(int colour)
{
    if (!basic_gfx_buf)
        return;
    uint8_t ci = basic_rgb2cga(colour);
    memset(basic_gfx_buf, (ci << 4) | ci, GFX_H * GFX_BPR);
    basic_gfx_mark(0, 0, GFX_W - 1, GFX_H - 1);
}

/* ── Init: buffer, font, geometry, pointer installation ─────────────────── */

extern void serial_dbg(const char *msg);   /* frankos_platform.c */

/* Allocate via the OS allocator directly through the sys table (slot 166,
 * __calloc: PSRAM-first, ctx-tracked).  MUST stay a direct sys-table call:
 * routing this through the app-global calloc wrapper chain produced a
 * mispatched inter-object BL (loader relocation quirk) that branched into
 * garbage and wedged the whole OS at launch. */
static void *gfx_calloc(size_t n, size_t sz)
{
    static void * const * const _st = (void * const *)0x10FFF000UL;
    typedef void *(*fn)(size_t, size_t);
    return ((fn)_st[166])(n, sz);
}

void basic_gfx_init(void)
{
    if (!basic_gfx_buf)
        basic_gfx_buf = (uint8_t *)gfx_calloc(1, GFX_H * GFX_BPR);

    build_font();
    FontTable[1] = basic_pm_font8x16;

    HRes = GFX_W;
    VRes = GFX_H;
    DisplayHRes = GFX_W;
    DisplayVRes = GFX_H;
    CurrentX = CurrentY = 0;
    gui_fcolour = 0xFFFFFF;
    gui_bcolour = 0x000000;
    SetFont(0x11);           /* font 1 (ours), scale 1 -> 8x16 */

    /* No backing buffer: leave the Draw.c function pointers on their
     * DisplayNotSet no-ops so graphics commands are inert instead of
     * writing through NULL.  Console/editor still work (font installed). */
    if (!basic_gfx_buf) {
        serial_dbg("[gfx] no buffer - primitives disabled\n");
        return;
    }

    DrawPixel      = gfx_DrawPixel;
    DrawRectangle  = gfx_DrawRectangle;
    DrawBitmap     = gfx_DrawBitmap;
    DrawBuffer     = gfx_DrawBuffer;
    ReadBuffer     = gfx_ReadBuffer;
    ReadBufferFast = gfx_ReadBufferFast;   /* 4bpp sprite streams */
    DrawBufferFast = gfx_DrawBufferFast;
    DrawBLITBuffer = gfx_DrawBuffer;
    ReadBLITBuffer = gfx_ReadBuffer;
    ScrollLCD      = gfx_ScrollLCD;

    /* SPRITE refuses to run "on a physical display": it demands a
     * memory framebuffer (WriteBuf) it can read back.  Our graphics
     * layer IS memory — point WriteBuf at it.  (restorepanel and
     * closeframebuffer re-pin instead of NULLing it, see Draw.c.) */
    WriteBuf = basic_gfx_buf;
}

/* ── Dead hardware references ───────────────────────────────────────────────
 * Draw.c links against globals normally defined in GUI.c and the SPI-LCD /
 * SSD1963 / Touch panel drivers, none of which are compiled here.  Every
 * code path that could reach these is gated on Option.DISPLAY_TYPE being a
 * physical panel type, which never happens on this platform (it stays 0 and
 * setframebuffer() early-returns), so no-op stubs are safe. */

short gui_font_width, gui_font_height;   /* set by Draw.c's SetFont() */
int   last_fcolour, last_bcolour;
int   HResD, VResD, HResS, VResS;
int   InvokingCtrl;

void ResetGUI(void) {}
void HideAllControls(void) {}
void GetCalibration(int x, int y, int *xval, int *yval)
{
    (void)x; (void)y;
    *xval = *yval = 0;
}
int  GetTouchValue(int cmd) { (void)cmd; return 0; }
int  GetTouch(int x)        { (void)x; return -1; }   /* TOUCH_ERROR */
int  GetTouchAxis(int axis) { (void)axis; return -1; }

void InitDisplaySPI(int InitOnly) { (void)InitOnly; }
void InitDisplayI2C(int InitOnly) { (void)InitOnly; }
void ClearCS(int pin) { (void)pin; }
void spi_write_fast(void *spi, const uint8_t *src, size_t len)
    { (void)spi; (void)src; (void)len; }
void spi_finish(void *spi) { (void)spi; }
void DefineRegionSPI(int xstart, int ystart, int xend, int yend, int rw)
    { (void)xstart; (void)ystart; (void)xend; (void)yend; (void)rw; }
int  GetLineILI9341(void) { return 0; }
void SetAreaILI9341(int xs, int ys, int xe, int ye, int rw)
    { (void)xs; (void)ys; (void)xe; (void)ye; (void)rw; }
void SetAreaSSD1963(int x1, int y1, int x2, int y2)
    { (void)x1; (void)y1; (void)x2; (void)y2; }
void SetAreaIPS_4_16(int xs, int ys, int xe, int ye, int rw)
    { (void)xs; (void)ys; (void)xe; (void)ye; (void)rw; }
void ScrollSSD1963(int lines) { (void)lines; }
void Write16bitCommand(int cmd) { (void)cmd; }
void WriteData16bit(int data) { (void)data; }
void WriteCmdDataIPS_4_16(int cmd, int n, int data)
    { (void)cmd; (void)n; (void)data; }
void blit121(uint8_t *s, uint8_t *d, int xs, int ys, int w, int h,
             int xd, int yd, int mc)
    { (void)s; (void)d; (void)xs; (void)ys; (void)w; (void)h;
      (void)xd; (void)yd; (void)mc; }

#define STUB_PX(name)   void name(int x, int y, int c) \
    { (void)x; (void)y; (void)c; }
#define STUB_RECT(name) void name(int x1, int y1, int x2, int y2, int c) \
    { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
#define STUB_BMP(name)  void name(int x1, int y1, int w, int h, int sc, \
                                  int fc, int bc, unsigned char *bm) \
    { (void)x1; (void)y1; (void)w; (void)h; (void)sc; \
      (void)fc; (void)bc; (void)bm; }
#define STUB_BUF(name)  void name(int x1, int y1, int x2, int y2, \
                                  unsigned char *c) \
    { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
#define STUB_SCR(name)  void name(int lines) { (void)lines; }

STUB_PX(DrawPixel16)         STUB_PX(DrawPixel222)      STUB_PX(DrawPixelMEM332)
STUB_RECT(DrawRectangle16)   STUB_RECT(DrawRectangle222)
STUB_RECT(DrawRectangleMEM332)
STUB_RECT(DrawRectangleSPI)  STUB_RECT(DrawRectangleSPISCR)
STUB_BMP(DrawBitmap16)       STUB_BMP(DrawBitmap222)    STUB_BMP(DrawBitmapMEM332)
STUB_BMP(DrawBitmapSPI)      STUB_BMP(DrawBitmapSPISCR)
STUB_BUF(DrawBuffer16)       STUB_BUF(DrawBuffer16Fast) STUB_BUF(DrawBuffer222)
STUB_BUF(DrawBufferMEM332)   STUB_BUF(DrawBufferSPI)    STUB_BUF(DrawBufferSPISCR)
STUB_BUF(ReadBuffer16Fast)   STUB_BUF(ReadBuffer222)    STUB_BUF(ReadBufferMEM332)
STUB_BUF(ReadBufferSPI)      STUB_BUF(ReadBufferSPISCR)
STUB_BUF(DrawBlitBufferMEM332)
STUB_BUF(ReadBlitBufferMEM332)
STUB_SCR(ScrollLCD16)        STUB_SCR(ScrollLCD222)     STUB_SCR(ScrollLCDSPI)
