# hcp::collector module — Zephyr build integration.
# commands.cpp and schedule.cpp are pure (topic mapping, cadence and backoff are all
# host-tested); the rest is the Zephyr worker that owns the bus and the report.
zephyr_include_directories(include)
if(CONFIG_HCP_COLLECTOR)
  target_sources(app PRIVATE src/commands.cpp src/schedule.cpp
                             src/zephyr/collector_zephyr.cpp)
endif()
