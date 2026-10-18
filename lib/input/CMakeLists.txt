# hcp::input module — Zephyr build integration.
# debounce.cpp is pure logic (also built by host tests); button_zephyr.cpp target-only.
zephyr_include_directories(include)
target_sources(app PRIVATE src/debounce.cpp)
if(CONFIG_HCP_INPUT)
  target_sources(app PRIVATE src/zephyr/button_zephyr.cpp)
endif()
