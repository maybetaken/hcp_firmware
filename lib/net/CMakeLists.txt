# hcp::net module — Zephyr build integration (Wi-Fi is target-only).
zephyr_include_directories(include)
if(CONFIG_HCP_NET)
  target_sources(app PRIVATE src/zephyr/wifi_zephyr.cpp)
endif()
