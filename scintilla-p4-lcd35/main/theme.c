/* SPDX-License-Identifier: GPL-3.0-only */
/* The look of synthex-web's panel on 480 x 320: dark grey metal between wooden end cheeks, cream
 * membrane buttons with a red LED, white section titles on a rule, orange where something is held. */
#include "theme.h"

#define CHEEK 8

void theme_screen(lv_obj_t *scr)
{
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    for (int side = 0; side < 2; side++) {
        lv_obj_t *w = lv_obj_create(scr);
        lv_obj_remove_style_all(w);
        lv_obj_set_size(w, CHEEK, 320);
        lv_obj_set_pos(w, side ? 480 - CHEEK : 0, 0);
        lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(w, hex(side ? C_WOOD_3 : C_WOOD_1), 0);
        lv_obj_set_style_bg_grad_color(w, hex(side ? C_WOOD_1 : C_WOOD_3), 0);
        lv_obj_set_style_bg_grad_dir(w, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_clickable(w, false);
    }
}

lv_obj_t *theme_label(lv_obj_t *parent, const char *text, uint32_t colour, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, hex(colour), 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *theme_title(lv_obj_t *parent, const char *text, int x, int y, int w)
{
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, w, 1);
    lv_obj_set_pos(rule, x, y + 8);
    lv_obj_set_style_bg_opa(rule, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(rule, hex(C_INK), 0);
    lv_obj_t *l = theme_label(parent, text, C_INK, x + 10, y);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, hex(C_PANEL), 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

lv_obj_t *theme_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, bool led, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, hex(C_BTN_FACE), 0);
    lv_obj_set_style_bg_grad_color(b, hex(C_BTN_LO), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(b, hex(C_ORANGE_HI), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(b, hex(C_ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, hex(0x000000), 0);
    lv_obj_set_style_text_color(b, hex(C_PANEL_DEEP), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    if (led) {
        lv_obj_t *d = lv_obj_create(b);             /* child 1: the LED, top left */
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 5);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, 5, 4);
        lv_obj_set_style_radius(d, 2, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, hex(C_LED_OFF), 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 3);
    } else {
        lv_obj_center(l);
    }
    return b;
}

void theme_button_lit(lv_obj_t *b, bool lit)
{
    lv_obj_t *d = lv_obj_get_child(b, 1);
    if (d) lv_obj_set_style_bg_color(d, hex(lit ? C_LED : C_LED_OFF), 0);
}
