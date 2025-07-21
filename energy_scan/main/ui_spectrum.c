#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ieee_scan.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define TAG          "UI"

#define CANVAS_W   320         // X-axis (channels)
#define CANVAS_H   172         // Y-axis (energy)
#define CH_CNT     16
#define COL_W      (CANVAS_W / CH_CNT)   // 20 px per channel slice
#define SLICE_W  (CANVAS_W / CH_CNT)  // ≈10 px per channel
#define SLICE_H (CANVAS_H / CH_CNT)   // height per channel


#include "lvgl.h"

static lv_obj_t     *canvas;
static lv_color_t   *cbuf;                 /* RGB565 pixel buffer */
static lv_obj_t     *channel_label;

// Clamp v to the interval [lo, hi]
#define CLAMP(v, lo, hi)  \
    ((v) < (lo) ? (lo) :   \
     (v) > (hi) ? (hi) :   \
                 (v))

void ui_canvas_create(void)
{
    /* 1. Black out the base screen */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,  LV_OPA_COVER, LV_PART_MAIN);
 
    lv_coord_t w = lv_disp_get_hor_res(NULL);  // e.g. 172
    lv_coord_t h = lv_disp_get_ver_res(NULL);  // e.g. 320

    /* 1. Allocate exactly w*h pixels of lv_color_t */
    size_t buf_size = (size_t)w * (size_t)h * sizeof(lv_color_t);
    cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    assert(cbuf);

    printf("Size of buffer: %zu\n", buf_size);
    printf("Width: %d\n", w);
    printf("Height: %d\n", h);
    printf("Color size: %d\n", sizeof(lv_color_t));

    /* 2. Create the canvas and attach your buffer */
    canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas, cbuf, w, h, LV_IMG_CF_TRUE_COLOR);

    /* 3. Make it cover the full screen */
    lv_obj_set_size(canvas, w, h);
    lv_obj_set_pos(canvas, 0, 0);

    /* 4. Clear it once to black */
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    channel_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(channel_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(channel_label, 10, 200); // Adjust position as needed
    lv_coord_t label_w = lv_obj_get_width(channel_label);
    lv_coord_t label_h = lv_obj_get_height(channel_label);
    lv_obj_set_style_transform_pivot_x(channel_label, label_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(channel_label, label_h / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(channel_label, 2700, LV_PART_MAIN); // Rotate by 270 degrees (2700 = 270.0 degrees)
    lv_label_set_text(channel_label, "Scanning Channel: --");
}


static inline int map_dbm_to_height(int8_t pwr_dbm) {
    // clamp into [-100…-20]
    int p = pwr_dbm < -100 ? -100
          : pwr_dbm >  -20 ? -20
          : pwr_dbm;
    // map: -20 → zero, -100 → full height
    return ((-p - 20) * CANVAS_H) / 80;
}

static void draw_channel(uint8_t ch, int8_t pwr) {
    int idx = ch - CH_FIRST;
    if (idx < 0 || idx >= CH_CNT) return;

    // X coordinate: one slice per channel, line drawn in its center
    int x = (CH_CNT - idx) * SLICE_W + SLICE_W/2;

    // Compute line height (0…CANVAS_H) and invert for Y0
    int h = map_dbm_to_height(pwr);
    int y0 = h;        // (-100 => full height = no dot, -20 => zero height = full line)
    int y1 = CANVAS_H - 1;      // bottom

    ESP_LOGI("UI", "ch=%u pwr=%d → idx=%d h=%d x=%d y0=%d", ch, pwr, idx, h, x, y0);

    // Draw a vertical line
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = 8;
    dsc.color = lv_palette_main(LV_PALETTE_RED);

    lv_point_t pts[2] = { { y0, x }, { y1, x } };
    lv_canvas_draw_line(canvas, pts, 2, &dsc);
}

static void clear_slice(int idx)
{
    /* Calculate slice position */
    lv_coord_t x = (CH_CNT - idx) * COL_W;
    lv_coord_t y = 0;

    /* Build a simple rect-descriptor that paints black */
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_black();
    dsc.bg_opa   = LV_OPA_COVER;

    /* v8 & v9 API: (canvas, x, y, w, h, &dsc) */
    lv_canvas_draw_rect(canvas, y, x, CANVAS_H, COL_W, &dsc);
    //lv_canvas_draw_rect(canvas, 0, 0, CANVAS_H, CANVAS_W, &dsc);
}

void ui_spectrum_task(void *arg)
{
    ui_canvas_create();
    lv_timer_handler();         /* first flush */
    vTaskDelay(pdMS_TO_TICKS(100)); // Give LVGL time to process UI creation

    QueueHandle_t q = ieee_scan_get_queue();
    ed_point_t pt;

    for (;;) {
        /* non-blocking receive so we can refresh LVGL */
        int i;
        for (i = 0; i < 16; i++) {
            if (xQueueReceive(q, &pt, 0) == pdTRUE) {
                int idx = pt.ch - 11;
                if (idx >= 0 && idx < CH_CNT) {
                    clear_slice(idx);
                    draw_channel(pt.ch, pt.pwr);
                    lv_label_set_text_fmt(channel_label, "Scanning Channel: %d", pt.ch);
                }
            }
        }

        /* let LVGL push the new pixels (5-10 ms tick) */
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

