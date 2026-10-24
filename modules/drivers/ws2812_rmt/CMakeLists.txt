# WS2812-over-RMT driver for ESP32 parts (Zephyr module).
#
# Named library rather than the default, so a link error names this module instead of
# "app". Built only when a board actually describes such a node (see Kconfig), which is
# why nothing here has to be remembered when a new board is added.
zephyr_library_named(ws2812_rmt)
zephyr_library_sources_ifdef(CONFIG_LED_STRIP_ESP_RMT ws2812_rmt.c)

# The RMT-per-channel GPIO-matrix signal indices come from the SoC's own table rather
# than from arithmetic on RMT_SIG_OUT0_IDX: the table is the vendor's statement of
# which channel maps to which signal, and "they are contiguous" is exactly the sort of
# reasonable assumption that produces an LED that never answers. The file needs only
# hal/rmt_periph.h and soc/gpio_sig_map.h, so compiling it in is cheaper than
# duplicating its contents here.
if(CONFIG_LED_STRIP_ESP_RMT)
  set(ESP_HAL_RMT ${ZEPHYR_BASE}/../modules/hal/espressif/components/esp_hal_rmt)
  zephyr_library_sources(${ESP_HAL_RMT}/${CONFIG_SOC}/rmt_periph.c)
endif()
