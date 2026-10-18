/*
 * Central side: once the split peripheral is connected and encrypted, find its USB power
 * characteristic, read it and subscribe to changes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <split_usb_power.h>

LOG_MODULE_REGISTER(split_usb_power, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_split_peripheral_usb_power_changed);

// Give ZMK's own split service discovery a head start on a new connection
#define DISCOVER_DELAY K_SECONDS(2)

static struct bt_uuid_128 char_uuid = BT_UUID_INIT_128(SPLIT_USB_POWER_CHAR_UUID);

static struct bt_conn *peripheral_conn;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_read_params read_params;

static atomic_t powered;

bool split_usb_power_peripheral_powered(void) { return atomic_get(&powered); }

// Raise from the system work queue rather than the BT RX thread
static void raise_work_cb(struct k_work *work) {
    raise_zmk_split_peripheral_usb_power_changed(
        (struct zmk_split_peripheral_usb_power_changed){.powered = atomic_get(&powered)});
}
static K_WORK_DEFINE(raise_work, raise_work_cb);

static void set_powered(bool value) {
    if (atomic_set(&powered, value) != value) {
        k_work_submit(&raise_work);
    }
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length) {
    if (data == NULL) { // unsubscribed
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    if (length >= 1) {
        set_powered(((const uint8_t *)data)[0]);
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                       const void *data, uint16_t length) {
    if (err == 0 && data != NULL && length >= 1) {
        set_powered(((const uint8_t *)data)[0]);
    }
    return BT_GATT_ITER_STOP;
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        LOG_WRN("Peripheral has no USB power characteristic");
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;

    subscribe_params.notify = notify_cb;
    subscribe_params.value = BT_GATT_CCC_NOTIFY;
    subscribe_params.value_handle = chrc->value_handle;
    subscribe_params.ccc_handle = chrc->value_handle + 1; // CCC follows the value
    atomic_set_bit(subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    int err = bt_gatt_subscribe(conn, &subscribe_params);
    if (err < 0 && err != -EALREADY) {
        LOG_WRN("USB power subscribe failed (%d)", err);
    }

    read_params.func = read_cb;
    read_params.handle_count = 1;
    read_params.single.handle = chrc->value_handle;
    read_params.single.offset = 0;
    err = bt_gatt_read(conn, &read_params);
    if (err < 0) {
        LOG_WRN("USB power read failed (%d)", err);
    }

    return BT_GATT_ITER_STOP;
}

static void discover_work_cb(struct k_work *work) {
    if (peripheral_conn == NULL) {
        return;
    }

    discover_params.uuid = &char_uuid.uuid;
    discover_params.func = discover_cb;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(peripheral_conn, &discover_params);
    if (err < 0) {
        LOG_WRN("USB power discovery failed (%d)", err);
    }
}
static K_WORK_DELAYABLE_DEFINE(discover_work, discover_work_cb);

static bool is_split_peripheral(struct bt_conn *conn) {
    struct bt_conn_info info;
    // We are the BLE central only towards the split peripheral; hosts connect to us
    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err) {
    if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2 || !is_split_peripheral(conn)) {
        return;
    }

    if (peripheral_conn == NULL) {
        peripheral_conn = bt_conn_ref(conn);
    }
    k_work_reschedule(&discover_work, DISCOVER_DELAY);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn != peripheral_conn) {
        return;
    }

    k_work_cancel_delayable(&discover_work);
    bt_conn_unref(peripheral_conn);
    peripheral_conn = NULL;
    subscribe_params.value_handle = 0;
    set_powered(false);
}

BT_CONN_CB_DEFINE(split_usb_power_conn_cb) = {
    .security_changed = security_changed,
    .disconnected = disconnected,
};
