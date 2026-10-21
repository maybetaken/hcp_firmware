/*
 * BluFi device-side API (reusable Zephyr module).
 * Brings up a BLE peripheral exposing the ESP BluFi GATT (service 0xFFFF,
 * write char 0xFF01 phone->esp, notify char 0xFF02 esp->phone). Received
 * frames are parsed and surfaced to a callback.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_BLUFI_H_
#define BLUFI_BLUFI_H_

#include <stdint.h>

#include "blufi/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called (from the blufi rx work queue) for each parsed BluFi frame. */
typedef void (*blufi_data_cb_t)(uint8_t type, uint8_t subtype,
	const uint8_t *data, uint16_t len, void *user);

/* Register the callback before starting. */
void blufi_set_callback(blufi_data_cb_t cb, void *user);

/* Start advertising the BluFi service (non-blocking). Returns 0/neg errno. */
int blufi_start(void);

/* Send a data frame to the phone (E2P notify): returns 0/neg errno. */
int blufi_send(uint8_t type, uint8_t subtype, const uint8_t *data, uint16_t len);

/* Tear down the current BLE link (the phone asked us to, via
 * CTRL/DISCONNECT_BLE - the ESP-IDF example calls esp_blufi_disconnect() there).
 * Advertising resumes from the disconnected() callback. Returns 0/neg errno. */
int blufi_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* BLUFI_BLUFI_H_ */
