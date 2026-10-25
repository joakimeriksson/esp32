/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* synthex-web's panel colours (packages/app/src/App.svelte, Keyboard.svelte, ProgramPanel.svelte) */
#include "lvgl.h"
#define C_SURFACE     0x2a2828
#define C_PANEL       0x3a3838
#define C_PANEL_DEEP  0x303030
#define C_PANEL_LIGHT 0x444242
#define C_INK         0xe8e4dc
#define C_INK_SOFT    0x908880
#define C_ORANGE      0xd65a1c
#define C_ORANGE_HI   0xee6e2a
#define C_ORANGE_DARK 0xa04014
#define C_LED         0xff3018
#define C_LED_OFF     0x4a1a0c
#define C_BTN_FACE    0xdcd4c6
#define C_BTN_LO      0xb8b09a
#define C_WOOD_1      0xc08858
#define C_WOOD_2      0x8b5a30
#define C_WOOD_3      0x5a3a1c
#define C_KEY_BED     0x0e0c0a
#define C_WHITE_TOP   0xfefaeb
#define C_WHITE_BOT   0xe0d6b8
#define C_BLACK_TOP   0x2a241c
#define C_BLACK_BOT   0x0a0805
#define C_DISPLAY     0x1a1816

static inline lv_color_t hex(uint32_t c) { return lv_color_hex(c); }
void theme_screen(lv_obj_t *scr);                       /* panel background and the wood cheeks */
lv_obj_t *theme_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, bool led, lv_event_cb_t cb, void *user);
void theme_button_lit(lv_obj_t *button, bool lit);      /* the LED in a membrane button */
void theme_slider(lv_obj_t *slider);                    /* a dark track, orange fill, cream cap */
lv_obj_t *theme_label(lv_obj_t *parent, const char *text, uint32_t colour, int x, int y);
lv_obj_t *theme_title(lv_obj_t *parent, const char *text, int x, int y, int w);   /* a section rule with its name */
