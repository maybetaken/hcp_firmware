/*
 * BluFi device side: BLE peripheral + ESP BluFi GATT (service 0xFFFF,
 * write 0xFF01 phone->esp, notify 0xFF02 esp->phone). Received frames are
 * parsed and surfaced to the app callback. With CONFIG_BLUFI_SECURITY the
 * ESP-compatible DH key negotiation + AES-128-CFB128/CRC-16 layer is wired in
 * (see blufi_security.c and the FC_ENC/FC_CHECK handling in p2e_write/blufi_send).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(CONFIG_BLUFI) && defined(CONFIG_BT_PERIPHERAL)

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
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
/* Local identity index used for advertising. Set to a fresh random-static
 * identity at start so the phone treats each power-up as a new device and
 * (re)runs the BluFi negotiation instead of trusting a cached "no-encryption"
 * entry keyed on a fixed public address. */
static uint8_t g_adv_id = BT_ID_DEFAULT;
/* Current connection (for MTU-based outbound fragmentation like esp's frag_size). */
static struct bt_conn *g_conn;
/* Deferred (re)start of advertising after a disconnect, with bounded retries
 * because bt_le_adv_start can transiently fail with -ENOMEM during teardown. */
static struct k_work_delayable adv_restart_work;
static uint8_t g_adv_retries;
/* Set by blufi_stop(): keep the link down and the radio off until blufi_start()
 * runs again. The controller is powered down from a work item after the
 * disconnect has completed, because bt_disable() while a connection is still
 * being torn down is not something the stack expects. */
static bool g_want_off;
static struct k_work_delayable bt_off_work;
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

/* Completed frames are deferred to a work item instead of being handled inside
 * the GATT write callback, so the ATT Write Response goes out before any
 * notification produced while processing the request (same ordering as
 * Espressif's BTC task). Depth covers a phone sending SSID + PASSWD +
 * CONN_TO_AP back to back. */
struct blufi_rx_msg {
	uint8_t type;
	uint16_t len;
	uint8_t data[BLUFI_FRAG_MAX];
};

K_MSGQ_DEFINE(g_rx_q, sizeof(struct blufi_rx_msg), 4, 4);

/* Frames with BLUFI_FC_REQ_ACK must be answered with a CTRL/ACK frame carrying
 * the received sequence number (esp btc_blufi_send_ack); some apps block their
 * request queue until it arrives. Queued for the same ordering reason as above. */
K_MSGQ_DEFINE(g_ack_q, sizeof(uint8_t), 4, 1);

static void ack_work_fn(struct k_work *work)
{
	uint8_t seq;

	ARG_UNUSED(work);

	while (k_msgq_get(&g_ack_q, &seq, K_NO_WAIT) == 0) {
		blufi_send(BLUFI_TYPE_CTRL, BLUFI_CTRL_ACK, &seq, 1);
	}
}

K_WORK_DELAYABLE_DEFINE(g_ack_work, ack_work_fn);

static void queue_ack(uint8_t seq)
{
	if (k_msgq_put(&g_ack_q, &seq, K_NO_WAIT) != 0) {
		LOG_WRN("ack queue full, dropping ack %u", seq);
		return;
	}
	k_work_reschedule(&g_ack_work, K_MSEC(20));
}

static void handle_decoded(uint8_t type, uint8_t sub, const uint8_t *data,
			   uint16_t len);

static void rx_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_rx_work, rx_work_fn);

static void rx_work_fn(struct k_work *work)
{
	struct blufi_rx_msg msg;

	ARG_UNUSED(work);

	while (k_msgq_get(&g_rx_q, &msg, K_NO_WAIT) == 0) {
		handle_decoded(BLUFI_GET_TYPE(msg.type),
			       BLUFI_GET_SUBTYPE(msg.type), msg.data, msg.len);
	}
}

static void rx_queue_frame(uint8_t type_subtype, const uint8_t *data,
			   uint16_t len)
{
	struct blufi_rx_msg msg;

	if (len > sizeof(msg.data)) {
		LOG_WRN("rx queue: frame too large (%u)", len);
		return;
	}
	msg.type = type_subtype;
	msg.len = len;
	if (len) {
		memcpy(msg.data, data, len);
	}
	if (k_msgq_put(&g_rx_q, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("rx queue full, dropping frame");
		return;
	}
	/* Give the RX thread time to queue the ATT Write Response before this
	 * work item can emit the reply notification. */
	k_work_reschedule(&g_rx_work, K_MSEC(20));
}

static K_SEM_DEFINE(bt_enabled_sem, 0, 1);

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	LOG_INF("blufi: ATT MTU updated tx=%u rx=%u", tx, rx);
}

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = att_mtu_updated,
};

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
	LOG_HEXDUMP_DBG(buf, len, "blufi P2E rx");

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

	/* esp answers every frame/fragment that requests an ACK, after checksum
	 * verification and before fragment reassembly. */
	if (fc & BLUFI_FC_REQ_ACK) {
		queue_ack(seq);
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
	/* SET_SEC_MODE is tiny and must take effect before the peer's next
	 * (encrypted) write. Everything else is deferred so the ATT response
	 * leaves before the reply notification. */
	if (BLUFI_GET_TYPE(out_type) == BLUFI_TYPE_CTRL &&
	    BLUFI_GET_SUBTYPE(out_type) == BLUFI_CTRL_SET_SEC_MODE) {
		handle_decoded(BLUFI_GET_TYPE(out_type),
			       BLUFI_GET_SUBTYPE(out_type), disp, disp_len);
	} else {
		rx_queue_frame(out_type, disp, disp_len);
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

static void e2p_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_INF("blufi: E2P CCC changed (0x%04x)", value);
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
	BT_GATT_CCC(e2p_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
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
	param.id = g_adv_id;
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

static void bt_off_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int err = bt_disable();
	if (err) {
		/* Advertising is already stopped and g_want_off keeps it stopped, so
		 * the phone can no longer see us even if this controller build has no
		 * .disable implementation - we just do not get the RAM back. */
		LOG_WRN("bt_disable failed (%d); BLE silent but still powered", err);
	} else {
		LOG_INF("Bluetooth disabled after provisioning");
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
	g_conn = bt_conn_ref(conn);
	g_send_seq = 0;
	g_recv_seq = 0;
	blufi_reasm_reset(&g_reasm);
	k_msgq_purge(&g_rx_q);
	LOG_INF("blufi: ATT MTU now %u", bt_gatt_get_mtu(conn));
#if defined(CONFIG_BLUFI_SECURITY)
	/* Fresh DH/PSK state per connection. */
	blufi_security_deinit();
	blufi_security_init();
#endif
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("phone disconnected (%u), re-advertising", reason);
	if (g_conn) {
		bt_conn_unref(g_conn);
		g_conn = NULL;
	}
	g_sec_mode = 0;
	g_recv_seq = 0;
	k_msgq_purge(&g_rx_q);
#if defined(CONFIG_BLUFI_SECURITY)
	blufi_security_deinit();
#endif
	/* Do NOT call bt_le_adv_start() synchronously here: the link/adv buffers
	 * are still being torn down and it fails with -ENOMEM. Defer to the
	 * system workqueue with a small delay and retry until it takes. */
	if (g_want_off) {
		LOG_INF("provisioning closed; not re-advertising");
		k_work_reschedule(&bt_off_work, K_MSEC(200));
		return;
	}
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

int blufi_start(void)
{
	k_work_init_delayable(&adv_restart_work, adv_restart_fn);
	k_work_init_delayable(&bt_off_work, bt_off_fn);
	g_want_off = false;
	int err = bt_enable(bt_ready);
	if (err && err != -EALREADY) {
		return err;
	}
	if (err != -EALREADY) {
		k_sem_take(&bt_enabled_sem, K_SECONDS(10));
	}
	bt_gatt_cb_register(&gatt_cb);
	bt_set_name(BLUFI_ADV_NAME);
	/* Advertise from a fresh random-static identity so the app negotiates. Reuse
	 * it if blufi_start() runs again after a bt_disable(): the identity table
	 * survives the radio power cycle, and re-creating it would burn the next
	 * BT_ID_MAX slot (and then silently fall back to the default identity). */
	if (g_adv_id == BT_ID_DEFAULT) {
		bt_addr_le_t raddr;
		sys_csrand_get(raddr.a.val, sizeof(raddr.a.val));
		raddr.a.val[5] |= 0xC0; /* random static: top 2 bits = 11 */
		raddr.type = BT_ADDR_LE_RANDOM;
		int id = bt_id_create(&raddr, NULL);
		if (id >= 0) {
			g_adv_id = (uint8_t)id;
			LOG_INF("blufi: advertising from random identity %d", id);
		} else {
			LOG_WRN("blufi: bt_id_create failed (%d), using default", id);
		}
	} else {
		LOG_INF("blufi: reusing identity %d", g_adv_id);
	}
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

	/* Async producers (scan results, provisioning timeouts) can outlive the
	 * link; notifying without a connection just fails with -ENOTCONN. */
	if (!g_conn) {
		LOG_DBG("no connection, dropping tx (type=%u sub=0x%02x)", type,
			subtype);
		return -ENOTCONN;
	}

	/* Outbound fragmentation to the negotiated ATT MTU, like esp's
	 * btc_blufi_send_encap(): frag_size = min(att_mtu, 255) - 11 (4-byte blufi
	 * header + 2-byte frag prefix + 2-byte checksum + 3-byte ATT header), and
	 * our frag_cap is the body size, i.e. frag_size + 2. 14 bytes at the default
	 * MTU 23, 246 once the phone negotiates 527 - so a 128-byte DH public key
	 * goes out as one notification, exactly like ESP. */
	uint16_t mtu = bt_gatt_get_mtu(g_conn);
	uint16_t esp_mtu = (mtu < 255) ? mtu : 255;
	uint16_t frag_cap = (esp_mtu >= 12) ? (uint16_t)(esp_mtu - 9) : 3;

	do {
		uint16_t blen;
		bool is_frag;
		r = blufi_next_fragment(data, len, &off, frag_cap, body,
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
		/* The controller TX pool is only a few buffers deep, so a fragmented
		 * reply can transiently fail with -ENOMEM; wait for it to drain and
		 * retry rather than truncating the reply. */
		for (int tries = 0; ; tries++) {
			ret = bt_gatt_notify(NULL, BLUFI_E2P_ATTR, e2p_val, n);
			if (ret != -ENOMEM && ret != -EAGAIN) {
				break;
			}
			if (tries >= 400) {
				break;
			}
			k_sleep(K_MSEC(5));
		}
		LOG_INF("tx type=%u sub=0x%02x seq=%u len=%u%s notify_ret=%d", type,
			subtype, used_seq, n, is_frag ? " FRAG" : "", ret);
		if (type == BLUFI_TYPE_DATA && subtype == BLUFI_DATA_NEG) {
			LOG_HEXDUMP_DBG(e2p_val, n, "blufi E2P tx neg");
		}
		if (ret) {
			LOG_WRN("notify failed (%d)", ret);
			return ret;
		}
		/* Pace multi-fragment replies: iOS accepts roughly one notification
		 * per connection interval, and the pool is shallow. */
		if (r == 0) {
			k_sleep(K_MSEC(15));
		}
	} while (r == 0);

	return ret;
}

int blufi_disconnect(void)
{
	if (!g_conn) {
		return -ENOTCONN;
	}
	/* Local-initiated teardown; disconnected() drops the reference, resets the
	 * per-link state and re-advertises, so nothing else has to be cleaned here.
	 * The BluFi frame that triggered this is handled on the rx work queue, never
	 * in the ATT callback, so tearing the link down from here is safe. */
	return bt_conn_disconnect(g_conn, BT_HCI_ERR_LOCALHOST_TERM_CONN);
}

int blufi_stop(void)
{
	if (!g_want_off) {
		g_want_off = true;
		k_work_cancel_delayable(&adv_restart_work);
		if (bt_le_adv_stop() == 0) {
			g_adv_running = false;
		}
		LOG_INF("blufi stopped (advertising off)");
	}
	if (g_conn) {
		/* disconnected() finishes the job and powers the radio down. */
		return bt_conn_disconnect(g_conn, BT_HCI_ERR_LOCALHOST_TERM_CONN);
	}
	k_work_reschedule(&bt_off_work, K_MSEC(50));
	return 0;
}

#endif /* CONFIG_BLUFI && CONFIG_BT_PERIPHERAL */
