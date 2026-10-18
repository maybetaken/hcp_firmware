# hcp::kernel module — Zephyr build integration.
# The posix backend is compiled directly by the host test build
# (tests/host/CMakeLists.txt); here we only wire the on-target backend.
# Sources go straight into the `app` target (out-of-tree app: a standalone
# zephyr_library() archive would not land on the final link line).
zephyr_include_directories(include)

if(CONFIG_HCP_KERNEL_BACKEND_ZEPHYR)
  target_sources(app PRIVATE src/zephyr/kernel_zephyr.cpp)
endif()
