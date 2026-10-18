# hcp::modbus module — Zephyr build integration.
# Vendored nanoMODBUS (C) + thin C++ master over hcp::serial::Port.
zephyr_include_directories(include)
zephyr_include_directories(vendor/nanomodbus)
if(CONFIG_HCP_MODBUS)
  target_sources(app PRIVATE
    vendor/nanomodbus/nanomodbus.c
    src/master.cpp)
endif()
