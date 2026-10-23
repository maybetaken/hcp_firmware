/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief Public API of the ota_client firmware update library.
 *
 * The library owns the *policy* of an update (should we take this image, when
 * is it considered installed, how is progress reported, how is it cancelled)
 * and knows nothing about transports or bootloaders. Those two ends are
 * described by operation tables:
 *
 * - a @ref ota_client_source produces image bytes (HTTP, CoAP, BLE, USB, ...),
 * - a @ref ota_client_sink consumes them and hands activation to the
 *   bootloader (MCUboot, ...).
 *
 * Both are provided by the application, so nothing here depends on a board,
 * an application or a vendor SDK. Typical use:
 *
 * @code{.c}
 * static struct ota_client client;
 * static struct ota_client_http http;              // src/ota_source_http.c
 * static struct ota_client_mcuboot sink;
 *
 * ota_client_mcuboot_init(&sink);
 * // A sink-only client answers the bootloader's questions from the first second
 * // of boot, long before any update URL exists:
 * ota_client_init(&client, &(struct ota_client_config){
 *         .sink = &sink.sink,
 *         .running_version = CONFIG_APP_VERSION,
 * });
 *
 * // Later, when a server is known:
 * ota_client_http_init(&http, "http://192.0.2.5:9998/version.txt",
 *                          "http://192.0.2.5:9998/firmware.bin");
 * ota_client_init(&client, &(struct ota_client_config){
 *         .source = &http.source,
 *         .sink = &sink.sink,
 *         .running_version = CONFIG_APP_VERSION,
 * });
 *
 * if (ota_client_check(&client) == 0) {
 *         ota_client_update(&client);   // or ota_client_start() with a thread
 * }
 * // ...and once the device is known healthy:
 * if (ota_client_pending_image(&client)) {
 *         ota_client_confirm(&client);
 * }
 * @endcode
 *
 * All functions return negative errno on failure. `ota_client_check()` and
 * `ota_client_update()` use `-EALREADY` for "nothing to do" (the offered image
 * is not newer), which is a normal outcome rather than an error.
 *
 * @ingroup ota_client
 */

#ifndef OTA_CLIENT_H_
#define OTA_CLIENT_H_

#include <ota_client/ota_util.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Update lifecycle, see ota_client_state(). */
enum ota_client_state {
	/** Nothing in flight. */
	OTA_CLIENT_IDLE,
	/** Querying the source for the offered version. */
	OTA_CLIENT_CHECKING,
	/** Downloading and writing the image. */
	OTA_CLIENT_UPDATING,
	/** Image fully written and activated; reboot pending. */
	OTA_CLIENT_PENDING_REBOOT,
	/** Last operation failed; ota_client_error() carries the errno. */
	OTA_CLIENT_ERROR,
};

struct ota_client;
struct ota_client_source;
struct ota_client_sink;

/**
 * @brief Progress notification.
 *
 * Called from the thread running the update, possibly many times per second.
 * Must not block. @p total is -1 when the source cannot tell the size upfront.
 */
typedef void (*ota_client_progress_cb)(const struct ota_client *client,
				       uint32_t received, int64_t total,
				       void *user_data);

/** @brief Image producer. */
struct ota_client_source_api {
	/**
	 * Read the version of the image this source offers.
	 *
	 * @param src Source being queried.
	 * @param version Buffer of CONFIG_OTA_CLIENT_VERSION_MAX_LEN bytes.
	 * @param size Size of @p version.
	 *
	 * @return 0 on success, -EALREADY if the source is unreachable and a
	 * retry is pointless, another negative errno otherwise.
	 */
	int (*get_version)(struct ota_client_source *src, char *version,
			   size_t size);

	/**
	 * Report the size of the image, if known.
	 *
	 * @return 0 and @p *total filled in, or -ENOSYS when unknown.
	 */
	int (*get_size)(struct ota_client_source *src, int64_t *total);

	/**
	 * Transfer the image into @p sink.
	 *
	 * The implementation calls @c sink->api->write() repeatedly and must not
	 * call finish/activate itself - the core does that once the transfer
	 * reports success. It should poll ota_client_aborted() on the way so an
	 * update can be interrupted, and may call ota_client_set_total() once
	 * the size becomes known.
	 *
	 * @retval 0 whole image transferred.
	 * @retval -ECANCELED the update was aborted.
	 */
	int (*fetch)(struct ota_client_source *src, struct ota_client *client,
		     struct ota_client_sink *sink);
};

/** @brief Image producer instance (application-owned, must outlive the client). */
struct ota_client_source {
	const struct ota_client_source_api *api;
	void *user_data;
};

/** @brief Image consumer and bootloader contact. */
struct ota_client_sink_api {
	/**
	 * Prepare the storage for an incoming image.
	 *
	 * @param client Reporting client, for ota_client_report_progress().
	 * @param total Expected size in bytes, -1 when unknown.
	 */
	int (*open)(struct ota_client_sink *sink, struct ota_client *client,
		    int64_t total);
	/** Append bytes to the staged image. Called from the transfer thread. */
	int (*write)(struct ota_client_sink *sink, const void *data, size_t len);
	/** Flush, and integrity-check what was written if the backend can. */
	int (*finish)(struct ota_client_sink *sink);
	/** Ask the bootloader to boot the staged image on the next reset. */
	int (*activate)(struct ota_client_sink *sink, bool permanent);
	/** Give up on the staged image; leaves the running one untouched. */
	void (*cancel)(struct ota_client_sink *sink);
	/**
	 * True while the running image is still a trial, i.e. the bootloader will go
	 * back to the previous image on the next reset unless it is confirmed. "False"
	 * must mean "there is nothing to confirm", never "I could not tell".
	 */
	int (*is_pending)(const struct ota_client_sink *sink, bool *pending);
	/** Mark the running image as good so the bootloader keeps it. */
	int (*confirm)(struct ota_client_sink *sink);
};

/** @brief Image consumer instance (application-owned). */
struct ota_client_sink {
	const struct ota_client_sink_api *api;
	void *user_data;
};

/** @brief Update policy and endpoints, passed once to ota_client_init(). */
struct ota_client_config {
	/**
	 * NULL is allowed and means "a queries-only client": pending/confirm and the
	 * accessors still work, while check/update fail with -EINVAL. That is the
	 * useful shape at boot, before any server has been discovered - "is the image
	 * I am running still a trial?" must be answerable without an image URL.
	 */
	struct ota_client_source *source;
	struct ota_client_sink *sink;
	/** Version of the firmware now running, "major.minor.patch". */
	const char *running_version;
	/**
	 * True to keep the new image across later resets (MCUboot "permanent"
	 * swap). False - the usual choice - lets the bootloader roll back to the
	 * running image if the new one never confirms itself.
	 */
	bool permanent_upgrade;
	ota_client_progress_cb progress_cb;
	void *progress_user;
};

/**
 * @brief Library state.
 *
 * Exposed so applications can allocate it statically; the fields are private
 * to the library and read through the accessors below.
 */
struct ota_client {
	struct ota_client_config cfg;
	enum ota_client_state state;
	int error;
	uint32_t received;
	int64_t total;
	char remote_version[CONFIG_OTA_CLIENT_VERSION_MAX_LEN];
	/* Written from any context (abort request), read by the update thread. */
	atomic_t abort;
	/* Non-zero while a check or update owns the endpoints. */
	atomic_t busy;
	void *platform;
};

/* ---- core ---- */

/**
 * @brief Bind a client to a source, a sink and a policy.
 *
 * @param client Caller-owned storage, zeroed by this call.
 * @param cfg    Configuration; copied, but the endpoints it points at must
 *               stay alive for as long as the client is used. The source may be
 *               NULL (see ota_client_config).
 *
 * @retval 0 Success.
 * @retval -EINVAL Missing sink, or no running version to compare against.
 */
int ota_client_init(struct ota_client *client,
		    const struct ota_client_config *cfg);

/** @brief Ask the source what it offers and compare with the running version.
 *
 * @retval 0 A newer image is available.
 * @retval -EALREADY Up to date (or older, unless downgrades are allowed).
 * @retval -EINVAL No source was configured.
 * @retval <0 Failure, also reflected in ota_client_error().
 */
int ota_client_check(struct ota_client *client);

/**
 * @brief Run a full update in the caller's context (blocking).
 *
 * Checks, transfers, finishes and activates. Safe to call from a work queue or
 * a dedicated thread; it is what ota_client_start() runs internally.
 *
 * @retval 0 New image staged and activation requested.
 * @retval -EALREADY Nothing newer to install.
 * @retval -ECANCELED Aborted through ota_client_abort().
 * @retval <0 Failure; the staged image is discarded, the running one intact.
 */
int ota_client_update(struct ota_client *client);

/** @brief Request cancellation of an in-flight check or update. */
void ota_client_abort(struct ota_client *client);

/** @brief Whether cancellation was requested; sources poll this between chunks. */
bool ota_client_aborted(const struct ota_client *client);

/**
 * @brief Record the total image size; for sources that learn it mid-transfer.
 *
 * A server sending "Content-Length" after the request was issued has the size
 * later than ota_client_source_api::get_size() could report it, so it updates
 * the value here instead. Passing -1 leaves it unchanged.
 */
void ota_client_set_total(struct ota_client *client, int64_t total);

/** @brief Current lifecycle state. */
enum ota_client_state ota_client_state(const struct ota_client *client);

/** @brief Human readable state name, for logs and status topics. */
const char *ota_client_state_str(enum ota_client_state state);

/** @brief Result of the last operation (0, or the negative errno it failed with). */
int ota_client_error(const struct ota_client *client);

/** @brief Bytes written (or received) during the current or last update. */
uint32_t ota_client_received(const struct ota_client *client);

/** @brief Total image size as last reported by the source, -1 when unknown. */
int64_t ota_client_total(const struct ota_client *client);

/** @brief Version offered by the source after a successful check. */
const char *ota_client_remote_version(const struct ota_client *client);

/** @brief True while a check or update is running. */
bool ota_client_busy(const struct ota_client *client);

/**
 * @brief Wait for a background update to finish.
 *
 * Only available with CONFIG_OTA_CLIENT_THREAD.
 *
 * @param timeout_ms 0 to poll, -1 to wait forever.
 *
 * @retval 0 Update finished successfully.
 * @retval -EALREADY Still running when the timeout expired.
 * @retval <0 Update failed; the reason is also in ota_client_error().
 */
int ota_client_join(struct ota_client *client, int timeout_ms);

/**
 * @brief Start the update on the library thread.
 *
 * Only available with CONFIG_OTA_CLIENT_THREAD.
 *
 * @retval 0 Thread started, ota_client_state() now reports updating.
 * @retval -EBUSY Another update is running.
 */
int ota_client_start(struct ota_client *client);

/**
 * @brief True when the running image has not been confirmed yet.
 *
 * Works on a sink-only client: whether the bootloader still considers this image a
 * trial has nothing to do with an image URL, and the answer is needed at boot -
 * before any server has been discovered. Applications use it to decide when the
 * device is healthy enough to keep the new image (typically: an IP address *and* a
 * live service session, not merely "it booted").
 */
bool ota_client_pending_image(const struct ota_client *client);

/**
 * @brief Tell the bootloader to keep the running image.
 *
 * Works on a sink-only client.
 *
 * @retval 0 Confirmed (or nothing was pending).
 * @retval <0 Backend failure.
 */
int ota_client_confirm(struct ota_client *client);

/**
 * @brief Report transfer progress; for sink implementations.
 *
 * The sink counts the bytes it accepts, so it is the accurate source for
 * progress. Calls the configured ota_client_config::progress_cb.
 */
void ota_client_report_progress(struct ota_client *client, uint32_t received,
				int64_t total);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CLIENT_H_ */
