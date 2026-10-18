# BluFi module — reusable BLE provisioning protocol (ESP BluFi compatible).
zephyr_include_directories(include)
if(CONFIG_BLUFI)
  zephyr_library()
  zephyr_library_sources(frame.c)
endif()
