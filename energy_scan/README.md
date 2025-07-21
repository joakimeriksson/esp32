# ESP32 IEEE 802.15.4 Energy Scan with LVGL UI

This project implements an IEEE 802.15.4 (e.g., Zigbee/Thread) energy detection scanner for ESP32, displaying the energy levels across different channels on an LCD using the LVGL graphics library.

## Overview

The application continuously scans IEEE 802.15.4 channels (11-26) for energy levels and presents a real-time spectrum visualization on a connected display. This can be useful for identifying interference or assessing channel utilization in 2.4 GHz ISM band environments.

## Features

*   **IEEE 802.15.4 Energy Detection:** Scans channels 11-26 for received signal strength (RSSI).
*   **Real-time Spectrum Display:** Visualizes energy levels per channel using LVGL on an LCD.
*   **ESP-IDF Framework:** Built using the Espressif IoT Development Framework.

## Hardware Requirements
I have used the ESP32-c6-LCD1.47 from Waveshare.

*   **ESP32-C6 Development Board:** The project is configured for the ESP32-C6.
*   **ST7789 Compatible LCD:** A display module compatible with the ST7789 driver (e.g., 240x320 or 172x320 resolution).
*   **RGB LED (Optional):** The code includes initialization for an RGB LED.

## Software Requirements

*   [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/index.html) (Espressif IoT Development Framework)
*   Properly configured toolchain for ESP32-C6.

## Building and Flashing

1.  **Clone the repository:**
    ```bash
    git clone <repository_url>
    cd energy_scan
    ```
2.  **Set up ESP-IDF environment:**
    Follow the official ESP-IDF "Get Started" guide for your operating system and ESP32-C6.
3.  **Configure the project target**
    ```bash
    idf.py set-target esp32c6
    ```

4.  **Build the project:**
    ```bash
    idf.py build
    ```
5.  **Flash to the device:**
    ```bash
    idf.py flash monitor
    ```

## How It Works

The application operates with two primary FreeRTOS tasks:

1.  **`ieee_sweep_task`**: This task is responsible for continuously initiating IEEE 802.15.4 energy detection scans. It iterates through channels 11-26, triggering the radio to perform an energy detect operation on each. The results (channel and power level) are sent to a FreeRTOS queue.
2.  **`ui_spectrum_task`**: This task initializes the LVGL display and continuously reads energy detection results from the queue. It then updates the LVGL canvas, drawing vertical lines whose height corresponds to the detected energy level for each channel, providing a real-time spectrum visualization.
