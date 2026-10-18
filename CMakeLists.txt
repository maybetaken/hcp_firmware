# Helios Collector Platform (HCP) — Zephyr application
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hcp LANGUAGES C CXX)

# Reusable lib modules (each owns its Kconfig/CMake/tests).
add_subdirectory(lib/kernel)
add_subdirectory(lib/log)
add_subdirectory(lib/input)
add_subdirectory(lib/storage)
add_subdirectory(lib/transport/serial)
add_subdirectory(lib/switch)
add_subdirectory(lib/led)

# Application sources.
target_sources(app PRIVATE src/main.cpp)
