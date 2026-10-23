/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief MCUboot image sink implementation.
 */

#include <ota_client/ota_sink_mcuboot.h>

#include <errno.h>
#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_DECLARE(ota_client, CONFIG_OTA_CLIENT_LOG_LEVEL);

static struct ota_client_mcuboot *to_ctx(struct ota_client_sink *sink)
{
	return CONTAINER_OF(sink, struct ota_client_mcuboot, sink);
}

static int mcuboot_open(struct ota_client_sink *sink, struct ota_client *client,
			int64_t total)
{
	struct ota_client_mcuboot *ctx = to_ctx(sink);
	int ret;

	ARG_UNUSED(total);

	ctx->client = client;
	ctx->written = 0;

	/* flash_img_init() only resolves and opens the upload partition (the
	 * secondary slot). Programming it is all flash_img does, so the slot has
	 * to be clean before anything goes in: NOR writes can only clear bits,
	 * staging over a previous image leaves behind "new AND old", and MCUboot
	 * then rejects the image on its hash check - and wipes the secondary slot
	 * while doing so, which makes the failure look random. Nothing else
	 * erases it unless the stream is told to do so sector by sector
	 * (CONFIG_IMG_ERASE_PROGRESSIVELY), which needs a driver that supports
	 * explicit erase; doing it here works on every one. */
	ret = flash_img_init(&ctx->img);
	if (ret != 0) {
		LOG_ERR("upload slot unusable (%d)", ret);
		return ret;
	}

	if (!IS_ENABLED(CONFIG_IMG_ERASE_PROGRESSIVELY)) {
		const uint32_t slot_size = ctx->img.flash_area->fa_size;

		/* Blocking for a few seconds on SPI-NOR (a slot here, 1.8 MB, takes
		 * about two) and nothing can be reported while it runs, so log it. */
		LOG_INF("erasing the upload slot (%u bytes)", (unsigned int)slot_size);

		ret = flash_area_erase(ctx->img.flash_area, 0, slot_size);
		if (ret != 0) {
			LOG_ERR("upload slot erase failed (%d)", ret);
			flash_area_close(ctx->img.flash_area);
			ctx->img.flash_area = NULL;
			return ret;
		}
	}

	return 0;
}

static int mcuboot_write(struct ota_client_sink *sink, const void *data,
			 size_t len)
{
	struct ota_client_mcuboot *ctx = to_ctx(sink);
	int ret;

	if (len == 0) {
		return 0;
	}

	ret = flash_img_buffered_write(&ctx->img, data, len, false);
	if (ret != 0) {
		LOG_ERR("write at %u failed (%d)", ctx->written, ret);
		return ret;
	}

	ctx->written += len;
	ota_client_report_progress(ctx->client, ctx->written, -1);

	return 0;
}

static int mcuboot_finish(struct ota_client_sink *sink)
{
	struct ota_client_mcuboot *ctx = to_ctx(sink);
	int ret;

	/* Flush with zero length: pushes the padded tail out of the block
	 * buffer, so what is in flash is the complete image. */
	ret = flash_img_buffered_write(&ctx->img, NULL, 0, true);
	if (ret != 0) {
		LOG_ERR("final flush failed (%d)", ret);
		return ret;
	}

	LOG_INF("%u bytes staged in the secondary slot", ctx->written);
	return 0;
}

static int mcuboot_activate(struct ota_client_sink *sink, bool permanent)
{
	ARG_UNUSED(sink);

	/* permanent == false leaves the trial swap to MCUboot: if the new image
	 * never confirms itself (see mcuboot_confirm) the bootloader goes back to
	 * this one on the next reset. */
	return boot_request_upgrade(permanent ? 1 : 0);
}

static void mcuboot_cancel(struct ota_client_sink *sink)
{
	struct ota_client_mcuboot *ctx = to_ctx(sink);

	/* Nothing to unwind: the slot was erased when the update started, so what
	 * is left in it is either nothing or a complete image, and MCUboot only
	 * swaps on the magic written by mcuboot_activate(). */
	LOG_WRN("update cancelled after %u bytes", ctx->written);
}

static int mcuboot_is_pending(const struct ota_client_sink *sink, bool *pending)
{
	const int type = mcuboot_swap_type();

	ARG_UNUSED(sink);

	if (type < 0) {
		return type;
	}

	/* REVERT is the only state worth confirming: "the next reset goes back to
	 * the other image". Do not infer this from boot_is_img_confirmed() - that
	 * helper reports a slot whose trailer carries no magic at all (a factory
	 * flashed image, but also the state after a revert) as "confirmed", which
	 * makes a trial image look already kept and silently skips the confirmation
	 * the bootloader was waiting for. Zephyr's own hawkbit/updatehub code asks
	 * mcuboot_swap_type() for the same reason. */
	*pending = (type == BOOT_SWAP_TYPE_REVERT);
	return 0;
}

static int mcuboot_confirm(struct ota_client_sink *sink)
{
	bool pending = false;
	int ret;

	ret = mcuboot_is_pending(sink, &pending);
	if (ret != 0) {
		LOG_ERR("could not read the swap state (%d)", ret);
		return ret;
	}
	if (!pending) {
		return 0;
	}

	ret = boot_write_img_confirmed();
	if (ret != 0) {
		LOG_ERR("could not confirm the running image (%d)", ret);
	}

	return ret;
}

static const struct ota_client_sink_api ota_client_mcuboot_api = {
	.open = mcuboot_open,
	.write = mcuboot_write,
	.finish = mcuboot_finish,
	.activate = mcuboot_activate,
	.cancel = mcuboot_cancel,
	.is_pending = mcuboot_is_pending,
	.confirm = mcuboot_confirm,
};

int ota_client_mcuboot_init(struct ota_client_mcuboot *ctx)
{
	if (ctx == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->sink.api = &ota_client_mcuboot_api;
	ctx->sink.user_data = ctx;

	return 0;
}
