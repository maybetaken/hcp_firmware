# hcp::log module — Zephyr build integration (posix backend built by host tests).
zephyr_include_directories(include)
if(CONFIG_HCP_LOG_BACKEND_ZEPHYR)
  target_sources(app PRIVATE src/zephyr/log_zephyr.cpp)
endif()
