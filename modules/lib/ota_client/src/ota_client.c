/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief ota_client core: the update policy and lifecycle.
 *
 * Knows when an image should be taken, in which order the endpoints are
 * driven, how failures leave the system, and how a running transfer is
 * cancelled. It never touches sockets or flash itself.
 */

#include <ota_client/ota_client.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ota_client, CONFIG_OTA_CLIENT_LOG_LEVEL);

/* Only one check or update may own the endpoints at a time; busy is acquired with
 * a compare-and-set so abort() and the accessors stay lock free. Note that
 * Zephyr's atomic_cas() returns true when the store happened (not the previous
 * value like Linux's cmpxchg), so "already held" is the false case. */
static int client_lock(struct ota_client *client)
{
	if (!atomic_cas(&client->busy, 0, 1)) {
		/* Worth a log line: without it a caller only sees -EBUSY and no hint
		 * about who still owns the endpoints. */
		LOG_ERR("busy: state %s, %u of %lld bytes",
			ota_client_state_str(client->state), client->received,
			(long long)client->total);
		return -EBUSY;
	}

	atomic_set(&client->abort, 0);
	return 0;
}

static void client_unlock(struct ota_client *client)
{
	atomic_set(&client->busy, 0);
}

static bool abort_requested(const struct ota_client *client)
{
	return atomic_get(&client->abort) != 0;
}

static void finish(struct ota_client *client, int ret)
{
	client->error = ret;

	switch (ret) {
	case 0:
		client->state = OTA_CLIENT_IDLE;
		break;
	case -EALREADY:
	case -ECANCELED:
		client->state = OTA_CLIENT_IDLE;
		break;
	default:
		client->state = OTA_CLIENT_ERROR;
		break;
	}
}

int ota_client_init(struct ota_client *client, const struct ota_client_config *cfg)
{
	if (client == NULL || cfg == NULL || cfg->sink == NULL ||
	    cfg->sink->api == NULL) {
		return -EINVAL;
	}

	if (cfg->source != NULL && cfg->source->api == NULL) {
		return -EINVAL;
	}

	/* An unusable running version would make "is this newer?" a guess, and
	 * that is the one decision that must never be guessed. */
	if (cfg->running_version == NULL ||
	    ota_client_version_cmp(cfg->running_version,
				   cfg->running_version) == -EINVAL) {
		return -EINVAL;
	}

	memset(client, 0, sizeof(*client));
	client->cfg = *cfg;
	client->state = OTA_CLIENT_IDLE;
	client->total = -1;

	return 0;
}

static int check_locked(struct ota_client *client)
{
	struct ota_client_source *source = client->cfg.source;
	char version[CONFIG_OTA_CLIENT_VERSION_MAX_LEN];
	int cmp;
	int ret;

	client->state = OTA_CLIENT_CHECKING;

	ret = source->api->get_version(source, version, sizeof(version));
	if (ret != 0) {
		goto out;
	}

	cmp = ota_client_version_cmp(version, client->cfg.running_version);
	if (cmp == -EINVAL) {
		LOG_ERR("source offered an unparsable version \"%s\"", version);
		ret = -EINVAL;
		goto out;
	}

	if (cmp > 0) {
		/* Newer than what we run: take it. */
		ret = 0;
	} else if (cmp < 0 && IS_ENABLED(CONFIG_OTA_CLIENT_ALLOW_DOWNGRADE)) {
		/* Downgrade requests are explicit, so any difference is a go. */
		ret = 0;
	} else {
		LOG_INF("up to date (running %s, offered %s)",
			client->cfg.running_version, version);
		ret = -EALREADY;
	}

	if (ret == 0) {
		strncpy(client->remote_version, version,
			sizeof(client->remote_version) - 1);
		client->remote_version[sizeof(client->remote_version) - 1] = '\0';
		LOG_INF("update available: %s -> %s", client->cfg.running_version,
			version);
	}

out:
	finish(client, ret);
	if (ret == 0) {
		/* check succeeded: a newer image is available, nothing staged. */
		client->state = OTA_CLIENT_IDLE;
	}
	return ret;
}

int ota_client_check(struct ota_client *client)
{
	int ret;

	if (client == NULL || client->cfg.source == NULL) {
		/* Queries-only client: no server to ask. */
		return -EINVAL;
	}

	ret = client_lock(client);
	if (ret != 0) {
		return ret;
	}

	ret = check_locked(client);
	client_unlock(client);

	return ret;
}

static int update_locked(struct ota_client *client)
{
	struct ota_client_source *source = client->cfg.source;
	struct ota_client_sink *sink = client->cfg.sink;
	int64_t total = -1;
	int ret;

	ret = check_locked(client);
	if (ret != 0) {
		return ret; /* -EALREADY is a normal, logged outcome */
	}

	client->state = OTA_CLIENT_UPDATING;
	client->received = 0;
	client->total = -1;

	if (source->api->get_size != NULL &&
	    source->api->get_size(source, &total) == 0) {
		client->total = total;
	}

	ret = sink->api->open(sink, client, total);
	if (ret != 0) {
		LOG_ERR("sink open failed (%d)", ret);
		goto out;
	}

	ret = source->api->fetch(source, client, sink);
	if (abort_requested(client) && ret == 0) {
		/* A source that ignores the abort flag still must not complete:
		 * the caller asked us to stop. */
		ret = -ECANCELED;
	}
	if (ret != 0) {
		LOG_ERR("transfer failed (%d, %u bytes written)", ret,
			client->received);
		sink->api->cancel(sink);
		goto out;
	}

	ret = sink->api->finish(sink);
	if (ret != 0) {
		LOG_ERR("image verify failed (%d)", ret);
		sink->api->cancel(sink);
		goto out;
	}

	ret = sink->api->activate(sink, client->cfg.permanent_upgrade);
	if (ret != 0) {
		LOG_ERR("activation failed (%d)", ret);
		sink->api->cancel(sink);
		goto out;
	}

	LOG_INF("image %s staged (%u bytes), reboot to boot it",
		client->remote_version, client->received);

out:
	finish(client, ret);
	if (ret == 0) {
		client->state = OTA_CLIENT_PENDING_REBOOT;
	}
	return ret;
}

int ota_client_update(struct ota_client *client)
{
	int ret;

	if (client == NULL || client->cfg.source == NULL) {
		return -EINVAL;
	}

	ret = client_lock(client);
	if (ret != 0) {
		return ret;
	}

	ret = update_locked(client);
	client_unlock(client);

	return ret;
}

void ota_client_abort(struct ota_client *client)
{
	if (client == NULL) {
		return;
	}

	/* Cooperative: the source polls ota_client_aborted() between chunks, and
	 * the core checks it again when the transfer returns. Nothing is
	 * interrupted mid-write, which is what keeps a half image from ever being
	 * activated. */
	atomic_set(&client->abort, 1);
}

bool ota_client_aborted(const struct ota_client *client)
{
	return (client != NULL) && abort_requested(client);
}

void ota_client_set_total(struct ota_client *client, int64_t total)
{
	if (client != NULL && total >= 0) {
		client->total = total;
	}
}

enum ota_client_state ota_client_state(const struct ota_client *client)
{
	return (client != NULL) ? client->state : OTA_CLIENT_IDLE;
}

const char *ota_client_state_str(enum ota_client_state state)
{
	switch (state) {
	case OTA_CLIENT_IDLE:
		return "idle";
	case OTA_CLIENT_CHECKING:
		return "checking";
	case OTA_CLIENT_UPDATING:
		return "updating";
	case OTA_CLIENT_PENDING_REBOOT:
		return "pending-reboot";
	case OTA_CLIENT_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

int ota_client_error(const struct ota_client *client)
{
	return (client != NULL) ? client->error : -EINVAL;
}

uint32_t ota_client_received(const struct ota_client *client)
{
	return (client != NULL) ? client->received : 0;
}

int64_t ota_client_total(const struct ota_client *client)
{
	return (client != NULL) ? client->total : -1;
}

const char *ota_client_remote_version(const struct ota_client *client)
{
	if (client == NULL) {
		return "";
	}

	return client->remote_version;
}

bool ota_client_busy(const struct ota_client *client)
{
	return (client != NULL) && (atomic_get(&client->busy) != 0);
}

void ota_client_report_progress(struct ota_client *client, uint32_t received,
				int64_t total)
{
	if (client == NULL) {
		return;
	}

	client->received = received;
	if (total >= 0) {
		client->total = total;
	}

	if (client->cfg.progress_cb != NULL) {
		client->cfg.progress_cb(client, received, client->total,
					client->cfg.progress_user);
	}
}

bool ota_client_pending_image(const struct ota_client *client)
{
	const struct ota_client_sink *sink;
	bool pending = false;

	if (client == NULL) {
		return false;
	}

	sink = client->cfg.sink;
	if (sink == NULL || sink->api->is_pending == NULL) {
		return false;
	}

	if (sink->api->is_pending(sink, &pending) != 0) {
		/* Loud on purpose: answering "not pending" when the read failed would
		 * silently disarm the rollback the bootloader is counting on. */
		LOG_ERR("could not read whether the image is a trial");
		return false;
	}

	return pending;
}

int ota_client_confirm(struct ota_client *client)
{
	struct ota_client_sink *sink;
	int ret;

	if (client == NULL) {
		return -EINVAL;
	}

	sink = client->cfg.sink;
	if (sink->api->confirm == NULL) {
		return -ENOSYS;
	}

	ret = sink->api->confirm(sink);
	if (ret == 0) {
		LOG_INF("running image confirmed");
	} else {
		LOG_ERR("confirm failed (%d)", ret);
	}

	return ret;
}

#if IS_ENABLED(CONFIG_OTA_CLIENT_THREAD)

/* One background update at a time: the thread is a library resource, so its
 * stack, control block and completion semaphore are static rather than
 * per-client. */
K_THREAD_STACK_DEFINE(ota_client_stack, CONFIG_OTA_CLIENT_THREAD_STACK_SIZE);
K_SEM_DEFINE(ota_client_done, 0, 1);

static struct k_thread ota_client_thread;
static int ota_client_result;
static struct ota_client *thread_owner;

static void ota_thread_worker(void *arg1, void *arg2, void *arg3)
{
	struct ota_client *client = arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* busy was already acquired by ota_client_start(), which is what keeps a
	 * second start from racing in here while the endpoints are in use. */
	ota_client_result = update_locked(client);
	atomic_set(&client->busy, 0);
	thread_owner = NULL;
	k_sem_give(&ota_client_done);
}

int ota_client_start(struct ota_client *client)
{
	int ret;

	if (client == NULL) {
		return -EINVAL;
	}

	if (thread_owner != NULL) {
		return -EBUSY;
	}

	ret = client_lock(client);
	if (ret != 0) {
		return ret;
	}

	k_sem_reset(&ota_client_done);
	thread_owner = client;
	ota_client_result = 0;
	k_thread_create(&ota_client_thread, ota_client_stack,
			K_THREAD_STACK_SIZEOF(ota_client_stack),
			ota_thread_worker, client, NULL, NULL,
			CONFIG_OTA_CLIENT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&ota_client_thread, "ota_client");

	return 0;
}

int ota_client_join(struct ota_client *client, int timeout_ms)
{
	k_timeout_t timeout = (timeout_ms < 0) ? K_FOREVER
					       : K_MSEC(timeout_ms);

	if (client == NULL) {
		return -EINVAL;
	}

	if (k_sem_take(&ota_client_done, timeout) != 0) {
		return -EALREADY; /* still running */
	}

	return ota_client_result;
}

#else /* !CONFIG_OTA_CLIENT_THREAD */

int ota_client_start(struct ota_client *client)
{
	ARG_UNUSED(client);
	/* Without the library thread the caller runs ota_client_update() on a
	 * thread of its own; say so rather than pretending it started. */
	return -ENOSYS;
}

int ota_client_join(struct ota_client *client, int timeout_ms)
{
	ARG_UNUSED(client);
	ARG_UNUSED(timeout_ms);
	return -ENOSYS;
}

#endif /* CONFIG_OTA_CLIENT_THREAD */
