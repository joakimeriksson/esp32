/* SPDX-License-Identifier: GPL-3.0-only */
/* The render loop: blocks of SC_BLOCK frames from the engine, mixed to mono for the ES8311 (one
 * DAC), written to the codec, which blocks on I2S DMA. Runs alone on core 1. Note events arrive
 * through the engine's own queue (sc_post, one writer); a preset change is taken between blocks. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "audio_task.h"
#include "sc_fixed.h"
#include "sc_presets.h"

sc_synth_t g_synth;
static esp_codec_dev_handle_t s_codec;
static atomic_int s_want_preset = -1;
static atomic_int s_load_permille;
static atomic_bool s_sound = true;

void audio_set_sound(bool on) { atomic_store(&s_sound, on); }
bool audio_sound(void) { return atomic_load(&s_sound); }

int audio_load_permille(void) { return atomic_load(&s_load_permille); }

void audio_request_preset(int preset) { atomic_store(&s_want_preset, preset); }

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

static void audio_task(void *arg)
{
    static int32_t mix[SC_BLOCK * 2];
    static int16_t pcm[SC_BLOCK];
    const int64_t budget_us = (int64_t)SC_BLOCK * 1000000 / SC_FS;
    int64_t sum = 0, worst = 0, last_report = esp_timer_get_time();
    int blocks = 0;
    bool was_on = true;
    for (;;) {
        bool on = atomic_load(&s_sound);
        if (was_on && !on) { sc_all_off(&g_synth); printf("audio: sound off\n"); }
        if (!was_on && on) printf("audio: sound on\n");
        was_on = on;
        int p = atomic_exchange(&s_want_preset, -1);
        if (p >= 0 && p < SC_N_PRESETS) {
            sc_all_off(&g_synth);
            g_synth.patch = SC_PRESETS[p];
            sc_reprep(&g_synth);
            printf("audio: preset %d %s\n", p, SC_PRESETS[p].name);
        }
        memset(mix, 0, sizeof mix);
        int64_t t0 = esp_timer_get_time();
        sc_render(&g_synth, mix, SC_BLOCK);
        int64_t dt = esp_timer_get_time() - t0;
        sum += dt; if (dt > worst) worst = dt; blocks++;
        for (int i = 0; i < SC_BLOCK; i++) pcm[i] = on ? sat16((mix[2 * i] + mix[2 * i + 1]) / 2) : 0;
        esp_codec_dev_write(s_codec, pcm, sizeof pcm);
        if (esp_timer_get_time() - last_report > 2000000) {
            atomic_store(&s_load_permille, (int)(1000 * sum / blocks / budget_us));
            printf("audio: %d blocks, %lld us/block mean, %lld worst, budget %lld us (%lld%% mean)\n",
                   blocks, sum / blocks, worst, budget_us, 100 * sum / blocks / budget_us);
            sum = worst = 0; blocks = 0; last_report = esp_timer_get_time();
        }
    }
}

void audio_start(esp_codec_dev_handle_t codec, int preset)
{
    s_codec = codec;
    sc_init(&g_synth, &SC_PRESETS[preset]);
    printf("audio: preset %d %s, %u bytes of engine state\n", preset, SC_PRESETS[preset].name, (unsigned)sizeof g_synth);
    xTaskCreatePinnedToCore(audio_task, "audio", 12288, NULL, 20, NULL, 1);
}
