# hcp::modbus module — Zephyr build integration.
# Thin C++ master over hcp::serial::Port. The Modbus protocol comes from the
# nanoMODBUS Zephyr module (modules/lib/nanomodbus), not vendored here.
zephyr_include_directories(include)
if(CONFIG_HCP_MODBUS)
  target_sources(app PRIVATE src/master.cpp)
endif()
