# A WiFi scanner for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5

An ESP-IDF v5.5.4 app for the ESP32-P4 rev >= 3 on Waveshare's ESP32-P4-WIFI6-Touch-LCD-3.5: the
P4 scans through the board's ESP32-C6 (ESP-Hosted 2.12.13 and esp_wifi_remote) and shows it with
LVGL 9 on the 320 x 480 ST7796 in three screens, swiped between:

- **Networks**: the strongest RSSI on each 2.4 GHz channel 1-13 as bars, and every network heard
  (SSID, channel, RSSI, security).
- **Spectrum**: the band 2400-2484 MHz, one point per MHz, rebuilt from the scan: each network over
  the band it occupies (centre 2407 + 5 x channel MHz, 20 MHz wide, or 40 MHz towards its
  secondary channel), shaped by the OFDM transmit mask (flat to 9 MHz from the centre, -20 dB at
  11, -28 at 20, -40 at 30 for 20 MHz) and summed in mW; below it a waterfall of that spectrum, the
  newest scan on top. It is not measured energy: it shows the networks the C6 decoded, at their
  beacon RSSI, and nothing else on the air. (ESP-Hosted 2.12.13 does not forward promiscuous mode
  or CSI, and the C6's 802.15.4 energy detect would need firmware of our own on it.)

- **Bluetooth**: the BLE devices advertising around, by RSSI: the name from the advertisement or
  its scan response, else the manufacturer from the manufacturer-specific data's company identifier
  (Bluetooth SIG Assigned Numbers: Apple, Microsoft, Samsung, Google and a few more by name), the
  last three address bytes, RSSI and seconds since last heard; devices not heard for 30 s drop out.
  NimBLE runs on the P4 and reaches the C6's controller over ESP-Hosted's virtual HCI
  (`CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI`), as ESP-Hosted's `host_nimble_bleprph_host_only_vhci`.

Only the page on screen scans, so the C6 never splits its radio between WiFi and BLE: the WiFi
pages run active WiFi scans back to back (a full scan takes about 1.3 s on the board), the
Bluetooth page an active BLE scan with the window as long as the interval. **Rescan** starts a WiFi
scan at once. On the console: `scan N: M networks (scan .. ms, records .. ms)`, one line per
network (`+`/`-` after the channel for a secondary channel above or below), `spectrum,` with the
85 values in dBm, and every 5 s `ble: N devices, .. named, .. with a manufacturer only`.

The board's C6 runs Waveshare's factory ESP-Hosted firmware (it reports version 0.0.0): it does not
answer the request to start its BT controller (`Req_FeatureControl`), but its controller runs from
boot, so the app starts NimBLE either way.

The board comes from Waveshare's BSP `waveshare/esp32_p4_wifi6_touch_lcd_3_5` 2.0.2 (Apache-2.0):
the panel on SPI2 with DMA (MOSI 20, CLK 21, CS 23, DC 26, RST 27), the LEDC backlight on GPIO 28,
FT5x06 touch on I2C (SCL 8, SDA 7, RST 29, INT 50); LVGL draws into a 50-line buffer in internal
RAM, without PSRAM or the PPA. `dependencies.lock` pins the components.

    idf.py set-target esp32p4 build      # rev >= 3 (IDF's default), 4 MB flash, single large app
    idf.py -b 230400 flash monitor

Flash at 230400 baud: on this board 460800 and 921600 stop part way ("The chip stopped responding").

Checked on the board (v3.2 chip, the C6 with its factory ESP-Hosted firmware): the panel and touch
come up, the C6 is identified, a WiFi scan takes about 1.3 s, and the spectrum shows the networks around
it as humps on channels 1, 6 and 11. The app is public
domain (Unlicense) or CC0, as the ESP-IDF examples it follows.
