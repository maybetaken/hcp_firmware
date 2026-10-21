# hcp::ssdp module — Zephyr build integration.
# discovery.cpp is pure parsing (also built by the host tests); the listener is
# target-only.
zephyr_include_directories(include)
target_sources(app PRIVATE src/discovery.cpp)
if(CONFIG_HCP_SSDP)
  target_sources(app PRIVATE src/zephyr/listener_zephyr.cpp)
endif()
