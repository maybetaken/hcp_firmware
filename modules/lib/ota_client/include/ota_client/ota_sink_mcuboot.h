/*
 * Copyright (c) 2026 Helios Collector Platform contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief MCUboot image sink for ota_client.
 *
 * Stages the incoming image in the secondary slot ("image-1" / the upload
 * partition chosen by Zephyr's flash_img helper) and asks MCUboot to boot it on
 * the next reset. Signature and trailer validation are MCUboot's, not ours.
 *
 * @ingroup ota_client
 */

#ifndef OTA_CLIENT_SINK_MCUBOOT_H_
#define OTA_CLIENT_SINK_MCUBOOT_H_

#include <ota_client/ota_client.h>

#include <zephyr/dfu/flash_img.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCUboot sink instance.
 *
 * @a sink must stay the first member. The embedded flash_img context carries
 * the write buffer, so this is a few hundred bytes plus
 * CONFIG_IMG_BLOCK_BUF_SIZE.
 */
struct ota_client_mcuboot {
	/** Hand this to ota_client_config::sink. */
	struct ota_client_sink sink;
	struct flash_img_context img;
	struct ota_client *client;
	uint32_t written;
};

/**
 * @brief Prepare an MCUboot sink.
 *
 * @param ctx Instance to initialise (owned by the caller).
 *
 * @retval 0 Success.
 * @retval -EINVAL NULL argument.
 */
int ota_client_mcuboot_init(struct ota_client_mcuboot *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CLIENT_SINK_MCUBOOT_H_ */
