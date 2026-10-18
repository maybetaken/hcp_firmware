# Standalone host test build for the hcp lib posix backends.
# Not part of the Zephyr image build — run with a normal Linux toolchain
# (we use the container's gcc/cmake/ninja). FetchContent pulls GoogleTest.
cmake_minimum_required(VERSION 3.20)
project(hcp_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(HCP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.16.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ---- hcp::kernel (posix backend) ----
set(K ${HCP_ROOT}/lib/kernel)
add_executable(test_kernel ${K}/src/posix/kernel_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_kernel.cpp)
target_include_directories(test_kernel PRIVATE ${K}/include)
target_compile_definitions(test_kernel PRIVATE _GNU_SOURCE)
target_link_libraries(test_kernel PRIVATE GTest::gtest_main pthread)
add_test(NAME test_kernel COMMAND test_kernel)

# ---- hcp::log (posix backend) ----
set(L ${HCP_ROOT}/lib/log)
add_executable(test_log ${L}/src/posix/log_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_log.cpp)
target_include_directories(test_log PRIVATE ${L}/include)
target_link_libraries(test_log PRIVATE GTest::gtest_main)
add_test(NAME test_log COMMAND test_log)

# ---- hcp::storage (posix backend) ----
set(S ${HCP_ROOT}/lib/storage)
add_executable(test_storage ${S}/src/posix/store_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_storage.cpp)
target_include_directories(test_storage PRIVATE ${S}/include)
target_link_libraries(test_storage PRIVATE GTest::gtest_main)
add_test(NAME test_storage COMMAND test_storage)

# ---- hcp::input (pure debounce logic) ----
set(I ${HCP_ROOT}/lib/input)
add_executable(test_input ${I}/src/debounce.cpp ${CMAKE_CURRENT_LIST_DIR}/test_input.cpp)
target_include_directories(test_input PRIVATE ${I}/include)
target_link_libraries(test_input PRIVATE GTest::gtest_main)
add_test(NAME test_input COMMAND test_input)

# ---- hcp::serial (posix loopback) ----
set(T ${HCP_ROOT}/lib/transport/serial)
add_executable(test_serial ${T}/src/posix/serial_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_serial.cpp)
target_include_directories(test_serial PRIVATE ${T}/include)
target_link_libraries(test_serial PRIVATE GTest::gtest_main pthread)
add_test(NAME test_serial COMMAND test_serial)

# ---- hcp::switchctl (posix fake) ----
set(W ${HCP_ROOT}/lib/switch)
add_executable(test_switch ${W}/src/posix/relay_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_switch.cpp)
target_include_directories(test_switch PRIVATE ${W}/include)
target_link_libraries(test_switch PRIVATE GTest::gtest_main)
add_test(NAME test_switch COMMAND test_switch)

# ---- hcp::led (posix framebuffer) ----
set(D ${HCP_ROOT}/lib/led)
add_executable(test_led ${D}/src/posix/strip_posix.cpp ${CMAKE_CURRENT_LIST_DIR}/test_led.cpp)
target_include_directories(test_led PRIVATE ${D}/include)
target_link_libraries(test_led PRIVATE GTest::gtest_main)
add_test(NAME test_led COMMAND test_led)

# ---- hcp::modbus (nanoMODBUS over posix serial) ----
set(M ${HCP_ROOT}/lib/modbus)
add_executable(test_modbus
  ${M}/vendor/nanomodbus/nanomodbus.c
  ${M}/src/master.cpp
  ${HCP_ROOT}/lib/transport/serial/src/posix/serial_posix.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test_modbus.cpp)
target_include_directories(test_modbus PRIVATE
  ${M}/include ${M}/vendor/nanomodbus ${HCP_ROOT}/lib/transport/serial/include)
target_link_libraries(test_modbus PRIVATE GTest::gtest_main pthread)
add_test(NAME test_modbus COMMAND test_modbus)
