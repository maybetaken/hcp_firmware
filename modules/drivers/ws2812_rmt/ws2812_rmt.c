/*
 * WS2812-class addressable LED driver for the ESP32 RMT peripheral.
 *
 * Implements Zephyr's led_strip API, so an application using led_strip_update_rgb()
 * does not know or care that the ESP32 has no in-tree RMT led_strip driver: a board
 * points its `hcp-led-strip`-style alias at a node with this compatible instead of at
 * the worldsemi,ws2812-spi one. That is the whole reason this is a driver and not a
 * branch inside the application's LED facade.
 *
 * Why RMT rather than bit-banging the data line from the CPU: the WS2812 distinguishes
 * a one from a zero by how long the line is high (400 ns vs 800 ns of a 1250 ns bit),
 * which is inside the cost of the primitives a CPU loop has to use to measure itself
 * with. Measured on an ESP32-C3: one GPIO store through Zephyr's API costs 312 ns and
 * one system-clock read 687 ns - against a 400 ns phase. A CPU loop can therefore
 * report in-spec timings while producing waveforms a strip ignores, which is exactly
 * what happened on the bench before this driver existed. RMT counts ticks off a
 * hardware clock (APB, 80 MHz on the C3 whatever the CPU is doing), so a phase is an
 * exact number of 12.5 ns ticks and neither the CPU speed, nor an ISR, nor the cost of
 * a register read can stretch one. It also needs no interrupt blackout across the
 * frame, which matters on a board that runs BLE as well.
 *
 * The previous firmware drove this same pin from this same peripheral
 * (_led(GPIO_NUM_8, 1) over led_strip_new_rmt_device), and it worked.
 *
 * Only Espressif's hal/ll layer is used: this HAL copy ships esp_hal_rmt but not the
 * IDF rmt driver, and the ll functions are all static inline, so nothing has to be
 * added to the build beyond this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_ws2812_rmt

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/rmt_ll.h>
#include <hal/rmt_periph.h>
#include <soc/clk_tree_defs.h>
#include <soc/rmt_struct.h>
#include <soc/soc.h>
#include <esp_rom_gpio.h>

/* Append one symbol to a channel's RAM. The member name is not the same across
 * generations, and this file refuses to guess: an ESP32-C6 build that silently wrote
 * the C3's field would compile only by accident, and the symptom would be an LED that
 * never answers - indistinguishable from a hardware fault from outside the box. Parts
 * listed here are parts someone has run on hardware; adding one means checking its
 * channel count and data member, not just making the name compile. */
#if defined(CONFIG_SOC_ESP32C3)
#define RMT_CHANNEL_DATA(dev, ch) ((dev)->data_ch[ch])
static const uint32_t kChannelsPerInstance = 4;
#else
#error "ws2812_rmt: this part's RMT channel-data register is not described here yet"
#endif

LOG_MODULE_REGISTER(ws2812_rmt, CONFIG_LOG_DEFAULT_LEVEL);

/* One channel of RMT RAM is 48 symbols on the ESP32-C3, and a WS2812 bit is one
 * symbol, so a strip is transmitted in chunks of six bytes and each chunk is started
 * after the previous one reports done. The gap between chunks is a handful of register
 * writes (microseconds), far below the ~50 us of low that latches a frame, so the
 * strip sees one continuous frame. */
#define kSymbolsPerChannel SOC_RMT_MEM_WORDS_PER_CHANNEL
#define kBytesPerChunk     (kSymbolsPerChannel / 8)

/* WS2812 phases in 12.5 ns ticks (APB 80 MHz on the ESP32-C3): t0H 400 ns -> 32, t1H
 * 800 ns -> 64, bit cell 1250 ns -> 100. A one and a zero then differ in the one thing
 * the strip samples. */
#define kT0hTicks     32
#define kT1hTicks     64
#define kBitCellTicks 100

struct ws2812_rmt_config {
	struct gpio_dt_spec data;
	uint8_t channel;      /* RMT TX channel 0..3 on this part */
	uint16_t length;      /* pixels */
	uint16_t reset_delay; /* microseconds of low that latches a frame */
	/* Byte order on the wire, read from DT's color-mapping so the board, not this
	 * driver, states how its LED is wired. LED_COLOR_ID_* values. */
	uint8_t color_order[3];
};

struct ws2812_rmt_data {
	rmt_dev_t *regs;
};

static rmt_dev_t *const rmt_base(void)
{
	/* Not the `RMT` extern: that is defined by IDF startup files this port does
	 * not build, and a link error there would look like a missing driver. */
	return (rmt_dev_t *)DR_REG_RMT_BASE;
}

/* The hardware's own symbol layout: duration0(15) level0(1) duration1(15) level1(1). */
static inline uint32_t ws2812_symbol(bool one)
{
	const uint32_t high = one ? kT1hTicks : kT0hTicks;
	const uint32_t low = kBitCellTicks - high;

	return (high & 0x7FFFU) | (1U << 15) | ((low & 0x7FFFU) << 16);
}

/* Push one chunk of bits and wait for the hardware to have driven them. */
static int ws2812_rmt_flush_chunk(const struct device *dev, const uint8_t *bytes,
				  uint8_t count)
{
	const struct ws2812_rmt_config *const cfg = dev->config;
	struct ws2812_rmt_data *const data = dev->data;
	const uint32_t ch = cfg->channel;
	/* The limit is where this chunk stops, not the channel's capacity. Left at the
	 * capacity, the read pointer runs past the symbols that were written and waits
	 * for ones that never arrive, so the frame never reaches its end and never
	 * reports done - which is exactly what the first bench run did, and the driver
	 * said so instead of leaving an LED that quietly does nothing. */
	const uint32_t symbols = (uint32_t)count * 8U;

	rmt_ll_tx_stop(data->regs, ch);
	rmt_ll_tx_reset_pointer(data->regs, ch);
	rmt_ll_tx_set_limit(data->regs, ch, symbols - 1);
	rmt_ll_clear_interrupt_status(data->regs,
				      RMT_LL_EVENT_TX_MASK(ch) |
				      RMT_LL_EVENT_TX_ERROR(ch));

	for (uint8_t i = 0; i < count; i++) {
		for (int bit = 7; bit >= 0; bit--) {
			RMT_CHANNEL_DATA(data->regs, ch) =
				ws2812_symbol(((bytes[i] >> bit) & 1U) != 0U);
		}
	}

	rmt_ll_tx_start(data->regs, ch);

	/* The RAW status, not the masked one: rmt_ll_tx_get_interrupt_status() reads
	 * int_st, which is "raw AND interrupt-enabled", and this driver takes no
	 * interrupt at all - so polling it can only ever return zero. The first build on
	 * the bench proved that, reporting "never reported done" for every frame. Reading
	 * raw costs nothing extra and keeps the wait free of an ISR, a handler and the
	 * shared state that would come with them.
	 *
	 * Bounded because a chunk is 48 symbols = 60 us of hardware time; a channel that
	 * never finishes is broken, and saying so beats hanging the caller, which is the
	 * system workqueue for a status LED. */
	const uint32_t done_bit = RMT_LL_EVENT_TX_DONE(ch);
	const uint32_t err_bit = RMT_LL_EVENT_TX_ERROR(ch);
	int rc = 0;

	for (uint32_t passes = 0;; passes++) {
		const uint32_t raw = rmt_ll_tx_get_interrupt_status_raw(data->regs, ch);

		if ((raw & done_bit) != 0U) {
			break;
		}
		if ((raw & err_bit) != 0U) {
			/* An error here means the symbols never reached the counter - the
			 * write path or the clock, not the strip - so it is worth its own
			 * line rather than the timeout's generic wording. */
			LOG_ERR("rmt channel %lu reported an error", (unsigned long)ch);
			rc = -EIO;
			break;
		}
		if (passes > 200000U) {
			/* A chunk is 48 symbols = 60 us of hardware time, so this is orders of
			 * magnitude past any real frame. The register dump is what makes it
			 * one round trip instead of four: every RMT register reading zero means
			 * the block is not being clocked or is held in reset (the actual cause
			 * found on the bench), while non-zero registers with no done bit point
			 * at the symbol path or the limit. */
			LOG_ERR("rmt ch%lu never done: tx_conf=0x%08x limit=0x%08x "
				"status=0x%08x raw=0x%08x sys_conf=0x%08x clk_en=%d "
				"rst_en0=0x%08x",
				(unsigned long)ch, (unsigned)data->regs->tx_conf[ch].val,
				(unsigned)data->regs->tx_lim[ch].val,
				(unsigned)data->regs->tx_status[ch].val,
				(unsigned)data->regs->int_raw.val,
				(unsigned)data->regs->sys_conf.val,
				(int)SYSTEM.perip_clk_en0.reg_rmt_clk_en,
				(unsigned)*(const volatile uint32_t *)&SYSTEM.perip_rst_en0);
			rc = -EIO;
			break;
		}
	}

	rmt_ll_tx_stop(data->regs, ch);
	rmt_ll_clear_interrupt_status(data->regs, RMT_LL_EVENT_TX_MASK(ch) | err_bit);
	return rc;
}

/* The wire order is whatever the board's color-mapping says; resolving it by name
 * rather than by casting the pixel struct keeps this driver honest about struct
 * led_rgb's layout being nobody's contract. Unknown ids are rejected at init. */
static uint8_t color_component(const struct led_rgb *pixel, uint8_t id)
{
	switch (id) {
	case LED_COLOR_ID_RED:
		return pixel->r;
	case LED_COLOR_ID_GREEN:
		return pixel->g;
	default:
		return pixel->b;
	}
}

static int ws2812_rmt_update_rgb(const struct device *dev,
				 struct led_rgb *pixels, size_t count)
{
	const struct ws2812_rmt_config *const cfg = dev->config;
	uint8_t chunk[kBytesPerChunk];
	size_t filled = 0;

	for (size_t i = 0; i < count; i++) {
		const uint8_t wire[3] = {
			color_component(&pixels[i], cfg->color_order[0]),
			color_component(&pixels[i], cfg->color_order[1]),
			color_component(&pixels[i], cfg->color_order[2]),
		};

		for (int j = 0; j < 3; j++) {
			chunk[filled++] = wire[j];

			/* Flush on a full chunk, and on the last byte of the frame so a
			 * strip whose byte count is not a multiple of the chunk size still
			 * gets its tail out. */
			const bool last = (i + 1 == count) && (j == 2);

			if (filled == kBytesPerChunk || last) {
				const int rc = ws2812_rmt_flush_chunk(
					dev, chunk, (uint8_t)filled);

				if (rc != 0) {
					return rc;
				}
				filled = 0;
			}
		}
	}

	/* Latch: the line must stay low for reset-delay before the next frame. */
	k_busy_wait(cfg->reset_delay);
	return 0;
}

static size_t ws2812_rmt_length(const struct device *dev)
{
	const struct ws2812_rmt_config *const cfg = dev->config;

	return cfg->length;
}

static const struct led_strip_driver_api ws2812_rmt_api = {
	.update_rgb = ws2812_rmt_update_rgb,
	.length = ws2812_rmt_length,
};

static int ws2812_rmt_init(const struct device *dev)
{
	const struct ws2812_rmt_config *const cfg = dev->config;
	struct ws2812_rmt_data *const data = dev->data;
	const uint32_t ch = cfg->channel;

	data->regs = rmt_base();

	if (cfg->channel >= kChannelsPerInstance) {
		LOG_ERR("rmt channel %u does not exist on this part (%u per instance)",
			cfg->channel, (unsigned)kChannelsPerInstance);
		return -EINVAL;
	}
	if (!gpio_is_ready_dt(&cfg->data)) {
		LOG_ERR("gpio controller for the data line is not ready");
		return -ENODEV;
	}
	/* A repeated or out-of-range color-mapping would send the same component twice
	 * and silently show wrong colours, which is the kind of bug that reads as "the
	 * strip is broken" - so it is refused here, at boot, instead. */
	for (int i = 0; i < 3; i++) {
		if (cfg->color_order[i] > LED_COLOR_ID_BLUE) {
			LOG_ERR("color-mapping[%d] is not an RGB id", i);
			return -EINVAL;
		}
		for (int j = i + 1; j < 3; j++) {
			if (cfg->color_order[i] == cfg->color_order[j]) {
				LOG_ERR("color-mapping repeats id %u", cfg->color_order[i]);
				return -EINVAL;
			}
		}
	}
	/* Plain output first: that is what selects the GPIO matrix for the pad and
	 * enables its output driver. The matrix source is re-pointed at RMT below, so
	 * the pin ends up driven by the peripheral, not by software. */
	int rc = gpio_pin_configure_dt(&cfg->data, GPIO_OUTPUT_INACTIVE);

	if (rc != 0) {
		return rc;
	}

	/* Bus clock before any register touch: gated off, they all read back zero and
	 * the failure shows up as an LED that never moves. */
	rmt_ll_enable_bus_clock(0, true);
	/* Pulse the peripheral out of reset. Without this the first bench builds read
	 * every RMT register as zero no matter what was written - the clock gate was on,
	 * the block was simply held in reset - which is why an LED driven this way looks
	 * dead rather than mis-configured. rmt_ll_reset_register() asserts then
	 * de-asserts the line, so it is safe to call at init. */
	rmt_ll_reset_register(0);

	/* RMT's symbol RAM has its own power switch. Left to the PMU, the memory can be
	 * powered down while the registers answer - the writes then go nowhere and TX
	 * never reaches its limit, which is the class of failure the first bench runs
	 * showed. Forcing it on is correct here rather than merely convenient: this
	 * device never suspends, so there is no low-power state to disturb, and the
	 * driver then depends on no PMU configuration at all. */
	rmt_ll_mem_force_power_on(data->regs);

	rmt_ll_set_group_clock_src(data->regs, ch, RMT_CLK_SRC_APB, 1, 1, 0);
	rmt_ll_enable_mem_access_nonfifo(data->regs, true);
	rmt_ll_tx_reset_channels_clock_div(data->regs, BIT(ch));
	rmt_ll_tx_set_channel_clock_div(data->regs, ch, 1);
	rmt_ll_tx_reset_pointer(data->regs, ch);

	/* Rest low between frames, so the tail of one is never read as the head of
	 * the next. */
	rmt_ll_tx_fix_idle_level(data->regs, ch, 0, true);
	rmt_ll_tx_set_mem_blocks(data->regs, ch, 1);
	/* No limit here on purpose: it belongs to a chunk, not to the channel, and it
	 * is written per flush (see ws2812_rmt_flush_chunk). */
	rmt_ll_tx_enable_loop(data->regs, ch, false);

	/* Signal index from the SoC's own table: "channel N" is a different PMUX
	 * entry on every part, which is precisely the kind of number not to copy out
	 * of a TRM into a driver. */
	esp_rom_gpio_connect_out_signal(
		cfg->data.pin, soc_rmt_signals[0].channels[ch].tx_sig, false, false);

	LOG_INF("ws2812 on rmt ch%lu gpio%u, %u px, %u ticks/bit",
		(unsigned long)ch, cfg->data.pin, cfg->length,
		(unsigned)kBitCellTicks);
	return 0;
}

/* Plain DEVICE_DT_INST_DEFINE rather than LED_DEVICE_DT_INST_DEFINE: the latter is not
 * defined by this Zephyr's led_strip headers (checked in include/zephyr/drivers/
 * led_strip.h), and the in-tree ws2812_spi driver uses the former. */
#define WS2812_RMT_DEFINE(idx)                                                   \
	static const struct ws2812_rmt_config ws2812_rmt_cfg_##idx = {           \
		.data = GPIO_DT_SPEC_INST_GET(idx, data_gpios),                  \
		.channel = DT_INST_PROP(idx, espressif_rmt_channel),             \
		.length = DT_INST_PROP(idx, chain_length),                       \
		.reset_delay = DT_INST_PROP(idx, reset_delay),                   \
		.color_order = { DT_INST_PROP_BY_IDX(idx, color_mapping, 0),     \
				 DT_INST_PROP_BY_IDX(idx, color_mapping, 1),     \
				 DT_INST_PROP_BY_IDX(idx, color_mapping, 2) },   \
	};                                                                       \
	static struct ws2812_rmt_data ws2812_rmt_data_##idx;                     \
	DEVICE_DT_INST_DEFINE(idx, ws2812_rmt_init, NULL,                        \
			      &ws2812_rmt_data_##idx, &ws2812_rmt_cfg_##idx,     \
			      POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,       \
			      &ws2812_rmt_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_RMT_DEFINE)
