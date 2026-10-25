/* SPDX-License-Identifier: GPL-3.0-only */
/* The play screen, landscape 480 x 320, in synthex-web's colours: the program display at the top,
 * octave, sound, the BLE MIDI light and the way to the instrument page in the middle, an 18-key
 * keyboard (C to F, two and a half octaves) at the bottom. The keyboard is drawn as plain objects and
 * hit-tested here, black keys first; sliding across it retriggers. Every note goes to the audio task
 * through events_post, as BLE MIDI's do. */
#include <stdio.h>
#include "lvgl.h"
#include "audio_task.h"
#include "sc_presets.h"
#include "ui.h"
#include "theme.h"
#include "events.h"
#include "ble_midi.h"

#define WHITE   18
#define KW      25                      /* white key width */
#define KX      15
#define KY      118
#define KH      198
#define BW      15                      /* black key: 60 % of a white key's width ... */
#define BH      118                     /* ... and 60 % of its height */
#define VEL     52000                   /* fixed velocity 100/127, Q16: touch has none */

static const int8_t white_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int8_t has_black[7] = { 1, 1, 0, 1, 1, 1, 0 };     /* a black key after C D F G A */
static lv_obj_t *play, *wkey[WHITE], *bkey[WHITE], *kb, *num_label, *name_label, *oct_label, *load_label;
static lv_obj_t *sound_btn, *midi_btn;
static int preset, octave, held = -1, held_key = -1;

static int base_note(void) { return 48 + 12 * octave; }        /* the leftmost C: C3 at octave 0 */

/* key index: 0..17 white, 100+i the black key after white i; -1 outside */
static int key_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= WHITE * KW || y >= KH) return -1;
    if (y < BH)
        for (int i = 0; i < WHITE - 1; i++)
            if (has_black[i % 7] && x >= (i + 1) * KW - BW / 2 && x < (i + 1) * KW + BW / 2) return 100 + i;
    return x / KW;
}

static int key_note(int k)
{
    int i = k % 100;
    return base_note() + 12 * (i / 7) + white_semi[i % 7] + (k >= 100);
}

static void key_paint(int k, bool down)
{
    if (k < 0) return;
    bool black = k >= 100;
    lv_obj_t *o = black ? bkey[k - 100] : wkey[k];
    uint32_t top = down ? (black ? C_ORANGE : C_ORANGE_HI) : (black ? C_BLACK_TOP : C_WHITE_TOP);
    uint32_t bot = down ? (black ? C_ORANGE_DARK : C_ORANGE) : (black ? C_BLACK_BOT : C_WHITE_BOT);
    lv_obj_set_style_bg_color(o, hex(top), 0);
    lv_obj_set_style_bg_grad_color(o, hex(bot), 0);
}

static void release(void)
{
    if (held >= 0) events_post(EV_NOTE_OFF, (uint8_t)held, 0);
    key_paint(held_key, false);
    held = held_key = -1;
}

static void keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { release(); return; }
    lv_point_t p;
    lv_area_t a;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_get_coords(kb, &a);
    int k = key_at(p.x - a.x1, p.y - a.y1);
    if (k == held_key) return;
    release();
    if (k < 0) return;
    held_key = k;
    held = key_note(k);
    key_paint(k, true);
    events_post(EV_NOTE_ON, (uint8_t)held, VEL);
}

static void show_preset(void)
{
    lv_label_set_text_fmt(num_label, "%02d", SC_PRESET_ADDR[preset]);
    lv_label_set_text(name_label, SC_PRESETS[preset].name);
}

static void preset_step(lv_event_t *e)
{
    release();
    preset = (preset + (int)(intptr_t)lv_event_get_user_data(e) + SC_N_PRESETS) % SC_N_PRESETS;
    audio_request_preset(preset);
    show_preset();
}

static void octave_step(lv_event_t *e)
{
    int o = octave + (int)(intptr_t)lv_event_get_user_data(e);
    if (o < -2 || o > 2) return;
    release();
    octave = o;
    lv_label_set_text_fmt(oct_label, "%+d", octave);
}

static void sound_toggle(lv_event_t *e)
{
    release();
    audio_set_sound(!audio_sound());
    theme_button_lit(sound_btn, audio_sound());
}

static void to_instrument(lv_event_t *e)
{
    release();
    instrument_show();
}

static void tick(lv_timer_t *t)
{
    if (audio_preset() != preset) { preset = audio_preset(); show_preset(); }   /* a program change */
    theme_button_lit(midi_btn, ble_midi_connected());
    lv_label_set_text_fmt(load_label, "CPU %d.%d %%", audio_load_permille() / 10, audio_load_permille() % 10);
}

static lv_obj_t *display_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, w, h);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, hex(C_DISPLAY), 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_border_color(d, hex(0x000000), 0);
    lv_obj_set_style_radius(d, 2, 0);
    lv_obj_set_clickable(d, false);
    lv_obj_set_scrollable(d, false);
    return d;
}

void ui_show_play(void) { lv_screen_load(play); }

void ui_build(int p)
{
    preset = p;
    play = lv_screen_active();
    theme_screen(play);

    /* the top band: name plate and program display */
    lv_obj_t *band = lv_obj_create(play);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, 464, 46);
    lv_obj_set_pos(band, 8, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(band, hex(C_PANEL_DEEP), 0);
    lv_obj_set_clickable(band, false);
    lv_obj_set_scrollable(band, false);
    lv_obj_t *logo = theme_label(play, "SCINTILLA", C_ORANGE, 16, 12);
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(logo, 1, 0);
    theme_button(play, LV_SYMBOL_LEFT, 146, 6, 40, 34, false, preset_step, (void *)(intptr_t)-1);
    lv_obj_t *disp = display_box(play, 190, 6, 230, 34);
    num_label = theme_label(disp, "", C_LED, 8, 6);
    lv_obj_set_style_text_font(num_label, &lv_font_montserrat_20, 0);
    name_label = theme_label(disp, "", C_INK, 44, 9);
    lv_obj_set_width(name_label, 180);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    theme_button(play, LV_SYMBOL_RIGHT, 424, 6, 40, 34, false, preset_step, (void *)(intptr_t)1);
    show_preset();

    /* the middle band */
    theme_title(play, "OCTAVE", 14, 50, 146);
    theme_button(play, LV_SYMBOL_MINUS, 16, 68, 40, 40, false, octave_step, (void *)(intptr_t)-1);
    lv_obj_t *ob = display_box(play, 60, 68, 50, 40);
    oct_label = theme_label(ob, "+0", C_LED, 0, 0);
    lv_obj_set_style_text_font(oct_label, &lv_font_montserrat_20, 0);
    lv_obj_center(oct_label);
    theme_button(play, LV_SYMBOL_PLUS, 114, 68, 40, 40, false, octave_step, (void *)(intptr_t)1);
    theme_title(play, "MASTER", 166, 50, 172);
    sound_btn = theme_button(play, "SOUND", 168, 68, 80, 40, true, sound_toggle, NULL);
    theme_button_lit(sound_btn, true);
    midi_btn = theme_button(play, "BT MIDI", 252, 68, 84, 40, true, NULL, NULL);
    lv_obj_set_clickable(midi_btn, false);
    lv_obj_t *ib = theme_button(play, "INSTRUMENT " LV_SYMBOL_RIGHT, 344, 68, 122, 40, false, to_instrument, NULL);
    lv_obj_set_style_bg_color(ib, hex(C_ORANGE_HI), 0);
    lv_obj_set_style_bg_grad_color(ib, hex(C_ORANGE), 0);
    lv_obj_set_style_text_color(ib, hex(0xfff6ea), 0);
    load_label = theme_label(play, "", C_INK_SOFT, 384, 50);
    lv_timer_create(tick, 500, NULL);

    /* the keyboard on its bed */
    lv_obj_t *bed = lv_obj_create(play);
    lv_obj_remove_style_all(bed);
    lv_obj_set_size(bed, 464, 320 - KY + 4);
    lv_obj_set_pos(bed, 8, KY - 4);
    lv_obj_set_style_bg_opa(bed, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bed, hex(C_KEY_BED), 0);
    lv_obj_set_clickable(bed, false);
    lv_obj_set_scrollable(bed, false);
    lv_obj_t *felt = lv_obj_create(play);                  /* the red felt strip behind the keys */
    lv_obj_remove_style_all(felt);
    lv_obj_set_size(felt, WHITE * KW, 2);
    lv_obj_set_pos(felt, KX, KY - 2);
    lv_obj_set_style_bg_opa(felt, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(felt, hex(0x8a1a10), 0);

    kb = lv_obj_create(play);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, WHITE * KW, KH);
    lv_obj_set_pos(kb, KX, KY);
    lv_obj_set_clickable(kb, true);
    lv_obj_set_scrollable(kb, false);
    for (int i = 0; i < WHITE; i++) {
        lv_obj_t *o = wkey[i] = lv_obj_create(kb);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, hex(0x5a5040), 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_size(o, KW, KH);
        lv_obj_set_pos(o, i * KW, 0);
        lv_obj_set_clickable(o, false);
        key_paint(i, false);
    }
    for (int i = 0; i < WHITE - 1; i++) {
        if (!has_black[i % 7]) continue;
        lv_obj_t *o = bkey[i] = lv_obj_create(kb);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, hex(0x000000), 0);
        lv_obj_set_style_radius(o, 2, 0);
        lv_obj_set_size(o, BW, BH);
        lv_obj_set_pos(o, (i + 1) * KW - BW / 2, 0);
        lv_obj_set_clickable(o, false);
        key_paint(100 + i, false);
    }
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_PRESS_LOST, NULL);

    instrument_build();
}
