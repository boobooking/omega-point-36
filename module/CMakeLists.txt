if(CONFIG_ZMK_LAYOUT_RESYNC)
  zephyr_library_named(layout_resync)
  # app adds its own headers with target_include_directories(app PRIVATE include),
  # and a separately named library does not inherit that. Without this line
  # <zmk/...> resolves only by accident, depending on what other modules set.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/resync_state.c)
  zephyr_library_sources(src/layout_resync.c)
endif()
