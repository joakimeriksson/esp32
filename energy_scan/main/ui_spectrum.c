#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "ieee_scan.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define TAG          "UI"

#define CANVAS_W   320         // X-axis (channels)
#define CANVAS_H   172         // Y-axis (energy)
#define CH_CNT     16
#define COL_W      (CANVAS_W / CH_CNT)   // 20 px per channel slice
#define SLICE_W  (CANVAS_W / CH_CNT)  // ≈10 px per channel

// --- UI objects ---
static lv_obj_t     *canvas;
static lv_color_t   *cbuf;
static lv_obj_t     *channel_label;
static lv_obj_t     *chart;
static lv_chart_series_t *chart_series;

// --- UI State ---
static SemaphoreHandle_t button_sem;
static scan_mode_t ui_mode = SCAN_MODE_SWEEP;

SemaphoreHandle_t ui_get_button_semaphore(void) {
    return button_sem;
}

static void clear_screen() {
    lv_obj_clean(lv_scr_act());
    canvas = NULL;
    cbuf = NULL;
    channel_label = NULL;
    chart = NULL;
    chart_series = NULL;
}

static void ui_spectrum_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,  LV_OPA_COVER, LV_PART_MAIN);

    lv_coord_t w = lv_disp_get_hor_res(NULL);
    lv_coord_t h = lv_disp_get_ver_res(NULL);

    size_t buf_size = (size_t)w * (size_t)h * sizeof(lv_color_t);
    cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    assert(cbuf);

    canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas, cbuf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, w, h);
    lv_obj_set_pos(canvas, 0, 0);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    channel_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(channel_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(channel_label, 10, 200);
    lv_obj_set_style_transform_angle(channel_label, 2700, LV_PART_MAIN);
    lv_label_set_text(channel_label, "Scanning All Channels");
}

static void ui_chart_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,  LV_OPA_COVER, LV_PART_MAIN);

    lv_coord_t w = lv_disp_get_hor_res(NULL);
    lv_coord_t h = lv_disp_get_ver_res(NULL);

    chart = lv_chart_create(scr);

    lv_obj_set_size(chart, h, w - 15);
    lv_obj_set_pos(chart, 10, h);
    lv_obj_set_style_transform_angle(chart, 2700, LV_PART_MAIN);
//    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, LV_PART_MAIN);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -110, -20);
    lv_chart_set_point_count(chart, 150);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_chart_set_div_line_count(chart, 8, 16);
    lv_obj_set_style_line_color(chart, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    
    chart_series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);

    channel_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(channel_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(channel_label, 10, 200);
    lv_obj_set_style_transform_angle(channel_label, 2700, LV_PART_MAIN);
    lv_label_set_text(channel_label, "Scanning Channel: 11");
}

static inline int map_dbm_to_height(int8_t pwr_dbm) {
    int p = pwr_dbm < -100 ? -100 : pwr_dbm > -20 ? -20 : pwr_dbm;
    return ((-p - 20) * CANVAS_H) / 80;
}

static void draw_channel(uint8_t ch, int8_t pwr) {
    int idx = ch - CH_FIRST;
    if (idx < 0 || idx >= CH_CNT || !canvas) return;

    int x = (CH_CNT - idx) * SLICE_W + SLICE_W/2;
    int h = map_dbm_to_height(pwr);
    int y0 = h;
    int y1 = CANVAS_H - 1;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = 8;
    dsc.color = lv_palette_main(LV_PALETTE_RED);
    lv_point_t pts[2] = { { y0, x }, { y1, x } };
    lv_canvas_draw_line(canvas, pts, 2, &dsc);
}

static void clear_slice(int idx)
{
    if (!canvas) return;
    lv_coord_t x = (CH_CNT - idx) * COL_W;
    lv_coord_t y = 0;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_black();
    dsc.bg_opa   = LV_OPA_COVER;
    lv_canvas_draw_rect(canvas, y, x, CANVAS_H, COL_W, &dsc);
}

void ui_task(void *arg)
{
    button_sem = xSemaphoreCreateBinary();
    ui_spectrum_create();
    ieee_scan_set_mode(SCAN_MODE_SWEEP, 0);

    QueueHandle_t q = ieee_scan_get_queue();
    ed_point_t pt;

    for (;;) {
        if (xSemaphoreTake(button_sem, 0) == pdTRUE) {
            if (ui_mode == SCAN_MODE_SWEEP) {
                ui_mode = SCAN_MODE_SINGLE_CHANNEL;
                clear_screen();
                ui_chart_create();
                // When switching to single channel mode, start with CH_FIRST
                ieee_scan_set_mode(SCAN_MODE_SINGLE_CHANNEL, CH_FIRST);
                lv_label_set_text_fmt(channel_label, "Scanning Channel: %d", CH_FIRST);
            } else { // ui_mode == SCAN_MODE_SINGLE_CHANNEL
                // Check if we should switch back to sweep mode or cycle channel
                if (s_single_channel == CH_LAST) { // If we are at the last channel, switch back to sweep
                    ui_mode = SCAN_MODE_SWEEP;
                    clear_screen();
                    ui_spectrum_create();
                    ieee_scan_set_mode(SCAN_MODE_SWEEP, 0);
                    lv_label_set_text(channel_label, "Scanning All Channels");
                } else { // Cycle to next channel
                    s_single_channel++;
                    lv_label_set_text_fmt(channel_label, "Scanning Channel: %d", s_single_channel);
                    ieee_scan_set_mode(SCAN_MODE_SINGLE_CHANNEL, s_single_channel);
                    // Clear chart data for new channel
                    if (chart) {
                        lv_chart_set_point_count(chart, 0); // Clear points
                        lv_chart_set_point_count(chart, 150); // Reset points
                    }
                }
            }
        }

        /* Loop here to ensure to get some of the queue reviced quickly */
        for (int i = 0; i < 10 && xQueueReceive(q, &pt, 0) == pdTRUE; i++) {
            if (ui_mode == SCAN_MODE_SWEEP) {
                int idx = pt.ch - 11;
                if (idx >= 0 && idx < CH_CNT) {
                    clear_slice(idx);
                    draw_channel(pt.ch, pt.pwr);
                }
            } else { // Single channel mode
                if (chart_series) {
                    lv_chart_set_next_value(chart, chart_series, pt.pwr);
                }
            }
        }
        ESP_LOGI(TAG, "Scan mode: %d, channel: %d, power: %d", ui_mode, pt.ch, pt.pwr);
 
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

