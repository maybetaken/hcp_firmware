/*
 * hcp::ota - Zephyr worker for firmware updates.
 *
 * Owns the trigger queue, the endpoint resolution (discovered host or explicit
 * base URL) and the health gate that decides when a trial image becomes
 * permanent. The download, the staging and the activation are the reusable
 * ota_client module's business.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_OTA

#include "hcp/ota/ota.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>

extern "C" {
#include <ota_client/ota_client.h>
#include <ota_client/ota_sink_mcuboot.h>
#include <ota_client/ota_source_http.h>
}

#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"
#include "hcp/mqtt/client.h"
#include "hcp/ssdp/discovery.h"

namespace hcp {
namespace ota {
namespace {

constexpr size_t kUrlCap = CONFIG_OTA_CLIENT_URL_MAX_LEN;
constexpr int kPollMs = 1000;  // how often the worker looks for work or health

struct Request {
	char base[kUrlCap];  // empty = use the discovered host + templates
};

K_MSGQ_DEFINE(g_requests, sizeof(Request), 2, 4);
K_THREAD_STACK_DEFINE(g_stack, CONFIG_HCP_OTA_STACK_SIZE);
struct k_thread g_thread;

// Static so the endpoints outlive the calls: the library keeps the pointers it is
// given and only one update runs at a time.
struct ota_client g_client;
struct ota_client_http g_http;
struct ota_client_mcuboot g_sink;

bool g_worker_started;
bool g_client_ready;  // sink bound, so the bootloader can be asked questions
bool g_ever_ran;    // an update was attempted since this boot
bool g_confirmed;   // trial image made permanent (or none was pending)
bool g_deadline;    // trial image is under a watchdog deadline right now

/*
 * Build the two endpoint URLs. base == nullptr takes the host hcp::ssdp learned
 * from Home Assistant and fills the Kconfig templates; otherwise base must end in
 * '/' and the two file names are appended to it.
 */
bool ResolveEndpoints(const char* base, char* version_url, char* image_url) {
	if (base != nullptr && base[0] != '\0') {
		const size_t len = strlen(base);

		/* Version-gating is the whole point, so an image URL on its own is
		 * refused instead of being installed without asking the server what
		 * version it is. */
		if (len == 0 || base[len - 1] != '/' ||
		    len + sizeof("firmware.bin") > kUrlCap) {
			return false;
		}
		const int n1 = snprintk(version_url, kUrlCap, "%sversion.txt", base);
		const int n2 = snprintk(image_url, kUrlCap, "%sfirmware.bin", base);
		return n1 > 0 && static_cast<size_t>(n1) < kUrlCap && n2 > 0 &&
		       static_cast<size_t>(n2) < kUrlCap;
	}

	char host[CONFIG_OTA_CLIENT_URL_MAX_LEN / 2];
	uint16_t port = 0;

	if (!ssdp::ParseMqttUrl(ssdp::last_url(), host, sizeof(host), &port) ||
	    host[0] == '\0') {
		return false;
	}

	const int n1 = snprintk(version_url, kUrlCap, CONFIG_HCP_OTA_VERSION_URL, host);
	const int n2 = snprintk(image_url, kUrlCap, CONFIG_HCP_OTA_IMAGE_URL, host);
	return n1 > 0 && static_cast<size_t>(n1) < kUrlCap && n2 > 0 &&
	       static_cast<size_t>(n2) < kUrlCap;
}

/*
 * Bind the sink-only client: "is the image I am running still a trial?" has to be
 * answerable from the first second of boot, long before any update URL exists -
 * and that question decides both whether to arm the trial deadline and when there
 * is nothing left to confirm. RunUpdate() re-binds the same client with a source
 * once it has one.
 */
bool EnsureClient() {
	if (g_client_ready) {
		return true;
	}
	if (ota_client_mcuboot_init(&g_sink) != 0) {
		HCP_LOGE(hcp::log::kDomainOta, "mcuboot sink init failed");
		return false;
	}

	static const struct ota_client_config queries_cfg = {
		.sink = &g_sink.sink,
		.running_version = CONFIG_APP_VERSION,
	};

	if (ota_client_init(&g_client, &queries_cfg) != 0) {
		HCP_LOGE(hcp::log::kDomainOta, "no usable bootloader backend");
		return false;
	}

	g_client_ready = true;
	return true;
}

void RunUpdate(const Request& req) {
	char version_url[kUrlCap];
	char image_url[kUrlCap];

	if (!ResolveEndpoints(req.base[0] != '\0' ? req.base : nullptr, version_url,
			    image_url)) {
		HCP_LOGW(hcp::log::kDomainOta,
			 "no update URL: nothing discovered yet, or the base URL is "
			 "not a directory");
		return;
	}

	if (ota_client_http_init(&g_http, version_url, image_url) != 0) {
		HCP_LOGW(hcp::log::kDomainOta, "unusable update URLs (%s, %s)",
			 version_url, image_url);
		return;
	}

	static const struct ota_client_config cfg = {
		.source = &g_http.source,
		.sink = &g_sink.sink,
		.running_version = CONFIG_APP_VERSION,
		/* Trial swap: MCUboot reboots back to the old image unless this device
		 * confirms the new one from a healthy network (see MaybeConfirm). */
		.permanent_upgrade = false,
		.progress_cb = nullptr,
		.progress_user = nullptr,
	};

	if (ota_client_init(&g_client, &cfg) != 0) {
		HCP_LOGE(hcp::log::kDomainOta, "ota_client_init failed");
		return;
	}

	g_ever_ran = true;
	HCP_LOGI(hcp::log::kDomainOta, "checking %s", version_url);

	const int ret = ota_client_update(&g_client);

	if (ret == -EALREADY) {
		HCP_LOGI(hcp::log::kDomainOta, "already on %s",
			 CONFIG_APP_VERSION);
		return;
	}
	if (ret != 0) {
		/* Read the state first: as an argument list its evaluation order relative
		 * to the call is unspecified, and "idle" here would be a lie. */
		const char* state = ota_client_state_str(ota_client_state(&g_client));

		HCP_LOGE(hcp::log::kDomainOta, "update failed (%d, %s)", ret, state);
		return;
	}

	HCP_LOGI(hcp::log::kDomainOta, "staged %s, rebooting into it (unconfirmed)",
		 ota_client_remote_version(&g_client));
	/* Let the buffered log and the in-flight MQTT traffic drain before the
	 * reset; the new image gets its own boot to prove itself. */
	k_sleep(K_MSEC(2000));
	kernel::Reboot();
}

/*
 * Make the trial image permanent, once. "It booted" is not the criterion - an
 * image that cannot reach the broker is exactly what the rollback exists for, so
 * wait for a live session plus a grace period.
 */
void MaybeConfirm() {
	if (g_confirmed) {
		return;
	}
	if (k_uptime_get() < CONFIG_HCP_OTA_CONFIRM_AFTER_MS) {
		return;
	}
	if (!mqtt::IsConnected()) {
		return;
	}
	if (!ota_client_pending_image(&g_client)) {
		/* One line per boot, and deliberate: if the bootloader is ever misread as
		 * "nothing to confirm", the rollback we then skip is invisible otherwise. */
		if (g_deadline) {
			kernel::DisarmWatchdog();
			g_deadline = false;
		}
		HCP_LOGI(hcp::log::kDomainOta,
			 "no trial image to confirm (running %s is final)",
			 CONFIG_APP_VERSION);
		g_confirmed = true;
		return;
	}
	if (ota_client_confirm(&g_client) == 0) {
		g_confirmed = true;
		if (g_deadline) {
			/* The image earned its keep: cancel the deadline armed for it at boot,
			 * or a healthy device would reset once per watchdog window forever. */
			kernel::DisarmWatchdog();
			g_deadline = false;
		}
		HCP_LOGI(hcp::log::kDomainOta, "new image kept (session is up)");
	}
}

void Worker(void*, void*, void*) {
	while (true) {
		Request req;

		if (k_msgq_get(&g_requests, &req, K_MSEC(kPollMs)) == 0) {
			RunUpdate(req);
		}
		MaybeConfirm();
	}
}

}  // namespace

bool Start() {
	if (g_worker_started) {
		return true;
	}

	if (!EnsureClient()) {
		return false;
	}

	k_thread_create(&g_thread, g_stack, K_THREAD_STACK_SIZEOF(g_stack),
			Worker, nullptr, nullptr, nullptr, K_PRIO_PREEMPT(12), 0,
			K_NO_WAIT);
	k_thread_name_set(&g_thread, "hcp_ota");
	g_worker_started = true;

	HCP_LOGI(hcp::log::kDomainOta, "update worker ready (running %s)",
		 CONFIG_APP_VERSION);
	return true;
}

bool GuardTrialImage() {
	if (!EnsureClient()) {
		return false;
	}

	if (!ota_client_pending_image(&g_client)) {
		/* MCUboot armed its own (long) deadline before jumping here. Nothing is
		 * pending, so this image is final: cancel it, or a perfectly healthy device
		 * would reset once per bootloader timeout for the rest of its life. */
		kernel::DisarmWatchdog();
		return false;
	}

	/* Nobody feeds this deadline. It exists so that "the new image booted but
	 * never became healthy" - which includes hanging on the way up, before the
	 * worker or the network exist - ends in a reset, and the reset ends in
	 * MCUboot putting the old firmware back. Tighter than the bootloader's, which
	 * stays as the net under an image that never reaches main() at all. */
	g_deadline = kernel::ArmWatchdog(CONFIG_HCP_OTA_WATCHDOG_MS);
	if (g_deadline) {
		/* Say what the chip will actually do, not what was written into the
		 * register: Espressif's MWDT counts the installed window twice (interrupt
		 * stage, then reset stage) - measured, 20000 ms installed reset this board
		 * after 40.0 s. */
		HCP_LOGI(hcp::log::kDomainOta,
			 "trial image: watchdog armed, reset in ~%u ms unless this image "
			 "reaches the broker",
			 2 * CONFIG_HCP_OTA_WATCHDOG_MS);
	} else {
		HCP_LOGW(hcp::log::kDomainOta,
			 "trial image with no watchdog: it will not roll back by itself");
	}
	return g_deadline;
}

bool Trigger(const char* url) {
	Request req;

	if (!g_worker_started) {
		return false;
	}

	req.base[0] = '\0';
	if (url != nullptr) {
		if (strlen(url) >= sizeof(req.base)) {
			return false;
		}
		strcpy(req.base, url);
	}

	return k_msgq_put(&g_requests, &req, K_NO_WAIT) == 0;
}

bool HandleCommand(const char* payload, size_t len) {
	static constexpr char kPrefix[] = "ota";

	if (payload == nullptr || len < sizeof(kPrefix) - 1 ||
	    strncasecmp(payload, kPrefix, sizeof(kPrefix) - 1) != 0) {
		return false;
	}

	const char* rest = payload + sizeof(kPrefix) - 1;
	const size_t rest_len = len - (sizeof(kPrefix) - 1);

	/* "ota" alone (or "ota ") means "use what discovery found"; anything else
	 * must be a base URL, not a stray word. */
	if (rest_len == 0 || (rest_len == 1 && (rest[0] == ' ' || rest[0] == '\r'))) {
		return Trigger(nullptr);
	}
	if (rest[0] != ' ' && rest[0] != '\t') {
		return true;  // ours, but malformed: consumed without acting
	}

	char base[kUrlCap];
	size_t n = 0;

	while (n < sizeof(base) - 1 && n < rest_len && rest[1 + n] != '\0' &&
	       rest[1 + n] != ' ' && rest[1 + n] != '\r' && rest[1 + n] != '\n') {
		base[n] = rest[1 + n];
		n++;
	}
	base[n] = '\0';

	if (n == 0) {
		return true;
	}
	Trigger(base);
	return true;
}

const char* StateStr() {
	if (!g_worker_started) {
		return "";
	}
	return ota_client_state_str(ota_client_state(&g_client));
}

size_t StatusText(char* out, size_t cap) {
	if (out == nullptr || cap == 0 || !g_ever_ran) {
		if (out != nullptr && cap > 0) {
			out[0] = '\0';
		}
		return 0;
	}

	const enum ota_client_state st = ota_client_state(&g_client);
	const int64_t total = ota_client_total(&g_client);
	const char* ver = ota_client_remote_version(&g_client);
	int n;

	/* This string ends up on <sn>/diagnostics, a document the broker's clients render,
	 * so only what is actually known goes on the wire: the module's internal "no
	 * size yet" marker is -1 and the offered version starts out empty, and neither
	 * belongs there. The errno does, because "error" alone does not tell anybody
	 * whether the server refused, the link dropped, or the image did not fit. */
	if (st == OTA_CLIENT_ERROR) {
		n = snprintk(out, cap, "%s (%d)", StateStr(), ota_client_error(&g_client));
	} else if (total > 0) {
		n = snprintk(out, cap, "%s %u/%lld%s%s", StateStr(),
			   ota_client_received(&g_client), static_cast<long long>(total),
			   ver[0] != '\0' ? " " : "", ver);
	} else {
		n = snprintk(out, cap, "%s", StateStr());
	}

	return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n)
						       : cap - 1;
}

}  // namespace ota
}  // namespace hcp

#endif /* CONFIG_HCP_OTA */
