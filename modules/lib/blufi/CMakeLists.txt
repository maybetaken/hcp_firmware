# BluFi module — reusable BLE provisioning protocol (ESP BluFi compatible).
zephyr_include_directories(include)
if(CONFIG_BLUFI)
  zephyr_library()
  zephyr_library_sources(frame.c)
  zephyr_library_sources_ifdef(CONFIG_BT_PERIPHERAL blufi_ble.c)
  if(CONFIG_BLUFI_SECURITY)
    # Vendored mbedtls-2.4 DHM/bignum (isolated namespace) + self-contained MD5/AES-CFB.
    # blufi_dhm_compat.h (-include) redirects MBEDTLS_CONFIG_FILE to the private minimal
    # config so these don't pick up the app-wide TF-PSA-Crypto config. Third-party sources
    # are compiled with warnings suppressed (they predate Zephyr's -Werror flags).
    set(_blufi_compat "-include ${CMAKE_CURRENT_SOURCE_DIR}/sec/blufi_dhm_compat.h")
    set_source_files_properties(sec/vendor/dhm.c sec/vendor/bignum.c
                                PROPERTIES COMPILE_FLAGS "-w ${_blufi_compat}")
    set_source_files_properties(sec/blufi_security.c
                                PROPERTIES COMPILE_FLAGS "${_blufi_compat}")
    zephyr_library_sources(sec/blufi_security.c sec/vendor/dhm.c sec/vendor/bignum.c)
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/sec ${CMAKE_CURRENT_SOURCE_DIR}/sec/vendor)
  endif()
endif()
