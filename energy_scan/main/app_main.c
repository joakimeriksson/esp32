#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ST7789.h"
#include "RGB.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"

#define BOOT_BUTTON_GPIO 9

extern void ieee_scan_start(void);
extern void ui_task(void*);
extern SemaphoreHandle_t ui_get_button_semaphore(void);

// Semaphore to signal the button press
static SemaphoreHandle_t button_sem;

// Task to handle the button press
static void button_task(void* arg)
{
    for(;;) {
        if(xSemaphoreTake(button_sem, portMAX_DELAY) == pdTRUE) {
            printf("BOOT button pressed!\n");
            xSemaphoreGive(ui_get_button_semaphore());
        }
    }
}

// ISR handler for the button press
static void IRAM_ATTR button_isr_handler(void* arg)
{
    xSemaphoreGiveFromISR(button_sem, NULL);
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create the semaphore
    button_sem = xSemaphoreCreateBinary();

    // Configure the BOOT button
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_NEGEDGE; // Interrupt on falling edge
    io_conf.pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // Install ISR service and add handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr_handler, NULL);


    RGB_Init();
    RGB_Example();

    LCD_Init();
    BK_Light(50);
    LVGL_Init();

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    assert(err == ESP_OK);

//    Lvgl_Example1();
//    lv_disp_t *disp = lv_disp_get_default();
//    lv_disp_set_rotation(disp, LV_DISP_ROT_90);

    ieee_scan_start();
    xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);


    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }
}
