/* SPDX-License-Identifier: GPL-3.0-only */
/* The instrument page: the patch that is playing, edited live, in six tabs that follow synthex-web's
 * panel (OSC, MIX, FILTER, ENV, LFO, FX), each up to eight controls in two columns. Sliders for
 * continuous values, with synthex-web's knob ranges; a button that steps through the choices for
 * the rest. Layer fields go to the upper or the lower layer (the UPPER/LOWER button); the effects
 * and the master volume belong to the whole patch. Every change goes to the audio task as a field
 * edit (events_param); the engine applies it to the notes that sound. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include "lvgl.h"
#include "audio_task.h"
#include "sc_presets.h"
#include "ui.h"
#include "theme.h"
#include "events.h"

enum { LIN, EXP, ENUM, BOOL, INT };
enum { P_OSC, P_MIX, P_FILTER, P_ENV, P_LFO, P_FX, P_N };
static const char *const PAGE_NAMES[P_N] = { "OSC", "MIX", "FILTER", "ENV", "LFO", "FX" };
static const char *const WAVES[] = { "Triangle", "Sawtooth", "Square", "Sine", "Noise" };
static const char *const FILTERS[] = { "LP 24", "LP 12", "BP 12", "BP 6", "HP 12" };
static const char *const LFOS[] = { "Triangle", "Square", "S & H", "Random", "Ramp", "Square +" };
static const char *const REVERBS[] = { "Plate", "Room", "Hall", "Galactic" };

typedef struct {
    uint8_t page;
    const char *label;
    uint8_t kind;
    bool layer;                 /* an offset in sc_layer_t (else in sc_patch_t) */
    uint16_t off;
    float lo, hi;
    const char *unit;
    const char *const *opts;    /* ENUM */
} param_t;

#define L(f) true, offsetof(sc_layer_t, f)
#define G(f) false, offsetof(sc_patch_t, f)
static const param_t PARAMS[] = {
    { P_OSC, "OSC 1 WAVE", ENUM, L(o1_wave), 0, 4, "", WAVES },
    { P_OSC, "OSC 2 WAVE", ENUM, L(o2_wave), 0, 4, "", WAVES },
    { P_OSC, "OSC 1 OCTAVE", INT, L(o1_oct), -2, 2, "" },
    { P_OSC, "OSC 2 OCTAVE", INT, L(o2_oct), -2, 2, "" },
    { P_OSC, "OSC 1 PULSE WIDTH", LIN, L(o1_pwm), 0.05f, 0.95f, "" },
    { P_OSC, "OSC 2 DETUNE", LIN, L(o2_detune), -50, 50, " c" },
    { P_OSC, "OSC 2 PULSE WIDTH", LIN, L(o2_pwm), 0.05f, 0.95f, "" },
    { P_OSC, "OSC 2 SYNC", BOOL, L(o2_sync), 0, 1, "" },
    { P_MIX, "OSC 1 VOLUME", LIN, L(mix_o1), 0, 1, "" },
    { P_MIX, "OSC 2 VOLUME", LIN, L(mix_o2), 0, 1, "" },
    { P_MIX, "NOISE VOLUME", LIN, L(mix_noise), 0, 1, "" },
    { P_MIX, "PINK NOISE", BOOL, L(noise_pink), 0, 1, "" },
    { P_MIX, "RING MOD", BOOL, L(ring), 0, 1, "" },
    { P_MIX, "CROSS MOD", LIN, L(cross), 0, 1, "" },
    { P_MIX, "GLIDE SPEED", EXP, L(glide_time), 0.01f, 10, " s" },
    { P_MIX, "GLIDE AMOUNT", LIN, L(glide_amt), -24, 24, " st" },
    { P_FILTER, "MODE", ENUM, L(flt_mode), 0, 4, "", FILTERS },
    { P_FILTER, "KEYBOARD", LIN, L(key_track), 0, 1, "" },
    { P_FILTER, "FREQUENCY", LIN, L(cutoff), 0, 1, "" },
    { P_FILTER, "RESONANCE", LIN, L(reso), 0, 1, "" },
    { P_FILTER, "ENVELOPE", LIN, L(env_amt), -1, 1, "" },
    { P_ENV, "FILTER ATTACK", EXP, L(env_f.a), 0.001f, 5, " s" },
    { P_ENV, "AMP ATTACK", EXP, L(env_a.a), 0.001f, 5, " s" },
    { P_ENV, "FILTER DECAY", EXP, L(env_f.d), 0.001f, 5, " s" },
    { P_ENV, "AMP DECAY", EXP, L(env_a.d), 0.001f, 5, " s" },
    { P_ENV, "FILTER SUSTAIN", LIN, L(env_f.s), 0, 1, "" },
    { P_ENV, "AMP SUSTAIN", LIN, L(env_a.s), 0, 1, "" },
    { P_ENV, "FILTER RELEASE", EXP, L(env_f.r), 0.001f, 5, " s" },
    { P_ENV, "AMP RELEASE", EXP, L(env_a.r), 0.001f, 5, " s" },
    { P_LFO, "LFO 1 SHAPE", ENUM, L(lfo[0].shape), 0, 5, "", LFOS },
    { P_LFO, "LFO 2 SHAPE", ENUM, L(lfo[1].shape), 0, 5, "", LFOS },
    { P_LFO, "LFO 1 FREQUENCY", EXP, L(lfo[0].rate), 0.05f, 20, " Hz" },
    { P_LFO, "LFO 2 FREQUENCY", EXP, L(lfo[1].rate), 0.05f, 20, " Hz" },
    { P_LFO, "LFO 1 DELAY", LIN, L(lfo[0].delay), 0, 5, " s" },
    { P_LFO, "LFO 2 DELAY", LIN, L(lfo[1].delay), 0, 5, " s" },
    { P_LFO, "LFO 1 DEPTH A", LIN, L(lfo[0].depth_a), 0, 1, "" },
    { P_LFO, "LFO 2 DEPTH A", LIN, L(lfo[1].depth_a), 0, 1, "" },
    { P_FX, "CHORUS", BOOL, G(fx.chorus.on), 0, 1, "" },
    { P_FX, "REVERB", BOOL, G(fx.reverb.on), 0, 1, "" },
    { P_FX, "DELAY", BOOL, G(fx.delay.on), 0, 1, "" },
    { P_FX, "REVERB TYPE", ENUM, G(fx.reverb.algo), 0, 3, "", REVERBS },
    { P_FX, "DELAY TIME", LIN, G(fx.delay.time), 0.01f, 1.5f, " s" },
    { P_FX, "REVERB MIX", LIN, G(fx.reverb.mix), 0, 1, "" },
    { P_FX, "DELAY MIX", LIN, G(fx.delay.mix), 0, 1, "" },
    { P_FX, "MASTER VOLUME", LIN, G(volume), 0, 1, "" },
};
#define N_PARAMS (int)(sizeof PARAMS / sizeof PARAMS[0])
#define SLIDER_MAX 1000

static lv_obj_t *scr, *tabs[P_N], *pages[P_N], *name_label, *layer_btn, *layer_label;
static lv_obj_t *value_label[N_PARAMS], *control[N_PARAMS];
static int page, layer;

static uint16_t field(const param_t *p)
{
    return p->layer ? (uint16_t)(offsetof(sc_patch_t, layer) + layer * sizeof(sc_layer_t) + p->off) : p->off;
}

static bool wide(const param_t *p) { return p->kind == LIN || p->kind == EXP; }

/* the field as it stands in the engine's patch: reads from the other core, whole words */
static int32_t get(const param_t *p)
{
    const uint8_t *b = (const uint8_t *)&g_synth.patch + field(p);
    return wide(p) ? *(const int32_t *)b : (int8_t)*b;
}

static float to_value(const param_t *p, int t)
{
    float u = (float)t / SLIDER_MAX;
    return p->kind == EXP ? p->lo * powf(p->hi / p->lo, u) : p->lo + (p->hi - p->lo) * u;
}

static int to_slider(const param_t *p, float v)
{
    float u = p->kind == EXP ? logf(fmaxf(v, p->lo) / p->lo) / logf(p->hi / p->lo) : (v - p->lo) / (p->hi - p->lo);
    return (int)lroundf(fminf(fmaxf(u, 0), 1) * SLIDER_MAX);
}

static void show_value(int i)
{
    const param_t *p = &PARAMS[i];
    int32_t raw = get(p);
    char buf[32];
    switch (p->kind) {
    case ENUM: snprintf(buf, sizeof buf, "%s", p->opts[raw < p->lo || raw > p->hi ? 0 : raw]); break;
    case BOOL: snprintf(buf, sizeof buf, "%s", raw ? "ON" : "OFF"); break;
    case INT:  snprintf(buf, sizeof buf, "%+d", (int)raw); break;
    default: {
        float v = raw / 65536.0f;
        if (fabsf(v) >= 10) snprintf(buf, sizeof buf, "%.0f%s", v, p->unit);
        else if (fabsf(v) >= 1 || !*p->unit) snprintf(buf, sizeof buf, "%.2f%s", v, p->unit);
        else snprintf(buf, sizeof buf, "%.3f%s", v, p->unit);
    } }
    lv_label_set_text(value_label[i], buf);
    if (wide(p)) lv_slider_set_value(control[i], to_slider(p, raw / 65536.0f), LV_ANIM_OFF);
    else if (p->kind == BOOL) theme_button_lit(control[i], raw != 0);
}

static void slider_moved(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const param_t *p = &PARAMS[i];
    float v = to_value(p, lv_slider_get_value(control[i]));
    events_param(true, field(p), (int32_t)lroundf(v * 65536));
    char buf[32];
    if (fabsf(v) >= 10) snprintf(buf, sizeof buf, "%.0f%s", v, p->unit);
    else if (fabsf(v) >= 1 || !*p->unit) snprintf(buf, sizeof buf, "%.2f%s", v, p->unit);
    else snprintf(buf, sizeof buf, "%.3f%s", v, p->unit);
    lv_label_set_text(value_label[i], buf);           /* the engine catches up within a block */
}

static void step_pressed(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const param_t *p = &PARAMS[i];
    int32_t v = get(p) + 1;
    if (v > p->hi) v = (int32_t)p->lo;
    events_param(false, field(p), v);
    char buf[32];
    if (p->kind == ENUM) snprintf(buf, sizeof buf, "%s", p->opts[v]);
    else if (p->kind == BOOL) { snprintf(buf, sizeof buf, "%s", v ? "ON" : "OFF"); theme_button_lit(control[i], v); }
    else snprintf(buf, sizeof buf, "%+d", (int)v);
    lv_label_set_text(value_label[i], buf);
}

static void refresh(void)
{
    lv_label_set_text_fmt(name_label, "%02d %s", SC_PRESET_ADDR[audio_preset()], SC_PRESETS[audio_preset()].name);
    lv_label_set_text(layer_label, layer ? "LOWER" : "UPPER");
    for (int i = 0; i < N_PARAMS; i++) show_value(i);
}

static void select_page(int n)
{
    page = n;
    for (int i = 0; i < P_N; i++) {
        theme_button_lit(tabs[i], i == n);
        lv_obj_set_hidden(pages[i], i != n);
    }
}

static void tab_pressed(lv_event_t *e) { select_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void back_pressed(lv_event_t *e) { ui_show_play(); }
static void layer_pressed(lv_event_t *e) { layer = !layer; refresh(); }

static lv_timer_t *watch;
static int shown_preset = -1;
static void watch_preset(lv_timer_t *t)                 /* a program change over MIDI while here */
{
    if (lv_screen_active() == scr && audio_preset() != shown_preset) { shown_preset = audio_preset(); refresh(); }
}

void instrument_show(void)
{
    shown_preset = audio_preset();
    refresh();
    lv_screen_load(scr);
}

void instrument_build(void)
{
    scr = lv_obj_create(NULL);
    theme_screen(scr);
    lv_obj_t *band = lv_obj_create(scr);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, 464, 42);
    lv_obj_set_pos(band, 8, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(band, hex(C_PANEL_DEEP), 0);
    lv_obj_set_clickable(band, false);
    lv_obj_set_scrollable(band, false);
    theme_button(scr, LV_SYMBOL_LEFT " PLAY", 14, 5, 76, 32, false, back_pressed, NULL);
    lv_obj_t *t = theme_label(scr, "INSTRUMENT", C_ORANGE, 100, 12);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    name_label = theme_label(scr, "", C_INK, 214, 12);
    lv_obj_set_width(name_label, 170);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    layer_btn = theme_button(scr, "", 392, 5, 74, 32, false, layer_pressed, NULL);
    layer_label = lv_obj_get_child(layer_btn, 0);

    for (int i = 0; i < P_N; i++) {
        tabs[i] = theme_button(scr, PAGE_NAMES[i], 14 + i * 76, 48, 72, 36, true, tab_pressed, (void *)(intptr_t)i);
        pages[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(pages[i]);
        lv_obj_set_size(pages[i], 464, 228);
        lv_obj_set_pos(pages[i], 8, 92);
        lv_obj_set_scrollable(pages[i], false);
    }
    int slot[P_N] = { 0 };
    for (int i = 0; i < N_PARAMS; i++) {
        const param_t *p = &PARAMS[i];
        int s = slot[p->page]++, x = 8 + (s & 1) * 230, y = (s >> 1) * 56;
        lv_obj_t *pg = pages[p->page];
        theme_label(pg, p->label, C_INK_SOFT, x, y + 4);
        value_label[i] = theme_label(pg, "", C_LED, x, y + 4);
        lv_obj_set_width(value_label[i], 216);
        lv_obj_set_style_text_align(value_label[i], LV_TEXT_ALIGN_RIGHT, 0);
        if (wide(p)) {
            lv_obj_t *sl = control[i] = lv_slider_create(pg);
            lv_slider_set_range(sl, 0, SLIDER_MAX);
            lv_obj_set_size(sl, 200, 12);
            lv_obj_set_pos(sl, x + 8, y + 32);
            theme_slider(sl);
            lv_obj_add_event_cb(sl, slider_moved, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        } else {
            control[i] = theme_button(pg, p->kind == BOOL ? "ON / OFF" : LV_SYMBOL_LOOP " NEXT", x, y + 24, 216, 28,
                                      p->kind == BOOL, step_pressed, (void *)(intptr_t)i);
        }
    }
    select_page(P_FILTER);
    watch = lv_timer_create(watch_preset, 300, NULL);
}
