# Standalone Zephyr ztest app exercising the hcp::storage Zephyr (settings)
# backend on native_sim with the SETTINGS_NONE backend (no flash needed).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hcp_storage_ztest)

get_filename_component(HCP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

target_include_directories(app PRIVATE ${HCP_ROOT}/lib/storage/include)
target_sources(app PRIVATE
  src/main.cpp
  ${HCP_ROOT}/lib/storage/src/zephyr/store_zephyr.cpp)
