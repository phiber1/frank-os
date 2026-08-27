# FRANK OS — USB HID Host Driver
# Provides keyboard and mouse input via USB Host
#
# Enable with: cmake -DUSB_HID_ENABLED=1
# NOTE: USB CDC stdio is DISABLED when USB HID is enabled.
#
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(usbhid INTERFACE)

if(USB_HID_ENABLED)
    message(STATUS "USB HID Host enabled")
    if(NOT PICO_BOARD STREQUAL "fruitjam")
        message(STATUS "  NOTE: USB CDC stdio will be DISABLED when USB HID is enabled!")
    endif()

    target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
    )

    target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
    )

    target_compile_definitions(usbhid INTERFACE
        USB_HID_ENABLED=1
    )

    # Fruit Jam: the two USB-A host jacks sit behind PIO-USB on GPIO 1/2
    # (the native controller is the USB-C port). Requires the Pico-PIO-USB
    # library vendored in the SDK's TinyUSB (hw/mcu/raspberry_pi/Pico-PIO-USB).
    if(PICO_BOARD STREQUAL "fruitjam")
        # Link the *base* host stack (not the marked tinyusb_host wrapper):
        # the wrapper defines LIB_TINYUSB_HOST, which makes pico_stdio_usb
        # compile itself out — and we want the USB-C CDC console alongside
        # the PIO-USB host. Our tusb_config.h declares both stacks.
        target_link_libraries(usbhid INTERFACE
            tinyusb_host_base
            tinyusb_common
            tinyusb_board
        )
        if(NOT TARGET tinyusb_pico_pio_usb)
            message(FATAL_ERROR "USB HID on fruitjam needs Pico-PIO-USB: "
                "clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git into "
                "<pico-sdk>/lib/tinyusb/hw/mcu/raspberry_pi/Pico-PIO-USB")
        endif()
        message(STATUS "  Fruit Jam: USB HID via PIO-USB on GPIO 1/2 (rhport 1)")
        target_compile_definitions(usbhid INTERFACE
            CFG_TUH_RPI_PIO_USB=1
            BOARD_TUH_RHPORT=1
            # RAM is tight: 16 endpoints comfortably covers hub + kbd + mouse
            PIO_USB_EP_POOL_CNT=16
            # Bake the Fruit Jam layout into PIO_USB_DEFAULT_CONFIG itself
            # (the tuh_configure hand-off proved unreliable). TX and RX
            # must be on separate PIOs — 47 instructions total.
            PIO_USB_DP_PIN_DEFAULT=1
            PIO_USB_RX_DEFAULT=1
            PIO_SM_USB_RX_DEFAULT=0
            PIO_SM_USB_EOP_DEFAULT=1
            PIO_USB_DMA_TX_DEFAULT=3
        )
        target_link_libraries(usbhid INTERFACE tinyusb_pico_pio_usb)
    else()
        target_link_libraries(usbhid INTERFACE
            tinyusb_host
            tinyusb_board
        )
    endif()
else()
    message(STATUS "USB HID Host disabled (use -DUSB_HID_ENABLED=1 to enable)")
endif()
