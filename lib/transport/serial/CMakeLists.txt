# hcp::serial module — Zephyr build integration (posix loopback built by host tests).
zephyr_include_directories(include)
if(CONFIG_HCP_SERIAL)
  target_sources(app PRIVATE src/zephyr/serial_zephyr.cpp)
endif()
