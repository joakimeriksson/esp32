/* SPDX-License-Identifier: GPL-3.0-only */
/* The whole instrument: two layers (upper, lower) of voices with synthex-web's voice allocation
 * (synth.ts, voice.ts), each layer through its own chorus, then a reverb, the master volume and a limiter.
 *
 * Threading: sx_render() runs in the audio interrupt; everything else may run in the main loop.
 * The two meet only through the event queue (sx_post: one writer, one reader) and through patch
 * fields written whole (32-bit stores) followed by SX_EV_PATCH. */
#ifndef SX_SYNTH_H
#define SX_SYNTH_H
#include <stdint.h>
#include "sx_patch.h"
#include "sx_voice.h"

#ifndef SX_VOICES
#define SX_VOICES 8                             /* per layer (patch.master.polyphony, capped) */
#endif
#define SX_CHORUS_LEN 1024                      /* samples: 23 ms, the chorus reaches ~20 */

enum { SX_EV_NOTE_ON = 1, SX_EV_NOTE_OFF, SX_EV_ALL_OFF, SX_EV_BEND, SX_EV_PATCH };
typedef struct { uint8_t type, note; int32_t val; } sx_event_t;
#define SX_QUEUE 64

typedef struct {
    int32_t buf[SX_CHORUS_LEN];                 /* Q20: a loud chord's voices sum past 1.0 (Brass I: ~1.8) */
    uint32_t w, ph;
} sx_chorus_t;

/* The reverb: eight delay lines in a Hadamard feedback loop, standing in for the app's convolution
 * with a noise impulse (fx/reverb.ts); sx_synth.c says what is matched. 64 KB. */
#define SX_REV_LINES 8
#define SX_REV_LEN 2048                         /* each line's ring (power of two); the longest is 1949 */
typedef struct {
    int32_t buf[SX_REV_LEN][SX_REV_LINES];      /* Q20, interleaved: slot j of line k is buf[j][k] */
    uint32_t w;
    int32_t lpl, lpr;                           /* the output lowpass, Q20 */
    int32_t g[SX_REV_LINES];                    /* each line's gain per pass, with the matrix's 1/sqrt(8): Q30 */
    int32_t a, wet, dry;                        /* Q16 */
    int8_t on;
} sx_reverb_t;

#define SX_LIM_LOOK 256                         /* the limiter's look-ahead: 5.8 ms (Chrome's is 6) */
typedef struct {
    int32_t env;                                /* the detector, linear Q15 */
    int32_t gain;                               /* the gain applied at the last sub-block's end, Q16 */
    int32_t dl[SX_LIM_LOOK], dr[SX_LIM_LOOK];   /* the audio, delayed while the detector looks ahead */
    uint32_t w;
} sx_limiter_t;

typedef struct {
    sx_voice_t v[SX_VOICES];
    int16_t note[SX_VOICES];                    /* VoicePool: -1 until first used */
    uint32_t started[SX_VOICES], released[SX_VOICES];   /* 0: never released, ~0: held */
    uint8_t held[32];
    uint32_t nheld;
    sx_prep_t prep;
    sx_chorus_t chorus;
} sx_layer_state_t;

typedef struct {
    sx_patch_t patch;                           /* the sound being played; edit, then post SX_EV_PATCH */
    sx_perf_t perf;                             /* the joystick section; edit, then post SX_EV_PATCH */
    sx_layer_state_t layer[2];
    uint32_t clock;                             /* orders voice starts and releases */
    sx_event_t q[SX_QUEUE];
    volatile uint32_t q_w, q_r;
    int32_t bus[2][256];                        /* scratch: each layer's voices, mono Q20 */
    sx_limiter_t lim;
    sx_reverb_t rev;                            /* after both layers' choruses: one for the two */
} sx_synth_t;

void sx_init(sx_synth_t *s, const sx_patch_t *patch);
void sx_perf_default(sx_perf_t *p);
/* from any context with one writer: queue an event for the next sx_render */
int  sx_post(sx_synth_t *s, uint8_t type, uint8_t note, int32_t val);
/* the audio interrupt: add n (<= 256) stereo frames (L R, Q15) into out */
void sx_render(sx_synth_t *s, int32_t *out, uint32_t n);
/* the same events, applied at once (only from the context that calls sx_render) */
void sx_note_on(sx_synth_t *s, int note, int32_t vel_q16);
void sx_note_off(sx_synth_t *s, int note);
void sx_all_off(sx_synth_t *s);
void sx_pitch_bend(sx_synth_t *s, int32_t semis_q16);
void sx_reprep(sx_synth_t *s);
/* the reverb alone (sx_render runs it; these are for tests): set it from the patch's, then L R (Q20)
 * in place, n <= 256 */
void sx_reverb_prep(sx_reverb_t *r, const sx_reverb_p_t *p);
void sx_reverb_run(sx_reverb_t *r, int32_t *L, int32_t *R, uint32_t n);

#endif
