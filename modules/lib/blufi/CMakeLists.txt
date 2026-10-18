# BluFi module — reusable BLE provisioning protocol (ESP BluFi compatible).
zephyr_include_directories(include)
if(CONFIG_BLUFI)
  zephyr_library()
  zephyr_library_sources(frame.c)
  zephyr_library_sources_ifdef(CONFIG_BT_PERIPHERAL blufi_ble.c)
endif()
