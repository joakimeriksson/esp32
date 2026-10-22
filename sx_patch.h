/* SPDX-License-Identifier: GPL-3.0-only */
/* A Synthex patch in integers. It mirrors synthex-web's patch.ts field for field; every continuous
 * value is the TypeScript value in Q16 (x 65536): seconds stay seconds, Hz stay Hz, cents stay
 * cents, levels stay 0..1. Enums are small integers in the TypeScript order. That one rule is what
 * lets tools/gen_presets.mjs convert the factory bank mechanically. */
#ifndef SX_PATCH_H
#define SX_PATCH_H
#include <stdint.h>

#define SX_Q16(x) ((int32_t)((x) * 65536))

enum { SX_WAVE_TRI, SX_WAVE_SAW, SX_WAVE_SQR, SX_WAVE_SIN, SX_WAVE_NOISE };
enum { SX_FLT_LP24, SX_FLT_LP12, SX_FLT_BP12, SX_FLT_BP6, SX_FLT_HP12 };
enum { SX_LFO_TRI, SX_LFO_SQR, SX_LFO_SH, SX_LFO_RND, SX_LFO_RAMP, SX_LFO_SQRU };
enum { SX_GLIDE_OFF, SX_GLIDE_GLIDE, SX_GLIDE_PORTA };
enum { SX_KEY_POLY, SX_KEY_MONO, SX_KEY_UNISON };
enum { SX_VM_SINGLE, SX_VM_SPLIT, SX_VM_DOUBLE };
enum { SX_DLY_STANDARD, SX_DLY_TAPE, SX_DLY_PINGPONG };
enum { SX_REV_PLATE, SX_REV_ROOM, SX_REV_HALL, SX_REV_GALACTIC };

/* modMatrix, in patch.ts's ModSlot order */
enum {
    SX_MM_L1_P1, SX_MM_L1_P2, SX_MM_L1_CUT, SX_MM_L1_PW1, SX_MM_L1_PW2, SX_MM_L1_AMP,
    SX_MM_L2_P1, SX_MM_L2_P2, SX_MM_L2_CUT, SX_MM_L2_PW1, SX_MM_L2_PW2, SX_MM_L2_AMP,
    SX_MM_E1_CUT, SX_MM_E1_P2,
    SX_MM_JX_L1, SX_MM_JX_L2,
    SX_MM_JY_CUT, SX_MM_JY_LFOFILT,
    SX_MM_VEL_AMP, SX_MM_VEL_CUT, SX_MM_VEL_E1,
    SX_MM_N
};

typedef struct { int32_t a, d, s, r; } sx_adsr_t;

typedef struct {
    int8_t shape, sync;
    int32_t rate, delay, depth_a, depth_b;
} sx_lfo_p_t;

typedef struct {                        /* LayerPatch */
    int8_t  o1_wave, o1_oct;
    int32_t o1_pwm, o1_glide_amt, o1_glide_speed;
    int8_t  o2_wave, o2_oct, o2_sync;
    int32_t o2_detune, o2_pwm, o2_glide_amt, o2_glide_speed;
    int32_t mix_o1, mix_o2, mix_noise;
    int8_t  noise_pink, ring;
    int32_t cross, cross2;              /* OSC1 -> OSC2 PWM, OSC2 -> OSC1 PWM */
    int8_t  flt_mode;
    int32_t cutoff, reso, env_amt, key_track;
    sx_adsr_t env_f, env_a;
    sx_lfo_p_t lfo[2];
    int32_t mm[SX_MM_N];
    int32_t glide_time, glide_amt;
    int8_t  glide_mode, glide_o1, glide_o2;
    int8_t  key_assign, multi_trigger;
    int32_t pan;
} sx_layer_t;

typedef struct { int8_t on, mode; int32_t mix, rate, depth; } sx_chorus_p_t;
typedef struct { int8_t on, mode; int32_t time, feedback, tone, spread, mix; } sx_delay_p_t;
typedef struct { int8_t on, algo; int32_t size, damping, mix; } sx_reverb_p_t;
typedef struct { sx_chorus_p_t chorus; sx_delay_p_t delay; sx_reverb_p_t reverb; } sx_fx_p_t;

typedef struct {                        /* Patch */
    char name[28];
    sx_layer_t layer[2];                /* upper, lower */
    int8_t  voice_mode, split;
    sx_fx_p_t fx;                       /* both layers use it (fxLower is not in the factory bank) */
    int32_t tune, fine_tune, volume, unison_detune;
    int8_t  polyphony, limiter;
} sx_patch_t;

#endif
