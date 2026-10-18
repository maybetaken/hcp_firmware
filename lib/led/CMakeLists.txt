# hcp::led module — Zephyr build integration (posix framebuffer built by host tests).
# Zephyr backend OFF by default (no in-tree Espressif RMT led_strip driver).
zephyr_include_directories(include)
if(CONFIG_HCP_LED AND CONFIG_HCP_LED_BACKEND_ZEPHYR)
  target_sources(app PRIVATE src/zephyr/strip_zephyr.cpp)
endif()
