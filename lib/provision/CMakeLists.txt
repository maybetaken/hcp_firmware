# hcp::provision module — Zephyr build integration.
# fsm.cpp is pure logic (also compiled by the host tests); provisioner.cpp is the
# target executor that talks to Wi-Fi / storage / LED / button / BluFi.
zephyr_include_directories(include)
if(CONFIG_HCP_PROVISION)
  target_sources(app PRIVATE src/fsm.cpp src/zephyr/provisioner.cpp)
endif()
