# Contiki-NG on ESP32 Example

This project demonstrates how to run Contiki-NG examples on ESP32 targets using the ESP-IDF framework. It is configured to run the `rpl-udp` client example, which makes the ESP32 device act as a UDP client in an RPL-based IPv6 network. The device will join the network, find a RPL root (udp-server), and periodically send UDP packets to the server and receive responses.

| Supported Targets | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- |

## How to Use Example

Before project configuration and build, be sure to set the correct chip target using `idf.py set-target <chip_name>`. Note that this example requires a target with an 802.15.4 radio.

### Hardware Required

*   A development board with a supported ESP32 System-on-Chip (e.g., ESP32-C6-DevKitC).
*   A USB cable for power supply and programming.
*   Any other IoT device acting as root running the corresponding Contiki-NG `udp-server` example to communicate with.

### Configure the Project

You can configure project-specific settings by creating a `project-conf.h` file in the `main` directory. This file can be used to override default Contiki-NG configurations.

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash, and monitor the project.

(To exit the serial monitor, type `Ctrl-]`.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

## Example Output

Once the device joins the 802.15.4 network, it will start sending UDP packets to the border router (server) and printing log messages about the communication. You will see output similar to the following:

```text
I (4525) App: Sending request 1 to fd00::212:4b00:14b5:d309
I (4535) App: UDP: src=34817  dst=23182
I (5755) App: Received response 'Hello from server' from fd00::212:4b00:14b5:d309
```

## Troubleshooting

*   If the device is not sending UDP packets, ensure it is in range of the 802.15.4 border router.
*   Verify that the border router is running and configured correctly with the same network keys.
*   Check the serial monitor output for error messages from the network stack.
*   Turn on DEBUG in the project-conf.h file.