/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_codec_dev.h"
#include "sc_synth.h"

#define SC_BLOCK 128            /* frames per sc_render: the host renderer's run length (CLAUDE.md upstream) */

extern sc_synth_t g_synth;      /* the one instrument; sc_post() from one other task only */
/* start the render loop on core 1, writing into the codec; preset is an index into SC_PRESETS */
void audio_start(esp_codec_dev_handle_t codec, int preset);
/* ask the audio task to switch preset between two blocks */
void audio_request_preset(int preset);
/* the render's mean share of the block budget over the last report, in 1/1000 */
int audio_load_permille(void);
/* sound off: every note stops and the output is silence, until sound is on again */
void audio_set_sound(bool on);
bool audio_sound(void);
