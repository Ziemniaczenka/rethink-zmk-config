# SPDX-License-Identifier: MIT

if(CONFIG_RETHINK_PAIRING_BUTTON)
  target_sources(app PRIVATE src/pairing_button.c)
endif()

if(CONFIG_RETHINK_PROFILE_LEDS)
  target_sources(app PRIVATE src/profile_leds.c)
endif()
