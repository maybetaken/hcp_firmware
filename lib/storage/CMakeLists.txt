# hcp::storage module — Zephyr build integration (posix built by host tests).
zephyr_include_directories(include)
if(CONFIG_HCP_STORAGE_BACKEND_ZEPHYR)
  target_sources(app PRIVATE src/zephyr/store_zephyr.cpp)
endif()
