# hcp::ota module — Zephyr build integration.
# The whole module is target-only glue around the reusable ota_client module
# (modules/lib/ota_client); the pieces worth testing on a host (version compare,
# URL shape) live and are tested there.
zephyr_include_directories(include)
if(CONFIG_HCP_OTA)
  target_sources(app PRIVATE src/zephyr/ota_zephyr.cpp)
endif()
