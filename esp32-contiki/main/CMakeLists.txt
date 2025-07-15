idf_component_register(
    SRCS      "contiki_example_main.c"
    INCLUDE_DIRS  "."
    PRIV_REQUIRES ieee802154   # ← tell CMake we need the 802.15.4 driver
                  driver
                  nvs_flash
                  contiki-ng-esp32c6
)
