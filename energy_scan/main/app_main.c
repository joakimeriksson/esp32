#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ST7789.h"
#include "RGB.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"

extern void ieee_scan_start(void);
extern void ui_spectrum_task(void*);

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();                
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());          
        ret = nvs_flash_init();                      
    }
    ESP_ERROR_CHECK(ret);

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
    xTaskCreate(ui_spectrum_task, "ui", 4096, NULL, 4, NULL);

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }
}
