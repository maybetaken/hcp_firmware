# hcp::registry module — Zephyr build integration.
# Point table, the Home Assistant wire shape, and the precedence/persistence rules.
# Deliberately pure logic: persistence goes through a KeyValue seam the caller
# implements over hcp::storage, which keeps every rule in the host tests.
zephyr_include_directories(include)
if(CONFIG_HCP_REGISTRY)
  target_sources(app PRIVATE src/table.cpp)
endif()
