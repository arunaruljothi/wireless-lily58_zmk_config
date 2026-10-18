if(CONFIG_ZMK_SPLIT_USB_POWER)
  zephyr_include_directories(include)

  zephyr_library_named(split_usb_power)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/split_usb_power/central.c)
  else()
    zephyr_library_sources(src/split_usb_power/peripheral.c)
  endif()
endif()
