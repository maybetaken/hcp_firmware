# hcp::modbus module — Zephyr build integration.
# Thin C++ facade over Zephyr's own Modbus RTU client (subsys/modbus). The UART and
# the RS485 direction pin come from the `zephyr,modbus-serial` devicetree node.
zephyr_include_directories(include)
if(CONFIG_HCP_MODBUS)
  # The subsys backend's implementation of the facade; it excludes itself with an
  # #if when the native backend is chosen, so listing it always is harmless.
  target_sources(app PRIVATE src/master.cpp)
  # DL/T645 framing is pure and backend-independent: the collector classifies a
  # control/cmd payload with it whichever backend is running.
  target_sources(app PRIVATE src/dlt645.cpp)
  target_sources_ifdef(CONFIG_HCP_MODBUS_BACKEND_NATIVE app PRIVATE
                       src/rtu.cpp src/zephyr/master_native.cpp)

  # MODBUS_ROLE_CLIENT is a choice symbol, so nothing can select it for a build that
  # picks the subsys backend - without this stop the subsys would come up with no
  # role at all and fail at run time on the far side of a USB cable.
  if(CONFIG_HCP_MODBUS_BACKEND_SUBSYS AND NOT CONFIG_MODBUS_ROLE_CLIENT)
    message(FATAL_ERROR
      "HCP_MODBUS_BACKEND_SUBSYS needs CONFIG_MODBUS_ROLE_CLIENT=y in prj.conf "
      "(Kconfig choices cannot be selected). The default native backend does not.")
  endif()
endif()
