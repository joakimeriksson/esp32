# the flasher without a screen or a board: the worker, the USB-MIDI host, WiFi and HTTPS, the catalog and
# the card mirror. The app gives it a screen (ui.h) and a board (board.h).
idf_component_register(SRCS "usb_midi.c" "net.c" "catalog.c" "flasher.c"
                       INCLUDE_DIRS "."
                       REQUIRES fm1 esp_wifi esp_netif esp_event nvs_flash esp_http_client esp-tls mbedtls json fatfs sdmmc usb esp_timer)
