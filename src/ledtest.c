/*
 * WS2812 bring-up test (onboard RGB on GPIO8) — enabled via CONFIG_HCP_LED_TEST.
 * Zephyr v4.4.2 has no Espressif led_strip/RMT driver, so this bit-bangs the
 * data line with cycle-accurate busy-waits under IRQ lock. Throwaway test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys_clock.h>
#include <zephyr/irq.h>

#include <stdbool.h>
#include <stdint.h>

#define STRIP_PIN 8
#define NUM_LEDS  1

static const struct gpio_dt_spec strip = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin = STRIP_PIN,
	.dt_flags = 0,
};

static uint32_t ns_to_cycles(uint32_t ns)
{
	return (uint32_t)(((uint64_t)ns * sys_clock_hw_cycles_per_sec()) / 1000000000ULL);
}

static inline void wait_cycles(uint32_t cycles)
{
	uint32_t start = k_cycle_get_32();
	while ((k_cycle_get_32() - start) < cycles) {
	}
}

/* WS2812 timings (ns). */
#define T0H 400
#define T0L 850
#define T1H 800
#define T1L 450

static void send_byte(uint8_t b)
{
	for (int i = 7; i >= 0; --i) {
		bool bit = b & (1u << i);
		gpio_pin_set_dt(&strip, 1);
		wait_cycles(ns_to_cycles(bit ? T1H : T0H));
		gpio_pin_set_dt(&strip, 0);
		wait_cycles(ns_to_cycles(bit ? T1L : T0L));
	}
}

static void show(uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int key = irq_lock();
	for (int i = 0; i < NUM_LEDS; ++i) {
		send_byte(g); /* GRB */
		send_byte(r);
		send_byte(b);
	}
	gpio_pin_set_dt(&strip, 0);
	irq_unlock(key);
	k_busy_wait(100); /* >50us reset */
}

int main(void)
{
	if (!gpio_is_ready_dt(&strip)) {
		printk("ledtest: gpio not ready\n");
		return 0;
	}
	gpio_pin_configure_dt(&strip, GPIO_OUTPUT_INACTIVE);
	printk("ledtest: WS2812 color cycle on GPIO%d\n", STRIP_PIN);

	static const struct { uint8_t r, g, b; const char *name; } seq[] = {
		{ 60, 0, 0, "red" },
		{ 0, 60, 0, "green" },
		{ 0, 0, 60, "blue" },
		{ 30, 30, 0, "yellow" },
		{ 0, 0, 0, "off" },
	};
	unsigned i = 0;
	while (1) {
		show(seq[i].r, seq[i].g, seq[i].b);
		printk("ledtest: %s\n", seq[i].name);
		i = (i + 1) % (sizeof(seq) / sizeof(seq[0]));
		k_sleep(K_MSEC(1200));
	}
	return 0;
}
