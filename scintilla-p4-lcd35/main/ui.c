/* SPDX-License-Identifier: GPL-3.0-only */
/* The play screen, landscape 480 x 320, in synthex-web's colours: the program display at the top,
 * octave, sound, the BLE MIDI light and the way to the instrument page in the middle, the volume, an 18-key
 * keyboard (C to F, two and a half octaves) at the bottom. The keyboard is drawn as plain objects and
 * hit-tested here, black keys first; sliding across it retriggers. Every note goes to the audio task
 * through events_post, as BLE MIDI's do. Tapping the program display opens the patch picker: the
 * Synthex numbering, two banks (10-49, 110-149) of four groups of ten, chosen with lit buttons, and
 * the group's ten patches by name. */
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
#define KY      158
#define KH      158
#define BW      15                      /* black key: 60 % of a white key's width ... */
#define BH      95                      /* ... and 60 % of its height */
#define VEL     52000                   /* fixed velocity 100/127, Q16: touch has none */

static const int8_t white_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int8_t has_black[7] = { 1, 1, 0, 1, 1, 1, 0 };     /* a black key after C D F G A */
static lv_obj_t *play, *wkey[WHITE], *bkey[WHITE], *kb, *num_label, *name_label, *oct_label, *load_label;
static lv_obj_t *sound_btn, *midi_btn, *vol_slider, *vol_label;
static int preset, octave, held = -1, held_key = -1;
static lv_obj_t *picker, *bank_btn[2], *group_btn[4], *pick_btn[10];
static int pick_bank, pick_group;

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

static void volume_moved(lv_event_t *e)
{
    audio_set_volume(lv_slider_get_value(vol_slider));
    lv_label_set_text_fmt(vol_label, "%d", audio_volume());
}

/* ---- the patch picker ---- */

static int picker_index(int slot) { return pick_bank * 40 + pick_group * 10 + slot; }   /* the bank holds 10..49 */

static void picker_paint(void)
{
    for (int i = 0; i < 2; i++) theme_button_lit(bank_btn[i], i == pick_bank);
    for (int i = 0; i < 4; i++) theme_button_lit(group_btn[i], i == pick_group);
    for (int s = 0; s < 10; s++) {
        int k = picker_index(s);
        lv_label_set_text_fmt(lv_obj_get_child(pick_btn[s], 0), "%d  %s", SC_PRESET_ADDR[k], SC_PRESETS[k].name);
        theme_button_lit(pick_btn[s], k == preset);
    }
}

static void picker_open(lv_event_t *e)
{
    release();
    pick_bank = preset / 40;
    pick_group = preset % 40 / 10;
    picker_paint();
    lv_obj_set_hidden(picker, false);
}

static void picker_close(lv_event_t *e) { lv_obj_set_hidden(picker, true); }
static void picker_bank(lv_event_t *e) { pick_bank = (int)(intptr_t)lv_event_get_user_data(e); picker_paint(); }
static void picker_group(lv_event_t *e) { pick_group = (int)(intptr_t)lv_event_get_user_data(e); picker_paint(); }

static void picker_pick(lv_event_t *e)
{
    preset = picker_index((int)(intptr_t)lv_event_get_user_data(e));
    audio_request_preset(preset);
    show_preset();
    lv_obj_set_hidden(picker, true);
}

static void picker_build(void)
{
    picker = lv_obj_create(play);
    lv_obj_remove_style_all(picker);
    lv_obj_set_size(picker, 464, 320 - 46);
    lv_obj_set_pos(picker, 8, 46);
    lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(picker, hex(C_PANEL), 0);
    lv_obj_set_scrollable(picker, false);
    lv_obj_set_clickable(picker, true);                  /* nothing underneath gets the touches */
    static const char *const banks[2] = { "10 - 49", "110 - 149" };
    for (int i = 0; i < 2; i++)
        bank_btn[i] = theme_button(picker, banks[i], 6 + i * 92, 6, 88, 38, true, picker_bank, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++) {
        char t[4] = { (char)('1' + i), 0 };
        group_btn[i] = theme_button(picker, t, 196 + i * 54, 6, 50, 38, true, picker_group, (void *)(intptr_t)i);
    }
    theme_button(picker, LV_SYMBOL_CLOSE, 414, 6, 44, 38, false, picker_close, NULL);
    for (int s = 0; s < 10; s++) {
        lv_obj_t *b = pick_btn[s] = theme_button(picker, "", 6 + (s / 5) * 228, 52 + (s % 5) * 44, 224, 40, true, picker_pick, (void *)(intptr_t)s);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_width(l, 190);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 22, 0);
    }
    lv_obj_set_hidden(picker, true);
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
    lv_obj_set_clickable(disp, true);
    lv_obj_add_event_cb(disp, picker_open, LV_EVENT_CLICKED, NULL);
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

    /* the volume: the codec's output level, which a preset does not change */
    theme_title(play, "VOLUME", 14, 114, 452);
    vol_slider = lv_slider_create(play);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, audio_volume(), LV_ANIM_OFF);
    lv_obj_set_size(vol_slider, 380, 12);
    lv_obj_set_pos(vol_slider, 26, 136);
    theme_slider(vol_slider);
    lv_obj_add_event_cb(vol_slider, volume_moved, LV_EVENT_VALUE_CHANGED, NULL);
    vol_label = theme_label(play, "", C_LED, 420, 131);
    lv_label_set_text_fmt(vol_label, "%d", audio_volume());
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

    picker_build();
    instrument_build();
}
