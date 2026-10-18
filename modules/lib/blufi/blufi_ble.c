/*
 * BluFi device side: BLE peripheral + ESP BluFi GATT (service 0xFFFF,
 * write 0xFFF4 phone->esp, notify 0xFFF5 esp->phone). Received frames are
 * parsed and surfaced to the app callback. Phase 1: no encryption yet (we can
 * already observe the app's handshake/control frames).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(CONFIG_BLUFI) && defined(CONFIG_BT_PERIPHERAL)

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "blufi/blufi.h"

LOG_MODULE_REGISTER(blufi, CONFIG_LOG_DEFAULT_LEVEL);

/* BluFi GATT: the primary service is 16-bit 0xFFFF (esp BLUFI_APP_UUID); only
 * the *advertisement* carries the 128-bit form. Characteristics are 16-bit. */
#define BLUFI_SVC_UUID   BT_UUID_DECLARE_16(0xFFFF)
#define BLUFI_P2E_UUID BT_UUID_DECLARE_16(0xFF01) /* phone -> esp (write) */
#define BLUFI_E2P_UUID BT_UUID_DECLARE_16(0xFF02) /* esp -> phone (notify) */

/* BluFi apps (EspConfig / EspBlufi) filter on a name starting with BLUFI. */
#define BLUFI_ADV_NAME   "BLUFI_DEVICE"

static blufi_data_cb_t g_cb;
static void *g_user;
static bool g_adv_running;

static K_SEM_DEFINE(bt_enabled_sem, 0, 1);

static void bt_ready(int err)
{
	if (err == 0) {
		k_sem_give(&bt_enabled_sem);
	} else {
		LOG_ERR("bt enable failed (%d)", err);
	}
}

static ssize_t p2e_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	LOG_HEXDUMP_DBG(buf, len, "blufi P2E rx");

	struct blufi_frame f;
	if (blufi_frame_parse(buf, len, &f) == 0) {
		uint8_t type = BLUFI_GET_TYPE(f.type_subtype);
		uint8_t sub = BLUFI_GET_SUBTYPE(f.type_subtype);
		LOG_INF("rx frame: %s subtype=0x%02x seq=%u len=%u",
			type == BLUFI_TYPE_CTRL ? "CTRL" : "DATA", sub, f.seq,
			f.len);
		if (g_cb) {
			g_cb(type, sub, f.data, f.len, g_user);
		}
	} else {
		LOG_WRN("rx %u bytes (not a complete/valid frame yet)", len);
	}
	return len;
}

/* Notify value storage for E2P. */
static uint8_t e2p_val[512];
static uint16_t e2p_len;

static ssize_t read_e2p(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, e2p_val, e2p_len);
}

/* NOTE: attrs MUST be non-const (RAM). Dynamic registration writes the
 * assigned handle back into each attr (gatt.c: attrs->handle = ++handle);
 * a `const` table lives in flash on ESP32-C6, so those writes silently fail
 * and the service registers with handle 0 -> clients can't enumerate it. */
static struct bt_gatt_attr blufi_attrs[] = {
	BT_GATT_PRIMARY_SERVICE(BLUFI_SVC_UUID),
	BT_GATT_CHARACTERISTIC(BLUFI_P2E_UUID,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, p2e_write, NULL),
	BT_GATT_CHARACTERISTIC(BLUFI_E2P_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_e2p, NULL, e2p_val),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service blufi_svc = BT_GATT_SERVICE(blufi_attrs);

/* attr index of the E2P value characteristic (for bt_gatt_notify): 0=svc,
 * 1=p2e decl, 2=p2e val, 3=e2p decl, 4=e2p val. */
#define BLUFI_E2P_ATTR (&blufi_attrs[4])

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	/* Same 128-bit service UUID as the GATT primary service (LSB-first on
	 * air, exactly like esp_blufi). */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
		      0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, BLUFI_ADV_NAME, sizeof(BLUFI_ADV_NAME) - 1),
};

static void adv_start(void)
{
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	g_adv_running = (err == 0);
	if (err) {
		LOG_ERR("adv start failed (%d)", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connect failed (%u)", err);
		return;
	}
	LOG_INF("phone connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("phone disconnected (%u), re-advertising", reason);
	if (!g_adv_running) {
		adv_start();
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

void blufi_set_callback(blufi_data_cb_t cb, void *user)
{
	g_cb = cb;
	g_user = user;
}

static bool attr_dump_cb(const struct bt_gatt_attr *attr, uint16_t handle,
			 void *user_data)
{
	ARG_UNUSED(user_data);
	if (attr->uuid != NULL &&
	    bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) == 0) {
		LOG_INF("gatt primary-service decl @handle %u", handle);
	}
	return false;
}

int blufi_start(void)
{
	int err = bt_enable(bt_ready);
	if (err && err != -EALREADY) {
		return err;
	}
	if (err != -EALREADY) {
		k_sem_take(&bt_enabled_sem, K_SECONDS(10));
	}
	bt_set_name(BLUFI_ADV_NAME);
	err = bt_gatt_service_register(&blufi_svc);
	if (err && err != -EALREADY) {
		LOG_ERR("gatt service register failed (%d)", err);
	}
	/* Runtime evidence: log the handles actually assigned to our attrs.
	 * If any is 0 the registration didn't take (see RAM note above). */
	for (size_t i = 0; i < ARRAY_SIZE(blufi_attrs); i++) {
		LOG_INF("blufi attr[%u] handle=0x%04x",
			(unsigned)i, blufi_attrs[i].handle);
	}
	bt_gatt_foreach_attr(0x0001, 0xffff,
			     (bt_gatt_attr_func_t)attr_dump_cb, NULL);
	adv_start();
	return 0;
}

int blufi_send(uint8_t type, uint8_t subtype, const uint8_t *data, uint16_t len)
{
	int n = blufi_frame_build(type, subtype, 0, data, len, e2p_val,
				  sizeof(e2p_val));
	if (n < 0) {
		return n;
	}
	e2p_len = n;
	return bt_gatt_notify(NULL, BLUFI_E2P_ATTR, e2p_val, n);
}

#endif /* CONFIG_BLUFI && CONFIG_BT_PERIPHERAL */
