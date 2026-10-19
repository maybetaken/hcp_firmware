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
#include "blufi/blufi_security.h"

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
/* Deferred (re)start of advertising after a disconnect, with bounded retries
 * because bt_le_adv_start can transiently fail with -ENOMEM during teardown. */
static struct k_work_delayable adv_restart_work;
static uint8_t g_adv_retries;
/* BluFi outbound frame sequence number. The client validates that the seq of
 * frames it receives increments strictly from 0 per connection (esp blufi
 * send_seq++), so we must too; reset on each new connection. */
static uint8_t g_send_seq;
/* Negotiated security mode (from CTRL SET_SEC_MODE). esp masks:
 * DATA check=0x01, DATA enc=0x02, CTRL check=0x10, CTRL enc=0x20.
 * Default 0 -> plaintext, un-checksummed (matches the current working flow). */
static uint8_t g_sec_mode;
/* Expected inbound frame seq (esp validates strictly); reset per connection. */
static uint8_t g_recv_seq;
/* Inbound fragment reassembly (esp blufi_env.aggr_buf): large frames (the DH
 * negotiate payload) arrive FC_FRAG-chunked. Pure logic lives in frame.c. */
static blufi_reasm g_reasm;

static K_SEM_DEFINE(bt_enabled_sem, 0, 1);

static void bt_ready(int err)
{
	if (err == 0) {
		k_sem_give(&bt_enabled_sem);
	} else {
		LOG_ERR("bt enable failed (%d)", err);
	}
}

/* Handle one fully-assembled (post-reassembly) BluFi frame. */
static void handle_decoded(uint8_t type, uint8_t sub, const uint8_t *data,
			   uint16_t len)
{
	if (type == BLUFI_TYPE_CTRL && sub == BLUFI_CTRL_SET_SEC_MODE && len >= 1) {
		g_sec_mode = data[0];
		LOG_INF("blufi: set sec_mode=0x%02x", g_sec_mode);
	}
#if defined(CONFIG_BLUFI_SECURITY)
	/* Key negotiation handled at the wire layer: run DH, reply device pubkey. */
	if (type == BLUFI_TYPE_DATA && sub == BLUFI_DATA_NEG) {
		const uint8_t *out = NULL;
		uint16_t out_len = 0;
		if (blufi_negotiate(data, len, &out, &out_len) == 0 && out_len > 0) {
			blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_NEG, out, out_len);
		}
		return;
	}
#endif
	if (g_cb) {
		g_cb(type, sub, data, len, g_user);
	}
}

static ssize_t p2e_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	LOG_HEXDUMP_INF(buf, len, "blufi P2E rx");

	if (len < BLUFI_HDR_LEN) {
		LOG_WRN("rx %u bytes: too short", len);
		return len;
	}
	const uint8_t *b = buf;
	uint8_t type = BLUFI_GET_TYPE(b[0]);
	uint8_t sub = BLUFI_GET_SUBTYPE(b[0]);
	uint8_t fc = b[1];
	uint8_t seq = b[2];

	/* esp validates recv_seq strictly; log a mismatch but still accept so we
	 * never regress the working plaintext flow. */
	if (seq != g_recv_seq) {
		LOG_WRN("rx seq %u != expected %u", seq, g_recv_seq);
	}
	g_recv_seq = (uint8_t)(seq + 1);

	if (fc & BLUFI_FC_ENC) {
		/* Non-NEG encrypted payload: AES-128-CFB128 decrypt in place before
		 * checksum verify (sender: checksum-then-encrypt). */
#if defined(CONFIG_BLUFI_SECURITY)
		if (!(type == BLUFI_TYPE_DATA && sub == BLUFI_DATA_NEG) &&
		    blufi_aes_decrypt(seq, (uint8_t *)b + BLUFI_HDR_LEN,
				      b[3]) < 0) {
			LOG_WRN("rx decrypt failed, dropping");
			return len;
		}
#else
		LOG_WRN("rx encrypted frame but security disabled; ignoring");
		return len;
#endif
	}
	if ((fc & BLUFI_FC_CHECK) &&
	    blufi_frame_verify_checksum(b, len) != 0) {
		LOG_WRN("rx checksum error, dropping frame");
		return len;
	}

	struct blufi_frame f;
	if (blufi_frame_parse(b, len, &f) != 0) {
		LOG_WRN("rx %u bytes (not a complete/valid frame yet)", len);
		return len;
	}
	LOG_INF("rx frame: %s subtype=0x%02x seq=%u len=%u%s",
		type == BLUFI_TYPE_CTRL ? "CTRL" : "DATA", sub, seq, f.len,
		(f.fc & BLUFI_FC_FRAG) ? " FRAG" : "");

	uint8_t out_type;
	uint8_t *disp;
	uint16_t disp_len;
	int rr = blufi_reasm_feed(&g_reasm, f.type_subtype, f.fc, f.data, f.len,
				  &out_type, &disp, &disp_len);
	if (rr == 0) {
		return len; /* mid-fragment, waiting for more */
	}
	if (rr < 0) {
		LOG_WRN("rx reassembly error %d, dropping", rr);
		return len;
	}
	handle_decoded(BLUFI_GET_TYPE(out_type), BLUFI_GET_SUBTYPE(out_type), disp,
		       disp_len);
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

/* Static GATT service (canonical Zephyr path, used by every BLE sample).
 * BT_GATT_SERVICE_DEFINE puts the service in the _bt_gatt_service_static
 * iterable section; handles are assigned by the stack at bt_enable time, the
 * attr table stays const in flash, and discovery/ATT reads never write back
 * into it. (The earlier dynamic bt_gatt_service_register path kept crashing
 * with an illegal-instruction on the first client Read By Group Type.) */
BT_GATT_SERVICE_DEFINE(blufi_svc,
	BT_GATT_PRIMARY_SERVICE(BLUFI_SVC_UUID),
	BT_GATT_CHARACTERISTIC(BLUFI_P2E_UUID,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, p2e_write, NULL),
	BT_GATT_CHARACTERISTIC(BLUFI_E2P_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_e2p, NULL, e2p_val),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* attr index of the E2P value characteristic (for bt_gatt_notify): 0=svc,
 * 1=p2e decl, 2=p2e val, 3=e2p decl, 4=e2p val. */
#define BLUFI_E2P_ATTR (&blufi_svc.attrs[4])

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

#define BLUFI_ADV_MAX_RETRIES 5

static void adv_restart_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (g_adv_running) {
		return;  /* already advertising */
	}
	adv_start();
	if (!g_adv_running && g_adv_retries < BLUFI_ADV_MAX_RETRIES) {
		g_adv_retries++;
		k_work_reschedule(&adv_restart_work, K_MSEC(500));
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connect failed (%u)", err);
		return;
	}
	/* Advertising stopped once we connected; clear the flag so a later
	 * disconnect re-advertises. */
	g_adv_running = false;
	LOG_INF("phone connected");
	g_send_seq = 0;
	g_recv_seq = 0;
	blufi_reasm_reset(&g_reasm);
#if defined(CONFIG_BLUFI_SECURITY)
	/* Fresh DH/PSK state per connection. */
	blufi_security_deinit();
	blufi_security_init();
#endif
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("phone disconnected (%u), re-advertising", reason);
	g_sec_mode = 0;
	g_recv_seq = 0;
#if defined(CONFIG_BLUFI_SECURITY)
	blufi_security_deinit();
#endif
	/* Do NOT call bt_le_adv_start() synchronously here: the link/adv buffers
	 * are still being torn down and it fails with -ENOMEM. Defer to the
	 * system workqueue with a small delay and retry until it takes. */
	g_adv_retries = 0;
	k_work_reschedule(&adv_restart_work, K_MSEC(100));
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

static uint8_t attr_dump_cb(const struct bt_gatt_attr *attr, uint16_t handle,
			    void *user_data)
{
	uint16_t *count = user_data;
	bool is_primary = (attr->uuid != NULL &&
			   bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) == 0);

	(*count)++;
	LOG_INF("db attr handle=0x%04x perm=0x%02x%s", handle, attr->perm,
		is_primary ? "  <== PRIMARY SERVICE" : "");
	return BT_GATT_ITER_CONTINUE;
}

int blufi_start(void)
{
	k_work_init_delayable(&adv_restart_work, adv_restart_fn);
	int err = bt_enable(bt_ready);
	if (err && err != -EALREADY) {
		return err;
	}
	if (err != -EALREADY) {
		k_sem_take(&bt_enabled_sem, K_SECONDS(10));
	}
	bt_set_name(BLUFI_ADV_NAME);
	/* Enumerate the DB through the SAME path ATT discovery uses
	 * (foreach_attr_type covers static + dynamic services). Our BluFi
	 * PRIMARY SERVICE should appear here with a valid handle. */
	uint16_t db_count = 0;
	bt_gatt_foreach_attr_type(0x0001, 0xffff, NULL, NULL, 0,
				  attr_dump_cb, &db_count);
	LOG_INF("gatt db total attrs visible to discovery = %u", db_count);
	adv_start();
	return 0;
}

int blufi_send(uint8_t type, uint8_t subtype, const uint8_t *data, uint16_t len)
{
	/* esp btc_blufi_send_encap: a payload larger than one frame is split into
	 * FC_FRAG chunks (each with a 2-byte "remaining length" prefix), the final
	 * chunk unflagged. Each frame gets its own seq, checksum, and encryption. */
	uint16_t off = 0;
	uint8_t body[BLUFI_MAX_DATA_LEN];
	int ret = 0;
	int r;

	do {
		uint16_t blen;
		bool is_frag;
		r = blufi_next_fragment(data, len, &off, BLUFI_MAX_DATA_LEN, body,
				  &blen, &is_frag);
		if (r < 0) {
			return r;
		}

		int n = blufi_frame_build(type, subtype, g_send_seq, body, blen,
					  e2p_val, sizeof(e2p_val));
		if (n < 0) {
			return n;
		}
		uint8_t used_seq = g_send_seq;
		g_send_seq = (uint8_t)(g_send_seq + 1);
		e2p_val[1] |= BLUFI_FC_DIR_E2P;
		if (is_frag) {
			e2p_val[1] |= BLUFI_FC_FRAG;
		}

		bool cksum =
			(type == BLUFI_TYPE_CTRL &&
			 (g_sec_mode & BLUFI_CTRL_SEC_MODE_CHECK_MASK)) ||
			(type == BLUFI_TYPE_DATA && subtype != BLUFI_DATA_NEG &&
			 subtype != BLUFI_DATA_ERROR_INFO &&
			 (g_sec_mode & BLUFI_DATA_SEC_MODE_CHECK_MASK));
		if (cksum) {
			int t = blufi_frame_append_checksum(e2p_val, n,
							    sizeof(e2p_val));
			if (t < 0) {
				return t;
			}
			n = t;
		}
#if defined(CONFIG_BLUFI_SECURITY)
		if (type == BLUFI_TYPE_DATA && subtype != BLUFI_DATA_NEG &&
		    subtype != BLUFI_DATA_ERROR_INFO &&
		    (g_sec_mode & BLUFI_DATA_SEC_MODE_ENC_MASK)) {
			if (blufi_aes_encrypt(used_seq, e2p_val + BLUFI_HDR_LEN,
					      (int)blen) < 0) {
				return -1;
			}
			e2p_val[1] |= BLUFI_FC_ENC;
		}
#endif
		e2p_len = n;
		ret = bt_gatt_notify(NULL, BLUFI_E2P_ATTR, e2p_val, n);
		LOG_INF("tx type=%u sub=0x%02x seq=%u len=%u%s notify_ret=%d", type,
			subtype, used_seq, n, is_frag ? " FRAG" : "", ret);
		if (ret) {
			LOG_WRN("notify failed (%d)", ret);
			return ret;
		}
	} while (r == 0);

	return ret;
}

#endif /* CONFIG_BLUFI && CONFIG_BT_PERIPHERAL */
