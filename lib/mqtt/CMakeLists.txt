# hcp::mqtt module — Zephyr build integration.
# backoff.cpp is pure logic (also built by the host tests); the client is target-only.
zephyr_include_directories(include)
target_sources(app PRIVATE src/backoff.cpp)
if(CONFIG_HCP_MQTT)
  target_sources(app PRIVATE src/zephyr/client_zephyr.cpp)
endif()
