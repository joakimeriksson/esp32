/* Contiki example main file for ESP32 platform 
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <assert.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

#include "nvs_flash.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#include "contiki.h"
#include "contiki-net.h"
#include "net/ipv6/simple-udp.h"
#include "radio.h"

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define SEND_INTERVAL             (10 * CLOCK_SECOND)


extern void contiki_task(void *arg); // Forward declaration for Contiki task

static const char *TAG = "contiki-example";

#define UART_PORT UART_NUM_0
#define UART_RX_BUF_SIZE 256

/* Test reception of UART - if we want to add commands later... */
static void uart_rx_task(void *arg)
{
    uint8_t* data = (uint8_t*) malloc(UART_RX_BUF_SIZE+1);
    while (1) {
        int len = uart_read_bytes(UART_PORT, data, UART_RX_BUF_SIZE, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            ESP_LOGI(TAG, "Received: %s", (char*)data);
            // Here you can add your command parsing logic
            // For example, if (strcmp((char*)data, "blink") == 0) { s_led_state = !s_led_state; }
        }
    }
}
static uint8_t s_led_state = 0;

#define BLINK_GPIO CONFIG_BLINK_GPIO

static led_strip_handle_t led_strip;

static void blink_led(void)
{
    /* If the addressable LED is enabled */
    if (s_led_state) {
        /* Set the LED pixel using RGB from 0 (0%) to 255 (100%) for each color */
        led_strip_set_pixel(led_strip, 0, 16, 16, 16);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    } else {
        /* Set all LED off to clear all pixels */
        led_strip_clear(led_strip);
    }
}

static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED!");
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // at least one LED on board
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();                
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());          
        ret = nvs_flash_init();                      
    }
    ESP_ERROR_CHECK(ret);  // makes warning fatal in debug

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_rx_task, "uart_rx_task", 2048, NULL, 10, NULL);

    /* Configure the peripheral according to the LED type */
    configure_led();

    /* Full debug on Radio */
    esp_log_level_set("ESP RADIO", ESP_LOG_DEBUG); 

    /* Start Contiki Task */
    xTaskCreate(contiki_task, "contiki", 8192, NULL, 5, NULL);

    while (1) {
        blink_led();
        /* Toggle the LED state */
        s_led_state = !s_led_state;

        vTaskDelay(pdMS_TO_TICKS(1));
        /* Wait for the transmission to complete */
        vTaskDelay(pdMS_TO_TICKS(1));
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}
