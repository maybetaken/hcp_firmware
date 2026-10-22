# ota_client module — Zephyr build integration
#
# Copyright (c) 2026 Helios Collector Platform contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(include)

if(CONFIG_OTA_CLIENT)
  zephyr_library()
  zephyr_library_sources(src/ota_util.c src/ota_client.c)
  zephyr_library_sources_ifdef(CONFIG_OTA_CLIENT_SOURCE_HTTP src/ota_source_http.c)
  zephyr_library_sources_ifdef(CONFIG_OTA_CLIENT_SINK_MCUBOOT src/ota_sink_mcuboot.c)
endif()
