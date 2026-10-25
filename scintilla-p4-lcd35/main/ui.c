/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen, landscape 480 x 320, in three bands: preset and octave at the top, the engine's load in
 * the middle, an 18-key keyboard (C to F, two and a half octaves) at the bottom. The keyboard is drawn
 * as plain objects and hit-tested here, black keys first; sliding across it retriggers. Every note
 * goes to the audio task through events_post, as BLE MIDI's do. */
#include <stdio.h>
#include "lvgl.h"
#include "audio_task.h"
#include "sc_presets.h"
#include "ui.h"
#include "events.h"
#include "ble_midi.h"

#define WHITE   18
#define KW      26                      /* white key width */
#define KX      6
#define KY      120
#define KH      196
#define BW      16                      /* black key: 60 % of a white key's width ... */
#define BH      118                     /* ... and 60 % of its height */
#define VEL     52000                   /* fixed velocity 100/127, Q16: touch has none */

static const int8_t white_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int8_t has_black[7] = { 1, 1, 0, 1, 1, 1, 0 };     /* a black key after C D F G A */
static lv_obj_t *wkey[WHITE], *bkey[WHITE], *kb, *name_label, *oct_label, *load_label, *sound_label;
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
    lv_obj_t *o = k >= 100 ? bkey[k - 100] : wkey[k];
    lv_color_t up = k >= 100 ? lv_color_hex(0x181818) : lv_color_hex(0xf4f4f0);
    lv_obj_set_style_bg_color(o, down ? lv_palette_main(LV_PALETTE_AMBER) : up, 0);
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
    if (code == LV_EVENT_PRESSED) printf("touch %d %d\n", (int)p.x, (int)p.y);
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
    lv_label_set_text_fmt(name_label, "%d  %s", SC_PRESET_ADDR[preset], SC_PRESETS[preset].name);
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
    lv_label_set_text_fmt(oct_label, "oct %+d", octave);
}

static void sound_toggle(lv_event_t *e)
{
    release();
    audio_set_sound(!audio_sound());
    lv_label_set_text(sound_label, audio_sound() ? LV_SYMBOL_VOLUME_MAX " Sound on" : LV_SYMBOL_MUTE " Sound off");
}

static void load_tick(lv_timer_t *t)
{
    if (audio_preset() != preset) { preset = audio_preset(); show_preset(); }   /* a program change */
    lv_label_set_text_fmt(load_label, "Scintilla   " LV_SYMBOL_BLUETOOTH " MIDI %s\nengine %d.%d %% of a core",
                          ble_midi_connected() ? "connected" : "waiting", audio_load_permille() / 10, audio_load_permille() % 10);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int w, lv_event_cb_t cb, int step)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_pos(b, x, 4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)step);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void any_press(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    printf("press %d %d\n", (int)p.x, (int)p.y);
}

void ui_build(int p)
{
    preset = p;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14161a), 0);
    lv_obj_add_event_cb(scr, any_press, LV_EVENT_PRESSED, NULL);

    button(scr, LV_SYMBOL_LEFT, 4, 44, preset_step, -1);
    name_label = lv_label_create(scr);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(name_label, 220);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(name_label, 56, 14);
    button(scr, LV_SYMBOL_RIGHT, 280, 44, preset_step, 1);
    button(scr, LV_SYMBOL_MINUS, 336, 40, octave_step, -1);
    oct_label = lv_label_create(scr);
    lv_obj_set_style_text_color(oct_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(oct_label, 382, 16);
    lv_label_set_text(oct_label, "oct +0");
    button(scr, LV_SYMBOL_PLUS, 436, 40, octave_step, 1);
    show_preset();

    load_label = lv_label_create(scr);
    lv_obj_set_style_text_color(load_label, lv_color_hex(0x9aa4ae), 0);
    lv_obj_set_pos(load_label, 8, 74);
    lv_label_set_text(load_label, "Scintilla");
    lv_timer_create(load_tick, 500, NULL);
    lv_obj_t *sb = lv_button_create(scr);
    lv_obj_set_size(sb, 150, 44);
    lv_obj_set_pos(sb, 322, 62);
    lv_obj_add_event_cb(sb, sound_toggle, LV_EVENT_CLICKED, NULL);
    sound_label = lv_label_create(sb);
    lv_label_set_text(sound_label, LV_SYMBOL_VOLUME_MAX " Sound on");
    lv_obj_center(sound_label);

    kb = lv_obj_create(scr);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, WHITE * KW, KH);
    lv_obj_set_pos(kb, KX, KY);
    lv_obj_set_clickable(kb, true);
    lv_obj_set_scrollable(kb, false);
    for (int i = 0; i < WHITE; i++) {
        lv_obj_t *o = wkey[i] = lv_obj_create(kb);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0x303030), 0);
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
}
