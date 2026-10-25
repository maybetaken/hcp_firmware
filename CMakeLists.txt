# Helios Collector Platform (HCP) — Zephyr application
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# In-tree Zephyr modules (reusable components). Add each module dir (the one
# containing zephyr/module.yml) before find_package.
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/modules/drivers/ws2812_rmt
  ${CMAKE_CURRENT_LIST_DIR}/modules/lib/blufi
  ${CMAKE_CURRENT_LIST_DIR}/modules/lib/ota_client)

# A module's devicetree bindings have to be announced separately from the module
# itself: ZEPHYR_EXTRA_MODULES wires up its CMakeLists and Kconfig, but dts roots are
# collected from the west manifest, and this application is not a west module. Verified
# the hard way - with only the line above, a node using the module's compatible got its
# properties ignored (no DT_HAS_* symbol, so the driver's Kconfig was never selectable)
# and the failure surfaced much later as an undefined device object at link time.
# pre_dt.cmake documents DTS_ROOT as "initial contents may be populated here".
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/modules/drivers/ws2812_rmt)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hcp LANGUAGES C CXX)

# Reusable lib modules (each owns its Kconfig/CMake/tests).
add_subdirectory(lib/kernel)
add_subdirectory(lib/log)
add_subdirectory(lib/input)
add_subdirectory(lib/storage)
add_subdirectory(lib/transport/serial)
add_subdirectory(lib/modbus)
add_subdirectory(lib/registry)
add_subdirectory(lib/collector)
add_subdirectory(lib/switch)
add_subdirectory(lib/led)
add_subdirectory(lib/net)
add_subdirectory(lib/identity)
add_subdirectory(lib/provision)
add_subdirectory(lib/mqtt)
add_subdirectory(lib/ssdp)
add_subdirectory(lib/ota)

# Application. main.cpp is only the entry point: the start-up sequence is
# hcp-app.cpp, and the collaborators it drives are the app_*.cpp beside it.
target_sources(app PRIVATE
  src/main.cpp
  src/hcp-app.cpp
  src/app_demos.cpp
  src/app_blufi.cpp
  src/app_services.cpp)
