# hcp::led module - Zephyr build integration (posix framebuffer built by host tests).
# One backend on target: the led_strip API on the hcp-led-strip alias, whose driver is
# chosen per board in devicetree (worldsemi,ws2812-spi; espressif,esp32-ws2812-rmt on
# the C3, from modules/drivers/ws2812_rmt). Nothing here knows which one it is.
zephyr_include_directories(include)
if(CONFIG_HCP_LED AND CONFIG_HCP_LED_BACKEND_ZEPHYR)
  target_sources(app PRIVATE src/zephyr/strip_zephyr.cpp)
endif()
