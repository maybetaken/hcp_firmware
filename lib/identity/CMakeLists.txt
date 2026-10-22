# hcp::identity module — Zephyr build integration.
# identity.cpp holds the pure formatters (also built by the host tests);
# identity_zephyr.cpp does the storage/MAC resolution and pushes the hostname.
zephyr_include_directories(include)
target_sources(app PRIVATE src/identity.cpp)
if(CONFIG_HCP_IDENTITY)
  target_sources(app PRIVATE src/zephyr/identity_zephyr.cpp)
endif()
