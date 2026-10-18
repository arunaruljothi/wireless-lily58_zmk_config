/*
 * Peripheral USB power state, shared with the central over split BLE.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <zephyr/bluetooth/uuid.h>
#include <zmk/event_manager.h>

#define SPLIT_USB_POWER_SERVICE_UUID                                                               \
    BT_UUID_128_ENCODE(0x5b1c7a40, 0x2f6e, 0x4d3a, 0x9c1b, 0x7e4f0a2d6c10)
#define SPLIT_USB_POWER_CHAR_UUID                                                                  \
    BT_UUID_128_ENCODE(0x5b1c7a40, 0x2f6e, 0x4d3a, 0x9c1b, 0x7e4f0a2d6c11)

// Raised on the central when the peripheral's USB power state changes or it disconnects
struct zmk_split_peripheral_usb_power_changed {
    bool powered;
};

ZMK_EVENT_DECLARE(zmk_split_peripheral_usb_power_changed);

// Central only: last known state (false while the peripheral is disconnected)
bool split_usb_power_peripheral_powered(void);
