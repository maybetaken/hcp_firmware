# hcp::modbus module — Zephyr build integration.
# Thin C++ facade over Zephyr's own Modbus RTU client (subsys/modbus). The UART and
# the RS485 direction pin come from the `zephyr,modbus-serial` devicetree node.
zephyr_include_directories(include)
if(CONFIG_HCP_MODBUS)
  target_sources(app PRIVATE src/master.cpp)
  # master.cpp is guarded by the backend choice and compiles to nothing here.
  target_sources_ifdef(CONFIG_HCP_MODBUS_BACKEND_NATIVE app PRIVATE
                       src/rtu.cpp src/zephyr/master_native.cpp)
endif()
