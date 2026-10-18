/*
 * Peripheral side: poll VBUS and expose it as a readable, notifying GATT characteristic.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>

#include <split_usb_power.h>

LOG_MODULE_REGISTER(split_usb_power, CONFIG_ZMK_LOG_LEVEL);

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SPLIT_USB_POWER_SERVICE_UUID);
static struct bt_uuid_128 char_uuid = BT_UUID_INIT_128(SPLIT_USB_POWER_CHAR_UUID);

static uint8_t powered;
static bool notify_enabled;
static bool force_notify;

static ssize_t read_powered(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &powered, sizeof(powered));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

// The central relies on the CCC directly following the value (see central.c)
BT_GATT_SERVICE_DEFINE(split_usb_power_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&char_uuid.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, read_powered, NULL, NULL),
                       BT_GATT_CCC(ccc_changed,
                                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void poll_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_cb);

static void poll_work_cb(struct k_work *work) {
    uint8_t now = nrf_power_usbregstatus_vbusdet_get(NRF_POWER) ? 1 : 0;

    if (now != powered || force_notify) {
        powered = now;
        force_notify = false;
        if (notify_enabled) {
            int err = bt_gatt_notify(NULL, &split_usb_power_svc.attrs[2], &powered,
                                     sizeof(powered));
            if (err < 0 && err != -ENOTCONN) {
                LOG_WRN("USB power notify failed (%d)", err);
            }
        }
    }

    k_work_schedule(&poll_work, K_MSEC(CONFIG_ZMK_SPLIT_USB_POWER_POLL_MS));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    notify_enabled = value & BT_GATT_CCC_NOTIFY;
    if (notify_enabled) {
        force_notify = true;
        k_work_reschedule(&poll_work, K_NO_WAIT);
    }
}

static int split_usb_power_init(void) {
    k_work_schedule(&poll_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(split_usb_power_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
