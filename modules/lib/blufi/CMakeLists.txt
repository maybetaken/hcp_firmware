# BluFi module — reusable BLE provisioning protocol (ESP BluFi compatible).
zephyr_include_directories(include)
if(CONFIG_BLUFI)
  zephyr_library()
  zephyr_library_sources(frame.c)
  zephyr_library_sources_ifdef(CONFIG_BT_PERIPHERAL blufi_ble.c)
  if(CONFIG_BLUFI_SECURITY)
    # The algorithms (DH, bignum, AES-CFB128, MD5) are compiled from Mbed TLS, not
    # from a copy inside this module. Where to find it, in order:
    #   1. -DBLUFI_MBEDTLS_DIR=<path>, if the caller says where;
    #   2. the mbedtls-2.28 west project, for a workspace that provides it - the
    #      consumer that needs Mbed TLS but not this module gets it from there too,
    #      so neither has to reach into the other;
    #   3. deps/mbedtls, the submodule this library carries. This is the standalone
    #      path and it stays: someone who takes just this library runs
    #      `git submodule update --init deps/mbedtls` and builds.
    set(BLUFI_MBEDTLS_DIR "" CACHE PATH
        "Mbed TLS 2.28 source tree used by the BluFi security handshake")
    foreach(_cand ${ZEPHYR_BASE}/../modules/crypto/mbedtls-2.28
                   ${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls)
      if(BLUFI_MBEDTLS_DIR STREQUAL "" AND EXISTS ${_cand}/library/dhm.c)
        set(BLUFI_MBEDTLS_DIR ${_cand})
      endif()
    endforeach()
    if(NOT EXISTS ${BLUFI_MBEDTLS_DIR}/library/dhm.c)
      message(FATAL_ERROR
        "CONFIG_BLUFI_SECURITY needs the Mbed TLS submodule "
        "(library/dhm.c, bignum.c, constant_time.c, aes.c, md5.c); nothing was "
        "found at\n  '${BLUFI_MBEDTLS_DIR}'\n"
        "Populate it with:\n"
        "  git submodule update --init --depth 1 deps/mbedtls\n"
        "from the module directory, or point -DBLUFI_MBEDTLS_DIR at any Mbed TLS "
        "2.28 tree - or leave CONFIG_BLUFI_SECURITY=n (plaintext provisioning, the "
        "default, and the state everything else is tested in).")
    endif()
    message(STATUS "blufi: encrypted provisioning builds Mbed TLS from ${BLUFI_MBEDTLS_DIR}")

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
