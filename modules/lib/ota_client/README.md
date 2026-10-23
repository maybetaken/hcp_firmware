# ota_client — transport agnostic firmware update client for Zephyr

`ota_client` answers the question an application has after it learned an update
exists: *should I take this image, how do I stage it, when is it mine to keep,
and how do I get out of the way?* It deliberately does not answer *where the
bytes come from* or *which bootloader runs them* — those ends are two small
operation tables the application supplies.

Nothing in the library depends on a board, an application, a cloud service or a
vendor SDK, and the pure parts (version ordering, URL parsing) have no kernel
dependency at all so they can be unit tested on a host.

## Why this exists

Zephyr ships the pieces — `flash_img`, `boot_request_upgrade()`, `HTTP_CLIENT` —
but no reusable "check version, download, stage, activate, confirm, roll back"
policy, so every project writes its own. This is that missing layer, kept small
and transport-neutral so it can serve more than one project (and, if it proves
useful, live in tree).

## Design

```
                 ota_client core (policy, lifecycle, progress, abort)
                    |                       ^
        source api  |                       |  sink api
   get_version()    |                       |  open/write/finish
   get_size()?      |                       |  activate/cancel
   fetch(client, sink)                      |  is_pending/confirm
                    v                       |
        ota_source_http (HTTP GET)   ota_sink_mcuboot (flash_img + MCUboot)
```

* The **core** drives the order: check, `sink->open()`, `source->fetch()` (which
  streams through `sink->write()`), `sink->finish()`, `sink->activate()`. A
  failure at any point after `open()` calls `sink->cancel()`, so a partial image
  is never activated.
* The **sink** counts the bytes it accepted, so it owns progress reporting
  (`ota_client_report_progress()`); the source owns the total size
  (`ota_client_set_total()`) because that is where it is learned.
* One update at a time per client, acquired with a compare-and-set on
  `struct ota_client::busy`, so `ota_client_abort()` and the accessors need no
  locks.
* Cancellation is cooperative: a source polls `ota_client_aborted()` and returns
  `-ECANCELED`. Nothing is interrupted mid-`write()`, which is what keeps a half
  image from ever reaching the bootloader.

## Using it

```c
#include <ota_client/ota_client.h>
#include <ota_client/ota_source_http.h>
#include <ota_client/ota_sink_mcuboot.h>

static struct ota_client client;
static struct ota_client_http http;
static struct ota_client_mcuboot mcuboot;

int my_update(const char *version_url, const char *image_url, bool ask_first)
{
	int ret = ota_client_http_init(&http, version_url, image_url);

	if (ret == 0) {
		ret = ota_client_mcuboot_init(&mcuboot);
	}
	if (ret == 0) {
		ret = ota_client_init(&client, &(struct ota_client_config){
			.source = &http.source,
			.sink = &mcuboot.sink,
			.running_version = MY_FW_VERSION,
		});
	}
	if (ret == 0 && ask_first) {
		ret = ota_client_check(&client);
	}
	if (ret == 0) {
		ret = ota_client_update(&client);
		if (ret == 0) {
			sys_reboot(SYS_REBOOT_WARM);
		}
	}
	return ret; /* -EALREADY means "nothing newer", not a failure */
}
```

Confirming a trial image is the application's call, made when it judges itself
healthy — an IP address plus a live service session is a much better signal than
"the kernel started":

```c
if (ota_client_pending_image(&client)) {
	ota_client_confirm(&client);
}
```

The MCUboot sink leaves the swap *unconfirmed* by default
(`ota_client_config::permanent_upgrade == false`), so an image that never
confirms itself is rolled back by the bootloader on the next reset.

## Porting

Implement the tables you need; reuse the rest.

| Need | Implement |
| --- | --- |
| Another transport (CoAP, MQTT, BLE, USB, a cloud SDK) | `struct ota_client_source_api` — `get_version()`, optional `get_size()`, `fetch()` |
| Another bootloader or a raw A/B copy | `struct ota_client_sink_api` — `open/write/finish/activate/cancel/is_pending/confirm` |

Both take the instance pointer first, so a `CONTAINER_OF()` against the embedded
`struct ota_client_source` / `struct ota_client_sink` recovers your own state.
Set `.user_data` if you would rather not embed.

## Kconfig

| Symbol | Purpose |
| --- | --- |
| `CONFIG_OTA_CLIENT` | Build the library (core only, no backends). |
| `CONFIG_OTA_CLIENT_SOURCE_HTTP` | HTTP source; selects `HTTP_CLIENT`. |
| `CONFIG_OTA_CLIENT_HTTP_RECV_BUF_SIZE` | Buffer the HTTP client parses into; body fragments are handed to the sink from it. |
| `CONFIG_OTA_CLIENT_HTTP_TIMEOUT_MS`, `CONFIG_OTA_CLIENT_HTTP_IMAGE_TIMEOUT_MS` | Budget for one whole exchange: the version probe fails fast, the image download gets tens of seconds (Zephyr's `http_client_req()` counts the total, not per read). Both are also fields on `struct ota_client_http`, so a call site can tune them without a rebuild. |
| `CONFIG_OTA_CLIENT_SINK_MCUBOOT` | MCUboot sink; needs `BOOTLOADER_MCUBOOT` and `FLASH_MAP`. |
| `CONFIG_OTA_CLIENT_ALLOW_DOWNGRADE` | Accept any version difference instead of only newer images. |
| `CONFIG_OTA_CLIENT_THREAD` (+ `_STACK_SIZE`, `_PRIORITY`) | Optional worker thread behind `ota_client_start()` / `ota_client_join()`. Without it those return `-ENOSYS` and you run `ota_client_update()` on your own thread. |
| `CONFIG_OTA_CLIENT_VERSION_MAX_LEN`, `CONFIG_OTA_CLIENT_URL_MAX_LEN` | Size limits; `-EMSGSIZE`/`-EINVAL` rather than truncation. |
| `CONFIG_OTA_CLIENT_LOG_LEVEL` | Standard module log level. |

Requirements for the MCUboot sink: a `fixed-partitions` layout with an upload
slot (`flash_img` picks `slot1_partition` when the running image is
`slot0_partition`), and MCUboot built with image confirmation enabled if you want
the trial/rollback behaviour.

The sink also **erases the upload slot before staging** (unless
`CONFIG_IMG_ERASE_PROGRESSIVELY` is on, which makes the stream erase each sector
as it reaches it). That is not a convenience: `flash_img` only programs, and NOR
writes can only clear bits, so staging over the image that is still in the slot
from the last update produces "new AND old". The bootloader then rejects the
image on its own hash check, wipes the slot, and boots the old firmware - which
looks like an update that fails every other time, because the first one after a
blank slot succeeds. If you replace the sink, do the same.

## Limitations

* No TLS in the HTTP source. It returns `-ENOTSUP` for an `https://` URL rather
  than pretending. Authenticity comes from the MCUboot signature, not the
  network.
* No hash of the download in the library: the source refuses a body shorter or
  longer than the advertised `Content-Length`, and the bootloader validates the
  image itself (signature or its own integrity check, depending on how it was
  built).
* One background update at a time when `CONFIG_OTA_CLIENT_THREAD` is on (the
  thread is a library resource, not per-client).
* `ota_client_update()` blocks for the length of the download; keep it off any
  thread with a deadline.
* HTTPS proxying, resumption (`Range:`) after a dropped connection, and A/B
  bookkeeping beyond what the sink exposes are simply not implemented.

## Layout

```
include/ota_client/ota_client.h      core API: state machine, source/sink tables
include/ota_client/ota_util.h        pure helpers: version order, URL split
include/ota_client/ota_source_http.h HTTP source instance
include/ota_client/ota_sink_mcuboot.h MCUboot sink instance
src/ota_util.c src/ota_client.c       core + helpers (no I/O)
src/ota_source_http.c src/ota_sink_mcuboot.c backends
```

## License

MIT - see [LICENSE](LICENSE). Contributions are expected to follow Zephyr's coding
style (`scripts/checkpatch.pl`), keep the core free of board and vendor
dependencies, and add a host test for anything pure.

The module has no third-party code and no build-time dependency outside Zephyr
itself; `CONFIG_BOOTLOADER_MCUBOOT`, `flash_img` and `HTTP_CLIENT` are Zephyr
subsystems it selects, so a build using it is subject to Zephyr's own licensing
like any other Zephyr application.
