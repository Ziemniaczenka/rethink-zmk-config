# Copyright (c) 2026 Ziemniaczenka
# SPDX-License-Identifier: MIT

zephyr_include_directories(include)

if(CONFIG_RETHINK_PAIRING_BUTTON)
  target_sources(app PRIVATE src/pairing_button.c)
endif()

if(CONFIG_RETHINK_PROFILE_LEDS)
  target_sources(app PRIVATE src/profile_leds.c)
endif()

if(CONFIG_RETHINK_ADAPTER)
  target_sources(app PRIVATE src/adapter.c src/expander.c)
endif()

if(CONFIG_RETHINK_MATRIX)
  target_sources(app PRIVATE src/matrix.c)
endif()

if(CONFIG_RETHINK_ADAPTER_TEST)
  target_sources(app PRIVATE src/adapter_test.c)
endif()

if(CONFIG_RETHINK_BACKLIGHT)
  target_sources(app PRIVATE src/backlight.c)
endif()

if(CONFIG_RETHINK_WIRED_OFF)
  target_sources(app PRIVATE src/wired_off.c)
endif()
