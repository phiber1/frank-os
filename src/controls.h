/*
 * FRANK OS — Reusable UI Controls
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Struct-based controls drawn within a parent window's client area.
 * Parent paint/event handlers forward to control functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"
#include "window_event.h"

/*==========================================================================
 * Scrollbar
 *=========================================================================*/

#define SCROLLBAR_WIDTH  16

typedef struct {
    int16_t  x, y, w, h;     /* position/size in client coords */
    bool     horizontal;      /* false = vertical, true = horizontal */
    bool     visible;         /* auto show/hide based on content vs viewport */
    int32_t  range;           /* total content size */
    int32_t  page;            /* visible page size */
    int32_t  pos;             /* current scroll position */

    /* Internal drag state */
    bool     dragging;        /* thumb drag active */
    int16_t  drag_offset;     /* mouse offset from thumb top */
} scrollbar_t;

void scrollbar_init(scrollbar_t *sb, bool horizontal);
void scrollbar_set_range(scrollbar_t *sb, int32_t range, int32_t page);
void scrollbar_set_pos(scrollbar_t *sb, int32_t pos);
void scrollbar_paint(scrollbar_t *sb);
bool scrollbar_event(scrollbar_t *sb, const window_event_t *event,
                     int32_t *new_pos);

/*==========================================================================
 * Textarea
 *=========================================================================*/

#define TEXTAREA_MAX_SIZE  32768  /* 32KB max */

typedef struct {
    /* Text buffer (caller-allocated) */
    char    *buf;             /* flat char buffer */
    int32_t  buf_size;        /* allocated size */
    int32_t  len;             /* current text length */

    /* Cursor */
    int32_t  cursor;          /* byte offset in buf */
    int32_t  sel_anchor;      /* selection start (-1 = no selection) */
    bool     cursor_visible;  /* blink state */

    /* Viewport */
    int16_t  rect_x, rect_y; /* position in client coords */
    int16_t  rect_w, rect_h; /* size in client coords */
    int32_t  scroll_x;       /* horizontal scroll (pixels) */
    int32_t  scroll_y;       /* vertical scroll (pixels) */

    /* Embedded scrollbars */
    scrollbar_t  vscroll;
    scrollbar_t  hscroll;

    /* Parent window (for invalidation) */
    hwnd_t   hwnd;

    /* Computed layout (updated by set_rect / text changes) */
    int32_t  total_lines;     /* total lines in buffer */
    int32_t  max_line_width;  /* longest line in chars */
} textarea_t;

void textarea_init(textarea_t *ta, char *buf, int32_t buf_size, hwnd_t hwnd);
void textarea_set_text(textarea_t *ta, const char *text, int32_t len);
const char *textarea_get_text(textarea_t *ta);
int32_t textarea_get_length(textarea_t *ta);
void textarea_set_rect(textarea_t *ta, int16_t x, int16_t y,
                        int16_t w, int16_t h);
void textarea_paint(textarea_t *ta);
bool textarea_event(textarea_t *ta, const window_event_t *event);
void textarea_cut(textarea_t *ta);
void textarea_copy(textarea_t *ta);
void textarea_paste(textarea_t *ta);
void textarea_select_all(textarea_t *ta);
bool textarea_find(textarea_t *ta, const char *needle, bool case_sensitive,
                    bool forward);
bool textarea_replace(textarea_t *ta, const char *needle,
                       const char *replacement, bool case_sensitive);
int  textarea_replace_all(textarea_t *ta, const char *needle,
                           const char *replacement, bool case_sensitive);
void textarea_blink(textarea_t *ta);

/*==========================================================================
 * Checkbox
 *=========================================================================*/

#define CHECKBOX_SIZE  13

typedef struct {
    int16_t  x, y;
    bool     checked;
    char     label[24];
} checkbox_t;

void checkbox_init(checkbox_t *cb, int16_t x, int16_t y, const char *label);
void checkbox_paint(checkbox_t *cb);
bool checkbox_event(checkbox_t *cb, const window_event_t *event, bool *changed);

/*==========================================================================
 * Radio Button Group
 *=========================================================================*/

#define RADIO_MAX_OPTIONS 6

typedef struct {
    int16_t  x, y;
    uint8_t  count;
    uint8_t  selected;
    int16_t  spacing;
    char     labels[RADIO_MAX_OPTIONS][24];
} radiogroup_t;

void radiogroup_init(radiogroup_t *rg, int16_t x, int16_t y,
                     uint8_t count, int16_t spacing);
void radiogroup_set_labels(radiogroup_t *rg, const char **labels);
void radiogroup_paint(radiogroup_t *rg);
bool radiogroup_event(radiogroup_t *rg, const window_event_t *event,
                      uint8_t *new_sel);

/*==========================================================================
 * Single-line Text Field
 *=========================================================================*/

typedef struct {
    int16_t  x, y, w, h;
    char    *buf;
    int16_t  buf_size;
    int16_t  len;
    int16_t  cursor;
    int16_t  scroll_x;
    bool     cursor_visible;
    bool     focused;
    hwnd_t   hwnd;
} textfield_t;

void textfield_init(textfield_t *tf, char *buf, int16_t buf_size, hwnd_t hwnd);
void textfield_set_rect(textfield_t *tf, int16_t x, int16_t y,
                        int16_t w, int16_t h);
void textfield_paint(textfield_t *tf);
bool textfield_event(textfield_t *tf, const window_event_t *event);
void textfield_blink(textfield_t *tf);
void textfield_set_text(textfield_t *tf, const char *text);
const char *textfield_get_text(textfield_t *tf);
int16_t textfield_get_length(textfield_t *tf);

/*==========================================================================
 * Slider
 *=========================================================================*/

typedef struct {
    int16_t  x, y, w, h;
    bool     horizontal;
    int32_t  min_val, max_val;
    int32_t  value;
    int32_t  step;
    bool     dragging;
    int16_t  drag_offset;
} slider_t;

void slider_init(slider_t *sl, bool horizontal);
void slider_set_range(slider_t *sl, int32_t min_val, int32_t max_val,
                      int32_t step);
void slider_set_rect(slider_t *sl, int16_t x, int16_t y,
                     int16_t w, int16_t h);
void slider_paint(slider_t *sl);
bool slider_event(slider_t *sl, const window_event_t *event, int32_t *new_val);

/*==========================================================================
 * Combobox (text field + dropdown)
 *=========================================================================*/

typedef struct {
    textfield_t  field;
    int16_t      drop_btn_w;
    bool         drop_open;
    int8_t       drop_hover;
    int8_t       drop_scroll;
    int8_t       item_count;
    char       (*items)[64];
    int16_t      max_visible;
    int16_t      drop_item_h;
    hwnd_t       hwnd;
} combobox_t;

void combobox_init(combobox_t *cb, char *buf, int16_t buf_size, hwnd_t hwnd);
void combobox_set_rect(combobox_t *cb, int16_t x, int16_t y,
                       int16_t w, int16_t h);
void combobox_set_items(combobox_t *cb, char (*items)[64], int8_t count);
void combobox_paint(combobox_t *cb);
bool combobox_event(combobox_t *cb, const window_event_t *event);
bool combobox_is_open(combobox_t *cb);
void combobox_select_item(combobox_t *cb, int8_t index);

#endif /* CONTROLS_H */
