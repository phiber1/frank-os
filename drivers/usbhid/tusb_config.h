/*
 * FRANK OS — TinyUSB Host Configuration for USB HID (Keyboard/Mouse)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * Only included when USB_HID_ENABLED is defined in CMake.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

#define BOARD_TUH_MAX_SPEED   OPT_MODE_FULL_SPEED

#define CFG_TUH_ENABLED       1

#if CFG_TUH_RPI_PIO_USB
/* Fruit Jam: host runs on PIO-USB (rhport 1), which leaves the native
 * controller (rhport 0) free to be a CDC device — the USB-C debug
 * console via pico_stdio_usb. These are the device-side settings that
 * pico_stdio_usb's own tusb_config.h would provide, but it steps back
 * when the application links the TinyUSB host stack. */
#include "pico/stdio_usb.h"
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST)
#define CFG_TUD_ENABLED       1
#define CFG_TUD_CDC           1
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64
/* stdio_usb's reset interface (picotool reboot) — mirror its default */
#if !PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR
#define CFG_TUD_VENDOR            (0)
#else
#define CFG_TUD_VENDOR            (1)
#define CFG_TUD_VENDOR_RX_BUFSIZE (256)
#define CFG_TUD_VENDOR_TX_BUFSIZE (256)
#endif
#else
/* M2: host owns the native controller — no device stack. */
#define CFG_TUD_ENABLED       0
#endif

#define CFG_TUH_MAX_SPEED     BOARD_TUH_MAX_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_DEVICE_MAX   4   /* hub + keyboard + mouse + one spare */
#define CFG_TUH_HUB          1
#define CFG_TUH_HID          4   /* HID interfaces (multimedia kbds use 2) */

#define CFG_TUH_CDC          0
#define CFG_TUH_VENDOR       0
#define CFG_TUH_MSC          0

#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif
