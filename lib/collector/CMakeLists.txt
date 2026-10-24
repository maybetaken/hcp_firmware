# hcp::collector module — Zephyr build integration.
# commands.cpp is pure (topic mapping, host-tested); the rest is the Zephyr worker.
zephyr_include_directories(include)
if(CONFIG_HCP_COLLECTOR)
  target_sources(app PRIVATE src/commands.cpp src/zephyr/collector_zephyr.cpp)
endif()
