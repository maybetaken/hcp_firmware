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

# ---- hcp::modbus (nanoMODBUS module over posix serial) ----
set(M ${HCP_ROOT}/lib/modbus)
set(NM ${HCP_ROOT}/modules/lib/nanomodbus)
add_executable(test_modbus
  ${NM}/nanomodbus.c
  ${M}/src/master.cpp
  ${HCP_ROOT}/lib/transport/serial/src/posix/serial_posix.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test_modbus.cpp)
target_include_directories(test_modbus PRIVATE
  ${M}/include ${NM} ${HCP_ROOT}/lib/transport/serial/include)
target_link_libraries(test_modbus PRIVATE GTest::gtest_main pthread)
add_test(NAME test_modbus COMMAND test_modbus)

# ---- BluFi frame codec + vendored crypto (module) ----
set(BF ${HCP_ROOT}/modules/lib/blufi)
set_source_files_properties(${BF}/sec/vendor/aes.c ${BF}/sec/vendor/md5.c
                            ${BF}/sec/vendor/dhm.c ${BF}/sec/vendor/bignum.c
                            PROPERTIES COMPILE_FLAGS "-w")
add_executable(test_blufi ${BF}/frame.c
              ${BF}/sec/blufi_security.c
              ${BF}/sec/vendor/dhm.c ${BF}/sec/vendor/bignum.c
              ${BF}/sec/vendor/aes.c ${BF}/sec/vendor/md5.c
              ${CMAKE_CURRENT_LIST_DIR}/test_blufi.cpp)
target_include_directories(test_blufi PRIVATE ${BF}/include ${BF}/sec ${BF}/sec/vendor)
target_compile_definitions(test_blufi PRIVATE MBEDTLS_CONFIG_FILE="blufi_dhm_config.h" CONFIG_BLUFI_SECURITY=1)
target_link_libraries(test_blufi PRIVATE GTest::gtest_main)
add_test(NAME test_blufi COMMAND test_blufi)

# ---- hcp::provision state machine ----
set(PV ${HCP_ROOT}/lib/provision)
add_executable(test_provision ${PV}/src/fsm.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_provision.cpp)
target_include_directories(test_provision PRIVATE ${PV}/include)
target_link_libraries(test_provision PRIVATE GTest::gtest_main)
add_test(NAME test_provision COMMAND test_provision)

# ---- hcp::mqtt reconnect backoff (pure logic) ----
set(MQ ${HCP_ROOT}/lib/mqtt)
add_executable(test_mqtt ${MQ}/src/backoff.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_mqtt.cpp)
target_include_directories(test_mqtt PRIVATE ${MQ}/include)
target_link_libraries(test_mqtt PRIVATE GTest::gtest_main)
add_test(NAME test_mqtt COMMAND test_mqtt)

# ---- hcp::ssdp announcement parsing (pure logic) ----
set(SD ${HCP_ROOT}/lib/ssdp)
add_executable(test_ssdp ${SD}/src/discovery.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_ssdp.cpp)
target_include_directories(test_ssdp PRIVATE ${SD}/include)
target_link_libraries(test_ssdp PRIVATE GTest::gtest_main)
add_test(NAME test_ssdp COMMAND test_ssdp)

# ---- hcp::identity serial/hostname formatting (pure logic) ----
set(ID ${HCP_ROOT}/lib/identity)
add_executable(test_identity ${ID}/src/identity.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_identity.cpp)
target_include_directories(test_identity PRIVATE ${ID}/include)
target_link_libraries(test_identity PRIVATE GTest::gtest_main)
add_test(NAME test_identity COMMAND test_identity)

# ---- ota_client module: version ordering and URL parsing (pure logic) ----
set(OTAM ${HCP_ROOT}/modules/lib/ota_client)
add_executable(test_ota_client ${OTAM}/src/ota_util.c
              ${CMAKE_CURRENT_LIST_DIR}/test_ota_client.cpp)
target_include_directories(test_ota_client PRIVATE ${OTAM}/include)
target_link_libraries(test_ota_client PRIVATE GTest::gtest_main)
add_test(NAME test_ota_client COMMAND test_ota_client)
