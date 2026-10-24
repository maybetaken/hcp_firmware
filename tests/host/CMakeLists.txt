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

# ---- hcp::modbus: no host test ----
# It used to be tested here over posix serial + nanoMODBUS. The backend is now
# Zephyr's own Modbus subsys, which cannot be compiled for the host, so the
# remaining façade logic (interface lookup from devicetree, parameter mapping,
# request length checks) is covered on real hardware instead: build with
# CONFIG_HCP_MODBUS_DEMO=y and read the log (see lib/modbus/Kconfig). test_serial
# above still covers the UART path the loopback self-test uses.

# ---- hcp::registry (point table, download rules) ----
set(RG ${HCP_ROOT}/lib/registry)
add_executable(test_registry ${RG}/src/table.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_registry.cpp)
target_include_directories(test_registry PRIVATE ${RG}/include)
target_link_libraries(test_registry PRIVATE GTest::gtest_main)
add_test(NAME test_registry COMMAND test_registry)

# ---- hcp::collector topic mapping (pure part) ----
set(CC ${HCP_ROOT}/lib/collector)
add_executable(test_collector ${CC}/src/commands.cpp
              ${CMAKE_CURRENT_LIST_DIR}/test_collector.cpp)
target_include_directories(test_collector PRIVATE
  ${CC}/include ${HCP_ROOT}/lib/registry/include)
target_link_libraries(test_collector PRIVATE GTest::gtest_main)
add_test(NAME test_collector COMMAND test_collector)

# ---- hcp::blufi frames + security handshake ----
set(BF ${HCP_ROOT}/modules/lib/blufi)

# The crypto is the Mbed TLS submodule this library carries, not a copy inside it;
# resolve it the same way the firmware build does (submodule first, then a west
# module location for workspaces that provide it that way).
set(BLUFI_MBEDTLS_DIR "" CACHE PATH "Mbed TLS 2.28 source tree for the BluFi tests")
if(BLUFI_MBEDTLS_DIR STREQUAL "")
  foreach(_cand ${BF}/deps/mbedtls
                 ${ZEPHYR_BASE}/../modules/crypto/mbedtls-2.28
                 /opt/toolchains/modules/crypto/mbedtls-2.28)
    if(EXISTS ${_cand}/library/dhm.c)
      set(BLUFI_MBEDTLS_DIR ${_cand})
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS ${BLUFI_MBEDTLS_DIR}/library/dhm.c)
  message(FATAL_ERROR "BluFi security test needs the Mbed TLS submodule; run "
          "'git submodule update --init --depth 1 deps/mbedtls' in ${BF}, or pass "
          "-DBLUFI_MBEDTLS_DIR=<checkout>")
endif()

# platform_util.c is only in this list, not the firmware one: on target the
# application already links mbedtls_platform_zeroize from TF-PSA-Crypto, and a
# second definition there would be a duplicate symbol.
add_executable(test_blufi ${BF}/frame.c ${BF}/sec/blufi_security.c
              ${BLUFI_MBEDTLS_DIR}/library/platform_util.c
              ${BLUFI_MBEDTLS_DIR}/library/dhm.c
              ${BLUFI_MBEDTLS_DIR}/library/bignum.c
              ${BLUFI_MBEDTLS_DIR}/library/constant_time.c
              ${BLUFI_MBEDTLS_DIR}/library/aes.c
              ${BLUFI_MBEDTLS_DIR}/library/md5.c
              ${CMAKE_CURRENT_LIST_DIR}/test_blufi.cpp)
target_include_directories(test_blufi PRIVATE ${BF}/include ${BF}/sec
                         ${BLUFI_MBEDTLS_DIR}/include)
# Upstream Mbed TLS is not written against this project's warning set.
target_compile_options(test_blufi PRIVATE -w)
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
