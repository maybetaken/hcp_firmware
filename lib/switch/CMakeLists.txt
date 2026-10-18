# hcp::switchctl module — Zephyr build integration (posix fake built by host tests).
zephyr_include_directories(include)
if(CONFIG_HCP_SWITCH)
  target_sources(app PRIVATE src/zephyr/relay_zephyr.cpp)
endif()
