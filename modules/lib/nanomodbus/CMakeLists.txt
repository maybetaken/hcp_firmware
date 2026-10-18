# nanoMODBUS as a Zephyr module (reusable across projects).
zephyr_include_directories(.)
if(CONFIG_NANOMODBUS)
  zephyr_library()
  zephyr_library_sources(nanomodbus.c)
endif()
