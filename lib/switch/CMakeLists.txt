# hcp::switchctl module — Zephyr build integration (posix fake built by host tests).
# command.cpp is pure (the JSON shapes of the "<sn>-K" device, host-tested); the
# relay backend is the only part that touches the board.
zephyr_include_directories(include)
if(CONFIG_HCP_SWITCH)
  target_sources(app PRIVATE src/command.cpp src/zephyr/relay_zephyr.cpp)
endif()
