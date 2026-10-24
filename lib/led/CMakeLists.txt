# hcp::led module — Zephyr build integration (posix framebuffer built by host tests).
# strip_zephyr.cpp serves both Zephyr-side backends: the led_strip/SPI one and the
# single-pin bit-bang one C3 uses (Zephyr has no in-tree Espressif RMT led_strip).
zephyr_include_directories(include)
if(CONFIG_HCP_LED AND (CONFIG_HCP_LED_BACKEND_ZEPHYR OR
                       CONFIG_HCP_LED_BACKEND_GPIO_BITBANG))
  target_sources(app PRIVATE src/zephyr/strip_zephyr.cpp)
endif()
