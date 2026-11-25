# Helios Collector Platform (HCP) — Zephyr application
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# The three reusable modules (blufi, ota_client, ws2812_rmt) are west projects
# now, fetched into the workspace's modules/ tree - west discovers them by their
# zephyr/module.yml, so nothing here needs to name them. What does need naming is
# the devicetree root: dts roots are collected from the west manifest, and this
# application is not a west module. Verified the hard way - without DTS_ROOT, a
# node using the module's compatible got its properties ignored (no DT_HAS_*
# symbol, so the driver's Kconfig was never selectable) and the failure surfaced
# much later as an undefined device object at link time.
#
# ZEPHYR_BASE is only defined INSIDE find_package(Zephyr), so it cannot be used
# here - the workspace root comes from the environment instead, which west sets.
list(APPEND DTS_ROOT $ENV{ZEPHYR_BASE}/../modules/drivers/ws2812_rmt)

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
