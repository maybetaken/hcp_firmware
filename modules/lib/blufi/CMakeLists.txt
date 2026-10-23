# BluFi module — reusable BLE provisioning protocol (ESP BluFi compatible).
zephyr_include_directories(include)
if(CONFIG_BLUFI)
  zephyr_library()
  zephyr_library_sources(frame.c)
  zephyr_library_sources_ifdef(CONFIG_BT_PERIPHERAL blufi_ble.c)
  if(CONFIG_BLUFI_SECURITY)
    # The algorithms (DH, bignum, AES-CFB128, MD5) come from Mbed TLS 2.28 LTS
    # sources that the west manifest checks out. This repository keeps no copy:
    # nothing here is edited, and the version is pinned by the manifest. Override
    # the location with -DBLUFI_MBEDTLS_DIR=<path> (the host test build does).
    set(BLUFI_MBEDTLS_DIR "" CACHE PATH
        "Mbed TLS 2.28 source tree used by the BluFi security handshake")
    if(BLUFI_MBEDTLS_DIR STREQUAL "")
      set(BLUFI_MBEDTLS_DIR ${ZEPHYR_BASE}/../modules/crypto/mbedtls-2.28)
    endif()
    if(NOT EXISTS ${BLUFI_MBEDTLS_DIR}/library/dhm.c)
      message(FATAL_ERROR
        "CONFIG_BLUFI_SECURITY needs the Mbed TLS 2.28 sources (library/dhm.c, "
        "bignum.c, aes.c, md5.c) but none were found at\n"
        "  ${BLUFI_MBEDTLS_DIR}\n"
        "Run 'west update', or point -DBLUFI_MBEDTLS_DIR at a 2.28 checkout, or "
        "leave CONFIG_BLUFI_SECURITY=n (plaintext provisioning, the default).")
    endif()

    # Two force-included headers, both owned by us and applied to blufi_security.c
    # as well as to the upstream sources (see sec/mbedtls/CMakeLists.txt): they must
    # agree on the symbol names and on which config those names come from.
    set_source_files_properties(sec/blufi_security.c PROPERTIES COMPILE_FLAGS
        "-I ${BLUFI_MBEDTLS_DIR}/include -include ${CMAKE_CURRENT_SOURCE_DIR}/sec/blufi_dhm_compat.h -include ${CMAKE_CURRENT_SOURCE_DIR}/sec/blufi_mbedtls_rename.h")
    zephyr_library_sources(sec/blufi_security.c)
    # blufi_dhm_config.h is named through MBEDTLS_CONFIG_FILE, so the directory
    # holding it has to be reachable.
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/sec)

    add_subdirectory(sec/mbedtls)
  endif()
endif()
