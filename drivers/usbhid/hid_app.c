/*
 * FRANK OS — USB HID Host Callbacks
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * TinyUSB Host callbacks for keyboard and mouse.
 * Based on TinyUSB HID host example.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tusb.h"
#include "usbhid.h"
#include <string.h>

#if CFG_TUH_RPI_PIO_USB
/* Fruit Jam: USB-A host jacks are on PIO-USB, not the native controller */
#include <stdio.h>
#include "pico/stdlib.h"
#include "board_config.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#endif

#if CFG_TUH_ENABLED

#define MAX_REPORT 4

static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

static volatile int16_t cumulative_dx = 0;
static volatile int16_t cumulative_dy = 0;
static volatile int8_t  cumulative_wheel = 0;
static volatile uint8_t current_buttons = 0;
static volatile int     mouse_has_update = 0;

static volatile int keyboard_connected = 0;
static volatile int mouse_connected = 0;

#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int     down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

static void process_kbd_report(hid_keyboard_report_t const *report,
                               hid_keyboard_report_t const *prev_report) {
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods  = report->modifier & ~(prev_report->modifier);

    /* Send separate L/R modifier keycodes matching HID usage page 0x07 */
    if (released_mods & KEYBOARD_MODIFIER_LEFTCTRL)   queue_key_action(0xE0, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_LEFTCTRL)   queue_key_action(0xE0, 1);
    if (released_mods & KEYBOARD_MODIFIER_LEFTSHIFT)   queue_key_action(0xE1, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_LEFTSHIFT)   queue_key_action(0xE1, 1);
    if (released_mods & KEYBOARD_MODIFIER_LEFTALT)     queue_key_action(0xE2, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_LEFTALT)     queue_key_action(0xE2, 1);
    if (released_mods & KEYBOARD_MODIFIER_LEFTGUI)     queue_key_action(0xE3, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_LEFTGUI)     queue_key_action(0xE3, 1);
    if (released_mods & KEYBOARD_MODIFIER_RIGHTCTRL)   queue_key_action(0xE4, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_RIGHTCTRL)   queue_key_action(0xE4, 1);
    if (released_mods & KEYBOARD_MODIFIER_RIGHTSHIFT)  queue_key_action(0xE5, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_RIGHTSHIFT)  queue_key_action(0xE5, 1);
    if (released_mods & KEYBOARD_MODIFIER_RIGHTALT)    queue_key_action(0xE6, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_RIGHTALT)    queue_key_action(0xE6, 1);
    if (released_mods & KEYBOARD_MODIFIER_RIGHTGUI)    queue_key_action(0xE7, 0);
    if (pressed_mods  & KEYBOARD_MODIFIER_RIGHTGUI)    queue_key_action(0xE7, 1);

    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode))
            queue_key_action(keycode, 0);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode))
            queue_key_action(keycode, 1);
    }
}

static void process_mouse_report(hid_mouse_report_t const *report) {
    cumulative_dx += report->x;
    cumulative_dy += report->y;
    cumulative_wheel += report->wheel;
    current_buttons = report->buttons & 0x07;
    mouse_has_update = 1;
}

static void process_generic_report(uint8_t dev_addr, uint8_t instance,
                                   uint8_t const *report, uint16_t len) {
    (void)dev_addr;

    uint8_t const rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        rpt_info = &rpt_info_arr[0];
    } else {
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) return;

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (rpt_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                process_kbd_report((hid_keyboard_report_t const *)report,
                                   &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
                break;
            case HID_USAGE_DESKTOP_MOUSE:
                process_mouse_report((hid_mouse_report_t const *)report);
                break;
            default:
                break;
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    printf("[USB] HID mounted: addr=%d itf=%d proto=%d (%s)\n",
           dev_addr, instance, itf_protocol,
           itf_protocol == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" :
           itf_protocol == HID_ITF_PROTOCOL_MOUSE    ? "mouse" : "other");

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
        keyboard_connected = 1;
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
        mouse_connected = 1;

    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
        keyboard_connected = 0;
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
        mouse_connected = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
            prev_kbd_report = *(hid_keyboard_report_t const *)report;
            break;
        case HID_ITF_PROTOCOL_MOUSE:
            process_mouse_report((hid_mouse_report_t const *)report);
            break;
        default:
            process_generic_report(dev_addr, instance, report, len);
            break;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void usbhid_init(void) {
#if CFG_TUH_RPI_PIO_USB
    /* Power the USB-A jacks (5V rail enable) */
    gpio_init(USB_HOST_5V_PIN);
    gpio_put(USB_HOST_5V_PIN, 1);
    gpio_set_dir(USB_HOST_5V_PIN, GPIO_OUT);

    /* Pico-PIO-USB gets PIO0 and PIO1 EXCLUSIVELY, in its default
     * configuration (tx=PIO0/SM0, rx=PIO1/SM0, eop=PIO1/SM1). Sharing
     * PIOs with it does not work: it force-loads TX at offset 0 and
     * rewrites it every SOF frame, and TX+RX programs total 47
     * instructions — more than one 32-slot PIO holds. FRANKOS
     * peripherals live on PIO2 (I2S audio + netcard UART); PS/2 is not
     * initialized on this board. NOTE these values are also baked as
     * PIO_USB_*_DEFAULT compile defines (drivers/usbhid/CMakeLists.txt)
     * because stdio_init_all()'s tusb_init() starts the host with the
     * default config before this tuh_configure can run. */
    static pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp     = USB_HOST_DP_PIN;   /* D+ = GPIO 1, D- = D+ + 1 */
    pio_cfg.pio_tx_num = 0;
    pio_cfg.sm_tx      = 0;
    pio_cfg.pio_rx_num = 1;
    pio_cfg.sm_rx      = 0;
    pio_cfg.sm_eop     = 1;
    /* pio_usb claims the channel itself (dma_claim_mask) — find a free
     * one, then unclaim so its claim doesn't double-claim and assert. */
    pio_cfg.tx_ch      = dma_claim_unused_channel(true);
    dma_channel_unclaim(pio_cfg.tx_ch);
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION,
                  &pio_cfg);
#endif
    tuh_init(BOARD_TUH_RHPORT);

#if CFG_TUH_RPI_PIO_USB
    /* stdio_init_all()'s tusb_init() started the PIO-USB host early in
     * boot and computed its bit-rate dividers from the clock at that
     * moment — but DispHSTX re-clocks the system to the video rate
     * during display init, leaving those dividers stale (TX off the USB
     * bit rate, undecodable by any device). Recompute them against the
     * final clock; pio_usb re-applies them to the SMs on every attach. */
    {
        extern pio_port_t pio_port[1];
        pio_port_t *pp = &pio_port[0];
        float cpu = (float)clock_get_hz(clk_sys);
        #define FRANK_SET_DIV(field, hz) do {                          \
            float _d = cpu / (float)(hz);                              \
            pp->field.div_int  = (uint16_t)_d;                         \
            pp->field.div_frac = (uint8_t)((_d - (uint16_t)_d) * 256); \
        } while (0)
        FRANK_SET_DIV(clk_div_fs_tx, 48000000);
        FRANK_SET_DIV(clk_div_ls_tx, 6000000);
        FRANK_SET_DIV(clk_div_fs_rx, 96000000);
        FRANK_SET_DIV(clk_div_ls_rx, 12000000);
        #undef FRANK_SET_DIV
    }
#endif

    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    current_buttons = 0;
    mouse_has_update = 0;
    key_action_head = 0;
    key_action_tail = 0;
}

#if CFG_TUH_RPI_PIO_USB
/* Power-cycle the hub to force a fresh attach and clean enumeration.
 * Call only once the system is fully booted and quiet. */
void usbhid_start_enumeration(void) {
    printf("[USB] boot settled — power-cycling hub for enumeration\n");
    stdio_flush();
    gpio_put(USB_HOST_5V_PIN, 0);
    sleep_ms(300);
    gpio_put(USB_HOST_5V_PIN, 1);
}
#endif

void usbhid_task(void) {
    tuh_task();
}

int usbhid_keyboard_connected(void) {
    return keyboard_connected;
}

int usbhid_mouse_connected(void) {
    return mouse_connected;
}

void usbhid_get_mouse_state(usbhid_mouse_state_t *state) {
    if (!state) return;
    state->dx = cumulative_dx;
    state->dy = cumulative_dy;
    state->wheel = cumulative_wheel;
    state->buttons = current_buttons;
    state->has_motion = mouse_has_update;

    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    mouse_has_update = 0;
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail)
        return 0;
    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

#endif
