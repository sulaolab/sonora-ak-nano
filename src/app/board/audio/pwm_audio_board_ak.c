/*
 * hal_pwm_audio board backend: Sonora AK512 and Nano AK506 (dsPIC33AK).
 *
 *   APP_PWM_OUTPUT == 1  MPQ6612A: PG5/DMA4 mono on B or XPRO1,
 *                        optional PG6/DMA5 B+XPRO1 stereo,
 *                        48/96/192 kHz carrier, nSLEEP/nFAULT from mpq6612a_board.h.
 *   APP_PWM_OUTPUT == 0  two MIC4607 half-bridge drivers, stereo: PG5 (L) and
 *                        PG6 (R, triggered by PG5), DMA4/DMA5, 96 kHz carrier,
 *                        each duty held for two carrier periods.
 *
 * Duty is in PG Q4 counts. PG alignment options, override with -Define:
 *   PWM_AUDIO_AK_CENTRE  1: centre-aligned PG (MODSEL=100), same carrier; the
 *                        DMA word is PGxDC = half the pulse width.
 *   PWM_AUDIO_AK_HREN    1: fine edge placement, PGxPER/DC bits[3:0] live.
 *   PWM_AUDIO_AK_TRIGA   1: DMA trigger at TRIGA (mid timer cycle; first half
 *                        in centre mode) instead of EOC. Default follows CENTRE.
 * Defaults: MPQ6612A = edge + HREN (owner listening 2026-10-18, Nano slot 3,
 * 192 kHz: best quality and level of edge+HREN, centre+HREN and 3-level);
 * MIC4607 = edge, standard resolution (all 0 = the PG registers are left
 * exactly as the HAL wrote them).
 * Duty shaping between the core block and the DMA buffer:
 *   PWM_AUDIO_AK_INTERP  1: 48 -> 192 kHz polyphase FIR (pwm_audio_interp_coeffs.h)
 *                        instead of repeating each duty 4 times (ZOH). x4 only.
 *   PWM_AUDIO_AK_NS2     1: TPDF dither + 2nd-order error feedback when the
 *                        192 kHz duty is rounded to the PG step (1 Q4 with
 *                        HREN, 16 without). Needs INTERP.
 *   Defaults: INTERP 1 on MPQ6612A at a 192 kHz carrier, else 0 (old ZOH).
 *   NS2 defaults 0: the core modulator already rounds each 48 kHz duty to a
 *   whole Q4 count, and that white rounding noise (~-86 dBFS) is the floor;
 *   shaping the 192 kHz rounding after it measured only ~1 dB
 *   (buildtools/pwm_sim/pwm_shape_c_sim.py). It needs a fractional duty from
 *   the HAL to pay off.
 *   PWM_AUDIO_AK_3LEVEL  1: MPQ6612A mono, 192 kHz, HREN, edge timer only. Off by
 *                        default: not adopted (owner listening 2026-10-18,
 *                        -5.6 dB headroom, clips at +12 dB PWM gain).
 *                        3-level brake-zero output (theme 3): PG5 in Dual PWM
 *                        (MODSEL=010), Independent outputs, IN1/IN2 idle HIGH
 *                        = brake (0 V). PWM5L (IN2, POLL=1) goes low for tp
 *                        centred at T/2 (+V); PWM5H (IN1, POLH=0) is high from
 *                        PHASE to DC, so it is low for tn centred on each
 *                        period boundary (-V, split tn[k]/2 | tn[k]/2 over the
 *                        boundary). Each window uses the signal at its own centre
 *                        (48 -> 384 kHz polyphase, pwm_audio_interp8_coeffs.h)
 *                        and a soft split, c = sqrt(a^2 + (d/2)^2) - a,
 *                        tp = tmin + c + d/2, tn = tmin + c - d/2
 *                        (buildtools/pwm_sim/RESULTS_3LEVEL_2026-10-18.md).
 *                        DMA4-7 write PHASE, DC, TRIGB, TRIGA every period on
 *                        the PG5 TRIGC event at T/2 (ADC trigger 1, the -V
 *                        window is around SOC so mid-period is idle); fixed DMA priority
 *                        keeps that order and the TRIGA write commits the
 *                        update (UPDTRG=11). Same net gain as 2-level; a net
 *                        whose windows would break the guards clips (clips=).
 *                        Overrides go through the polarity stage, so the
 *                        board writes OVRDAT itself (mixed POLH/POLL).
 *                        Default 0.
 * DMA4 HALF/DONE refills the half it just finished
 * from one core block, for both channels at once; DMA5 runs in lockstep from
 * the same start and has no interrupt of its own.
 */
#include "app_specific_config_defs.h"
#include "nora_pwm_audio_backend.h"

#if NORA_PWM_AUDIO_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <xc.h>

#include "board/audio/mpq6612a_board.h"
#include "board/audio/pwm_audio_board.h"
#include "board/clock/sonora_clock.h"
#include "nora_dma.h"
#include "nora_gpio.h"
#include "nora_pwm.h"
#include "pwm_audio_dma_buffer.h"   /* PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 */
#include "nora_high_res_timer.h"
#include "timer_app.h"
#include <stdio.h>

#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
#error "hal_pwm_audio drives one L/R output; the secondary pair is legacy-only"
#endif

#ifndef PWM_AUDIO_AK_3LEVEL
#define PWM_AUDIO_AK_3LEVEL 0
#endif
#ifndef PWM_AUDIO_AK_CENTRE
#define PWM_AUDIO_AK_CENTRE 0
#endif
#ifndef PWM_AUDIO_AK_HREN
#define PWM_AUDIO_AK_HREN (APP_PWM_OUTPUT == 1)
#endif
#ifndef PWM_AUDIO_AK_TRIGA
#define PWM_AUDIO_AK_TRIGA PWM_AUDIO_AK_CENTRE
#endif
#define BOARD_PG_TUNED (PWM_AUDIO_AK_CENTRE || PWM_AUDIO_AK_HREN || PWM_AUDIO_AK_TRIGA)
#ifndef PWM_AUDIO_AK_INTERP
#define PWM_AUDIO_AK_INTERP (APP_PWM_OUTPUT == 1 && PWM_AUDIO_UPSAMPLE_FACTOR == 4)
#endif
#ifndef PWM_AUDIO_AK_NS2
#define PWM_AUDIO_AK_NS2 0
#endif

#define BOARD_FACTOR      PWM_AUDIO_UPSAMPLE_FACTOR
#define BOARD_PERIOD_Q4   PWM_AUDIO_PERIOD_COUNT_Q4
/* Centre-aligned: one carrier period is two timer cycles and the pulse is
 * 2 * PGxDC wide, so the modulator works on half the period and its output
 * is PGxDC directly. One timer cycle lasts PER + 16 Q4 counts in both
 * resolutions (DS70005320E: HR PER = 16 * Fclk/Fpwm - 16 on this Q4 part;
 * standard resolution ignores bits[3:0], which is why the HAL's
 * period - 1 works there and is 1/16 clock short with HREN). */
#define BOARD_DC_DIV      (PWM_AUDIO_AK_CENTRE ? 2u : 1u)
#define BOARD_DC_SPAN     (BOARD_PERIOD_Q4 / BOARD_DC_DIV)
#define BOARD_PER_REG     (BOARD_DC_SPAN - 16u)
#define BOARD_NEUTRAL_Q4  (BOARD_DC_SPAN / 2u)
#define BOARD_HALF_WORDS  (NORA_PWM_AUDIO_FRAMES_PER_BLOCK * BOARD_FACTOR)
#define BOARD_DMA_WORDS   (2u * BOARD_HALF_WORDS)

#if APP_PWM_OUTPUT == 1
#define BOARD_CHANNELS (1u + MPQ6612A_STEREO)
#if PWM_AUDIO_SCOPE_ONLY
#define BOARD_MIN_DUTY_Q4 256u
#else
/* MPQ6612A tMON typ 200 ns; 500 ns initial headroom, rounded UP to whole
 * PWM clocks (the legacy float path rounded down: 399 -> 499.6 ns). */
#define BOARD_MIN_DUTY_Q4 ((((SONORA_CLOCK_PWM_MCLK_HZ) + 1999999u) / 2000000u) << 4)
#endif
#define BOARD_L_RP_H MPQ6612A_IN1_RP
#define BOARD_L_RP_L MPQ6612A_IN2_RP
#if MPQ6612A_STEREO
#define BOARD_R_RP_H MPQ6612A_RIGHT_IN1_RP
#define BOARD_R_RP_L MPQ6612A_RIGHT_IN2_RP
#endif
#define BOARD_DEAD_TIME   0u    /* integrated bridge handles shoot-through */
#else
#define BOARD_CHANNELS 2u
#define BOARD_MIN_DUTY_Q4 256u  /* 0x100: dsPIC33AK512 minimum PG register value */
/* PGxDT plain ticks (not Q4) at 798.72 MHz, ~20 ns. Estimate carried over
 * from the legacy path; see classic_audio_pwm.c. */
#define BOARD_DEAD_TIME   16u
/* Legacy MIC4607 pin map, unchanged. */
#define BOARD_L_RP_H ((nora_gpio_rp_t)28u)
#define BOARD_L_RP_L ((nora_gpio_rp_t)97u)
#define BOARD_R_RP_H ((nora_gpio_rp_t)82u)
#define BOARD_R_RP_L ((nora_gpio_rp_t)98u)
#endif

#if PWM_AUDIO_AK_HREN && APP_PWM_OUTPUT != 1
#error "HREN rescales PGxDT to 1/16 tick; the MIC4607 dead time would collapse"
#endif
#if (BOARD_PERIOD_Q4 % (2u * BOARD_DC_DIV)) != 0u || (BOARD_MIN_DUTY_Q4 % BOARD_DC_DIV) != 0u
#error "centre-aligned PWM needs an even half period and an even minimum pulse"
#endif

#if PWM_AUDIO_AK_INTERP && BOARD_FACTOR != 4
#error "PWM_AUDIO_AK_INTERP has 48 -> 192 kHz coefficients only (MPQ6612A_CARRIER_FACTOR=4)"
#endif
#if PWM_AUDIO_AK_NS2 && !PWM_AUDIO_AK_INTERP
#error "PWM_AUDIO_AK_NS2 shapes the 192 kHz interpolator output; enable PWM_AUDIO_AK_INTERP"
#endif

#if PWM_AUDIO_AK_3LEVEL
#if APP_PWM_OUTPUT != 1 || BOARD_CHANNELS != 1u || BOARD_FACTOR != 4
#error "PWM_AUDIO_AK_3LEVEL: MPQ6612A mono at a 192 kHz carrier only"
#endif
#if PWM_AUDIO_AK_CENTRE || !PWM_AUDIO_AK_HREN || !PWM_AUDIO_AK_INTERP || PWM_AUDIO_AK_NS2
#error "PWM_AUDIO_AK_3LEVEL needs CENTRE=0, HREN=1 (Dual PWM + HR forbids Complementary), INTERP=1, NS2=0"
#endif
/* Minimum window (also in scope builds: P2 checks the real 500 ns pulses),
 * guard between L windows (>= t3/t4 420 ns output shift), soft-split knee
 * and the DMA trigger point, all Q4. */
#define BOARD_3L_TMIN_Q4  ((((SONORA_CLOCK_PWM_MCLK_HZ) + 1999999u) / 2000000u) << 4)
#define BOARD_3L_GUARD_Q4 ((((SONORA_CLOCK_PWM_MCLK_HZ) + 500000u) / 1000000u) * 16u)  /* 1 us */
#define BOARD_3L_SOFT_Q4  ((((SONORA_CLOCK_PWM_MCLK_HZ) + 500000u) / 1000000u) * 4u)   /* 250 ns */
#if BOARD_PERIOD_Q4 <= 2u * (BOARD_3L_GUARD_Q4 + BOARD_3L_TMIN_Q4)
#error "3-level: two minimum windows plus guards do not fit one carrier period"
#endif
#endif

#if NORA_PWM_AUDIO_MAX_CHANNELS != BOARD_CHANNELS
#error "nora_pwm_audio_conf.h channel count does not match this board"
#endif
#if (BOARD_PERIOD_Q4 % 2u) != 0u || BOARD_MIN_DUTY_Q4 * 2u >= BOARD_PERIOD_Q4
#error "PWM period / minimum pulse cannot form a v1 modulator"
#endif

static volatile uint32_t s_duty_l[BOARD_DMA_WORDS] __attribute__((aligned(4)));
#if BOARD_CHANNELS == 2
static volatile uint32_t s_duty_r[BOARD_DMA_WORDS] __attribute__((aligned(4)));
#endif
#if PWM_AUDIO_AK_3LEVEL
/* s_duty_l feeds PG5PHASE (DMA4); these feed PG5DC, PG5TRIGB, PG5TRIGA. */
static volatile uint32_t s_3l_dc[BOARD_DMA_WORDS] __attribute__((aligned(4)));
static volatile uint32_t s_3l_tb[BOARD_DMA_WORDS] __attribute__((aligned(4)));
static volatile uint32_t s_3l_ta[BOARD_DMA_WORDS] __attribute__((aligned(4)));
#endif
#if PWM_AUDIO_AK_3LEVEL
/* Extra PG5 data-register channels, in DMA priority (= write) order. */
static const nora_dma_channel_t s_3l_ch[3] = {
    NORA_DMA_CHANNEL_5, NORA_DMA_CHANNEL_6, NORA_DMA_CHANNEL_7 };
static volatile uint32_t *const s_3l_buf[3] = { s_3l_dc, s_3l_tb, s_3l_ta };
#endif
static uint8_t s_channels;
static bool s_hw_up;              /* PG/DMA configured since the last stop */
static volatile bool s_released; /* outputs follow the PWM */
static bool s_lab_mode;           /* explicit *cp55xx power-stage bypass */
static volatile uint32_t s_half, s_done, s_dma_errors, s_phase_errors, s_overlaps;
static uint32_t s_hr_errors;      /* HR PLL not ready at start / lost while released */
#if PWM_AUDIO_SCOPE_ONLY
static volatile uint32_t s_duty_min[BOARD_CHANNELS], s_duty_max[BOARD_CHANNELS];
#endif
/* refill_half cost in high-res timer counts (preemption by higher ISRs included). */
static uint32_t s_refill_cnt_max, s_refill_cnt_sum, s_refill_n;

#if PWM_AUDIO_AK_INTERP
#if PWM_AUDIO_AK_3LEVEL
#include "board/audio/pwm_audio_interp8_coeffs.h"
#define PWM_AUDIO_INTERP_TAPS PWM_AUDIO_INTERP8_TAPS
#else
#include "board/audio/pwm_audio_interp_coeffs.h"
#endif
#define BOARD_DC_MIN    (BOARD_MIN_DUTY_Q4 / BOARD_DC_DIV)
#define BOARD_DC_MAX    (BOARD_DC_SPAN - BOARD_DC_MIN)
#define BOARD_DC_STEP   (PWM_AUDIO_AK_HREN ? 1.0f : 16.0f)

/* One channel: the 48 kHz duty history (centred on neutral, doubled so the
 * newest TAPS samples are always contiguous) and the shaper state. */
typedef struct {
    float hist[2u * PWM_AUDIO_INTERP_TAPS];
    uint8_t pos;
    float e1, e2;
} board_shape_t;
static board_shape_t s_shape[BOARD_CHANNELS];
#if PWM_AUDIO_AK_NS2
static uint32_t s_rng = 0x2545F491u;
#endif
static uint32_t s_clips;

#if PWM_AUDIO_AK_3LEVEL
static float s_3l_dmax;    /* largest |net| (Q4) whose two windows keep the guards */
static int32_t s_3l_tn;    /* previous period's tn: first half of this PHASE */
#endif

static void shape_reset(void)
{
    unsigned ch, j;
#if PWM_AUDIO_AK_3LEVEL
    {   /* tp + tn = 2 tmin + 2 c(d) <= T - 2 guard  ->  c <= cmax */
        const float a = (float)BOARD_3L_SOFT_Q4;
        const float cmax = 0.5f * (float)(BOARD_PERIOD_Q4 - 2u * BOARD_3L_GUARD_Q4) -
                           (float)BOARD_3L_TMIN_Q4;
        s_3l_dmax = 2.0f * sqrtf((cmax + a) * (cmax + a) - a * a);
        s_3l_tn = (int32_t)BOARD_3L_TMIN_Q4;
    }
#endif
    for (ch = 0u; ch < BOARD_CHANNELS; ++ch) {
        for (j = 0u; j < 2u * PWM_AUDIO_INTERP_TAPS; ++j) s_shape[ch].hist[j] = 0.0f;
        s_shape[ch].pos = 0u;
        s_shape[ch].e1 = s_shape[ch].e2 = 0.0f;
    }
}

#if PWM_AUDIO_AK_NS2
/* TPDF in [-1, 1) step units from two 24-bit LCG draws. */
static float tpdf(void)
{
    int32_t a, b;
    s_rng = s_rng * 1664525u + 1013904223u; a = (int32_t)(s_rng >> 8);
    s_rng = s_rng * 1664525u + 1013904223u; b = (int32_t)(s_rng >> 8);
    return (float)(a - b) * (1.0f / 16777216.0f);
}
#endif

#if PWM_AUDIO_AK_3LEVEL
/* Word w of the four DMA buffers. tp: IN2 low window centred at T/2. tn_prev
 * / tn: IN1 low from SOC to PHASE and from DC to EOC. Even widths, so every
 * half is a whole Q4 count. */
static void put_3level(uint16_t w, int32_t tp, int32_t tn_prev, int32_t tn)
{
    s_3l_ta[w] = (uint32_t)(BOARD_PERIOD_Q4 / 2u) - (uint32_t)(tp >> 1);
    s_3l_tb[w] = (uint32_t)(BOARD_PERIOD_Q4 / 2u) + (uint32_t)(tp >> 1);
    s_duty_l[w] = (uint32_t)(tn_prev >> 1);
    s_3l_dc[w] = BOARD_PERIOD_Q4 - (uint32_t)(tn >> 1);
}

static void fill_neutral_3level(void)
{
    uint16_t i;
    for (i = 0u; i < BOARD_DMA_WORDS; ++i)
        put_3level(i, (int32_t)BOARD_3L_TMIN_Q4, (int32_t)BOARD_3L_TMIN_Q4,
                   (int32_t)BOARD_3L_TMIN_Q4);
}

static int32_t even_q4(float t)
{
    return 2 * (int32_t)(0.5f * t + 0.5f);
}

/* Push one 48 kHz duty and write the 4 carrier periods for it. Phase 2k is
 * the +V window centre of period k (T/2), phase 2k+1 the -V centre on its end
 * boundary. The net keeps the 2-level gain: d = 2 (duty - neutral). */
static void shape_frame_3level(board_shape_t *st, uint32_t duty, uint16_t w)
{
    const float x = (float)duty - (float)BOARD_NEUTRAL_Q4;
    const float *win;
    const float a = (float)BOARD_3L_SOFT_Q4;
    const int32_t room = (int32_t)(BOARD_PERIOD_Q4 - 2u * BOARD_3L_GUARD_Q4);
    unsigned k, j;
    float acc[PWM_AUDIO_INTERP8_PHASES];
    st->pos = (uint8_t)(st->pos + 1u == PWM_AUDIO_INTERP_TAPS ? 0u : st->pos + 1u);
    st->hist[st->pos] = x;
    st->hist[st->pos + PWM_AUDIO_INTERP_TAPS] = x;
    win = &st->hist[st->pos + 1u];
    {
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        float a4 = 0.0f, a5 = 0.0f, a6 = 0.0f, a7 = 0.0f;
        for (j = 0u; j < PWM_AUDIO_INTERP_TAPS; ++j) {
            const float v = win[j];
            const float *c = pwm_audio_interp8_coeffs[j];
            a0 += c[0] * v; a1 += c[1] * v; a2 += c[2] * v; a3 += c[3] * v;
            a4 += c[4] * v; a5 += c[5] * v; a6 += c[6] * v; a7 += c[7] * v;
        }
        acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
        acc[4] = a4; acc[5] = a5; acc[6] = a6; acc[7] = a7;
    }
    for (k = 0u; k < 4u; ++k) {
        float dp = 2.0f * acc[2u * k], dn = 2.0f * acc[2u * k + 1u];
        int32_t tp, tn, wider;
        if (dp > s_3l_dmax) { dp = s_3l_dmax; ++s_clips; }
        if (dp < -s_3l_dmax) { dp = -s_3l_dmax; ++s_clips; }
        if (dn > s_3l_dmax) { dn = s_3l_dmax; ++s_clips; }
        if (dn < -s_3l_dmax) { dn = -s_3l_dmax; ++s_clips; }
        tp = even_q4((float)BOARD_3L_TMIN_Q4 + sqrtf(a * a + 0.25f * dp * dp) - a + 0.5f * dp);
        tn = even_q4((float)BOARD_3L_TMIN_Q4 + sqrtf(a * a + 0.25f * dn * dn) - a - 0.5f * dn);
        /* tp sits between the previous and this period's -V windows. */
        wider = tn > s_3l_tn ? tn : s_3l_tn;
        if (tp > room - wider) { tp = (room - wider) & ~1; ++s_clips; }
        put_3level((uint16_t)(w + k), tp, s_3l_tn, tn);
        s_3l_tn = tn;
    }
}
#else

/* Push one 48 kHz duty and write the 4 carrier-rate duties for it. */
static void shape_frame(board_shape_t *st, uint32_t duty, volatile uint32_t *out)
{
    const float x = (float)duty - (float)BOARD_NEUTRAL_Q4;
    const float *win;
    unsigned k, j;
    st->pos = (uint8_t)(st->pos + 1u == PWM_AUDIO_INTERP_TAPS ? 0u : st->pos + 1u);
    st->hist[st->pos] = x;
    st->hist[st->pos + PWM_AUDIO_INTERP_TAPS] = x;
    win = &st->hist[st->pos + 1u];   /* win[TAPS-1] = x, the newest */
    float acc[PWM_AUDIO_INTERP_PHASES];
    /* All four phases in one pass: four independent accumulators keep the
     * FPU MAC pipeline full (one accumulator stalls on every mac.s) and each
     * history sample is loaded once. */
    {
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        for (j = 0u; j < PWM_AUDIO_INTERP_TAPS; ++j) {
            const float w = win[j];
            const float *c = pwm_audio_interp_coeffs[j];
            a0 += c[0] * w; a1 += c[1] * w; a2 += c[2] * w; a3 += c[3] * w;
        }
        acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
    }
    for (k = 0u; k < PWM_AUDIO_INTERP_PHASES; ++k) {
        float y = acc[k] * (1.0f / BOARD_DC_STEP), q;
        int32_t d;
#if PWM_AUDIO_AK_NS2
        {
            const float u = y - (2.0f * st->e1 - st->e2);
            const float v = u + tpdf();
            q = (float)(int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
            st->e2 = st->e1;
            st->e1 = q - u;
        }
#else
        q = (float)(int32_t)(y < 0.0f ? y - 0.5f : y + 0.5f);
#endif
        d = (int32_t)BOARD_NEUTRAL_Q4 + (int32_t)(q * BOARD_DC_STEP);
        if (d < (int32_t)BOARD_DC_MIN) { d = (int32_t)BOARD_DC_MIN; ++s_clips; }
        if (d > (int32_t)BOARD_DC_MAX) { d = (int32_t)BOARD_DC_MAX; ++s_clips; }
        out[k] = (uint32_t)d;
    }
}
#endif /* PWM_AUDIO_AK_3LEVEL */
#endif /* PWM_AUDIO_AK_INTERP */

#if APP_PWM_OUTPUT == 1
static void stage_sleep_all(void)
{
    (void)nora_gpio_rp_clear(MPQ6612A_SLEEP_RP);
#if MPQ6612A_STEREO
    (void)nora_gpio_rp_clear(MPQ6612A_RIGHT_SLEEP_RP);
#endif
}
static bool stage_fault(void)
{
#if PWM_AUDIO_SCOPE_ONLY
    return false; /* Diagnostics are inactive while both bridges sleep. */
#else
    if (s_lab_mode) return false;
    return nora_gpio_rp_read(MPQ6612A_FAULT_RP) != NORA_GPIO_LEVEL_HIGH
#if MPQ6612A_STEREO
        || nora_gpio_rp_read(MPQ6612A_RIGHT_FAULT_RP) != NORA_GPIO_LEVEL_HIGH
#endif
        ;
#endif
}
#endif

static void fill_neutral(volatile uint32_t *buf, uint16_t from, uint16_t count)
{
    uint16_t i;
    for (i = 0u; i < count; ++i) buf[from + i] = BOARD_NEUTRAL_Q4;
}

/* Pin override of one generator: both pins forced to `high`. 3-level runs
 * PWM5L active-low and the override data passes the polarity stage (FRM
 * DS70005320E Fig. 4-16), so the HAL OVRDAT=00/11 would drive IN2 opposite
 * to IN1 (full reverse); write OVRDAT here with the L bit inverted. */
static bool board_override(nora_pwm_generator_id_t id, bool enable, bool high,
                           bool synchronous)
{
#if PWM_AUDIO_AK_3LEVEL
    uint32_t v = PG5IOCON2 & ~(_PG5IOCON2_OSYNC_MASK | _PG5IOCON2_OVRDAT_MASK |
                               _PG5IOCON2_OVRENH_MASK | _PG5IOCON2_OVRENL_MASK);
    (void)id;
    if (!synchronous) v |= 1u << _PG5IOCON2_OSYNC_POSITION;  /* 01 = immediate */
    v |= (high ? 2u : 1u) << _PG5IOCON2_OVRDAT_POSITION;      /* [1]=H, [0]=L data */
    if (enable) v |= _PG5IOCON2_OVRENH_MASK | _PG5IOCON2_OVRENL_MASK;
    PG5IOCON2 = v;
    return true;
#else
    return nora_pwm_generator_override(id, enable, high, synchronous);
#endif
}

/* EMERGENCY_SAFE: no waits, callable from any ISR. A foreground override
 * read-modify-write it interrupts may be lost; the stage is already off by
 * then on MPQ, and both pins are forced again on the next stop(). */
static void board_emergency_safe(void)
{
    s_released = false;
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();   /* both bridges off first */
#endif
    (void)board_override(NORA_PWM_GEN_5, true, false, false);
#if BOARD_CHANNELS == 2
    (void)nora_pwm_generator_override(NORA_PWM_GEN_6, true, false, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
#if PWM_AUDIO_AK_3LEVEL
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_6, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_7, false);
#endif
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
}

/* The completed half must be inactive on every DMA before and after copy.
 * Both channels use PG5's event, with at most one word arbitration distance. */
static bool dma_copy_window(uint16_t offset, bool reserve)
{
    uint32_t left = (nora_dma_read_src(NORA_DMA_CHANNEL_4) -
                     (uint32_t)s_duty_l) / sizeof(uint32_t);
    if (left >= BOARD_DMA_WORDS || left / BOARD_HALF_WORDS == offset / BOARD_HALF_WORDS)
        return false;
#if BOARD_CHANNELS == 2
    if (s_channels == 2u) {
        uint32_t right = (nora_dma_read_src(NORA_DMA_CHANNEL_5) -
                          (uint32_t)s_duty_r) / sizeof(uint32_t);
        uint32_t distance = left > right ? left - right : right - left;
        if (right >= BOARD_DMA_WORDS || (distance > 1u && distance != BOARD_DMA_WORDS - 1u)) {
            ++s_phase_errors;
            return false;
        }
        if (right / BOARD_HALF_WORDS == offset / BOARD_HALF_WORDS) return false;
        if (reserve && BOARD_HALF_WORDS - right % BOARD_HALF_WORDS <= 4u) return false;
    }
#endif
#if PWM_AUDIO_AK_3LEVEL
    {   /* DMA5-7 follow DMA4 on the same EOC, at most one word behind. */
        unsigned c;
        for (c = 0u; c < 3u; ++c) {
            uint32_t pos = (nora_dma_read_src(s_3l_ch[c]) - (uint32_t)s_3l_buf[c]) /
                           sizeof(uint32_t);
            uint32_t distance = left > pos ? left - pos : pos - left;
            if (pos >= BOARD_DMA_WORDS || (distance > 1u && distance != BOARD_DMA_WORDS - 1u)) {
                ++s_phase_errors;
                return false;
            }
            if (pos / BOARD_HALF_WORDS == offset / BOARD_HALF_WORDS) return false;
            if (reserve && BOARD_HALF_WORDS - pos % BOARD_HALF_WORDS <= 4u) return false;
        }
    }
#endif
    return !reserve || BOARD_HALF_WORDS - left % BOARD_HALF_WORDS > 4u;
}

static void refill_half(uint16_t offset)
{
    const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS];
    bool have_data;
    uint16_t i, k, w;
    /* DMA4's IRQ can beat DMA5's last transfer by a few bus cycles. */
    unsigned attempt = 0;
    while (!dma_copy_window(offset, true) && ++attempt < 32u) {}
    if (attempt == 32u) {
        ++s_overlaps;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    uint32_t t0 = nora_high_res_timer_is_initialized() ? nora_high_res_timer_get_count() : 0u;
    have_data = nora_pwm_audio_core_take(duty);
    w = offset;
#if PWM_AUDIO_SCOPE_ONLY
    for (unsigned ch = 0; ch < BOARD_CHANNELS; ++ch) {
        s_duty_min[ch] = BOARD_DC_SPAN;
        s_duty_max[ch] = 0;
    }
#endif
    for (i = 0u; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) {
        uint32_t l = have_data ? duty[0][i] : BOARD_NEUTRAL_Q4;
#if BOARD_CHANNELS == 2
        uint32_t r = have_data && s_channels == 2u ? duty[1][i] : BOARD_NEUTRAL_Q4;
#endif
#if PWM_AUDIO_AK_3LEVEL
        (void)k;
        shape_frame_3level(&s_shape[0], l, w);
        w = (uint16_t)(w + BOARD_FACTOR);
#elif PWM_AUDIO_AK_INTERP
        (void)k;
        shape_frame(&s_shape[0], l, &s_duty_l[w]);
#if BOARD_CHANNELS == 2
        shape_frame(&s_shape[1], r, &s_duty_r[w]);
#endif
        w = (uint16_t)(w + BOARD_FACTOR);
#else
        for (k = 0u; k < BOARD_FACTOR; ++k, ++w) {
            s_duty_l[w] = l;
#if BOARD_CHANNELS == 2
            s_duty_r[w] = r;
#endif
        }
#endif
#if PWM_AUDIO_SCOPE_ONLY
        if (l < s_duty_min[0]) s_duty_min[0] = l;
        if (l > s_duty_max[0]) s_duty_max[0] = l;
#if BOARD_CHANNELS == 2
        if (r < s_duty_min[1]) s_duty_min[1] = r;
        if (r > s_duty_max[1]) s_duty_max[1] = r;
#endif
#endif
    }
    if (have_data) nora_pwm_audio_core_release();
    if (nora_high_res_timer_is_initialized()) {
        uint32_t dt = nora_high_res_timer_elapsed_count(t0);
        if (dt > s_refill_cnt_max) s_refill_cnt_max = dt;
        if (s_refill_n < 65536u) { s_refill_cnt_sum += dt; ++s_refill_n; }
    }
    if (!dma_copy_window(offset, false)) {
        ++s_overlaps;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
    }
}

void __attribute__((interrupt, context)) _DMA4Interrupt(void)
{
    nora_dma_status_t status = nora_dma_isr_snapshot(NORA_DMA_CHANNEL_4);
    nora_dma_half_t half;
    bool bad = nora_dma_status_has_overrun(status) ||
               nora_dma_status_has_half_done_conflict(status);
#if BOARD_CHANNELS == 2
    if (s_channels == 2u) {
        nora_dma_status_t status_r = nora_dma_read_status(NORA_DMA_CHANNEL_5);
        nora_dma_clear_status(NORA_DMA_CHANNEL_5);
        bad = bad || nora_dma_status_has_overrun(status_r);
    }
#endif
#if PWM_AUDIO_AK_3LEVEL
    {
        unsigned c;
        for (c = 0u; c < 3u; ++c) {
            nora_dma_status_t st = nora_dma_read_status(s_3l_ch[c]);
            nora_dma_clear_status(s_3l_ch[c]);
            bad = bad || nora_dma_status_has_overrun(st);
        }
    }
#endif
    if (bad) {
        ++s_dma_errors;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    half = nora_dma_half_from_status(status);
    if (half == NORA_DMA_HALF_FIRST) { ++s_half; refill_half(0u); }
    else if (half == NORA_DMA_HALF_SECOND) { ++s_done; refill_half(BOARD_HALF_WORDS); }
}

static bool board_dma_config(nora_dma_channel_t ch, nora_dma_trigger_t trigger,
                             volatile uint32_t *duty_buf, volatile uint32_t *pg_dc,
                             bool irq)
{
    nora_dma_channel_cfg_t cfg = { 0 };

    cfg.src          = (volatile void *)duty_buf;
    cfg.dst          = (volatile void *)pg_dc;
    cfg.count        = BOARD_DMA_WORDS;
    cfg.src_mode     = NORA_DMA_ADDR_INCREMENT;
    cfg.dst_mode     = NORA_DMA_ADDR_FIXED;
    cfg.size         = NORA_DMA_SIZE_WORD;
    cfg.tr_mode      = NORA_DMA_TRMODE_REPEAT_ONESHOT;
    cfg.reload_count = true;
    cfg.reload_src   = true;
    cfg.reload_dst   = false;
    cfg.half_int_en  = irq;
    cfg.done_int_en  = irq;
    cfg.trigger      = trigger;
    cfg.irq_priority_set = false;   /* as the legacy path */
    cfg.irq_enable   = irq;
    return nora_dma_channel_config(ch, &cfg) && nora_dma_channel_enable(ch, true);
}

static bool board_pg_init(nora_pwm_generator_id_t id, nora_gpio_rp_t rp_h,
                          nora_gpio_rp_t rp_l, bool follow_pg5)
{
    nora_pwm_generator_cfg_t cfg = { 0 };

    cfg.period            = BOARD_PERIOD_Q4;
    cfg.duty_init         = BOARD_NEUTRAL_Q4;
    cfg.dead_time_high    = BOARD_DEAD_TIME;
    cfg.dead_time_low     = BOARD_DEAD_TIME;
    cfg.rp_h              = rp_h;
    cfg.rp_l              = rp_l;
    cfg.pen_h             = true;
    cfg.pen_l             = true;
    cfg.output_mode       = PWM_AUDIO_AK_3LEVEL ? NORA_PWM_MODE_INDEPENDENT
                                                : NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows   = follow_pg5;
    cfg.trigger_master    = NORA_PWM_GEN_5;
    cfg.is_module_master  = true;
    /* Both pins low (MPQ: coast Z/Z; MIC4607: both FETs off) until release. */
    cfg.start_overridden  = true;
    cfg.start_disabled    = true;
    if (!nora_pwm_generator_init(id, &cfg)) return false;
#if BOARD_PG_TUNED
    /* The HAL left an edge-aligned standard-resolution generator with ON=0.
     * HREN goes first, then the data registers it reinterprets (DS 16.2.1).
     * TRIGA bits[3:0] are not used outside Dual PWM; bit31=0 = first half. */
#define BOARD_PG_TUNE(n)                                                  \
    do {                                                                  \
        PG##n##CONbits.MODSEL = PWM_AUDIO_AK_CENTRE ? 4u : 0u;            \
        PG##n##CONbits.HREN = PWM_AUDIO_AK_HREN ? 1u : 0u;                \
        PG##n##PERbits.PER = BOARD_PER_REG;                               \
        PG##n##DC = BOARD_NEUTRAL_Q4;                                     \
        PG##n##TRIGA = (BOARD_PER_REG / 2u) & ~0xFu;                      \
        PG##n##EVT1bits.IEVTSEL = PWM_AUDIO_AK_TRIGA ? 1u : 0u;           \
    } while (0)
    if (id == NORA_PWM_GEN_5) BOARD_PG_TUNE(5);
#if BOARD_CHANNELS == 2
    else BOARD_PG_TUNE(6);
#endif
#undef BOARD_PG_TUNE
#endif
#if PWM_AUDIO_AK_3LEVEL
    /* Dual PWM, PWM5L active-low. Coast data for the new polarity goes in
     * before POLL flips (the bridge is asleep here anyway), then the first
     * period from neutral buffer word 0. The TRIGA write commits each DMA
     * update (UPDTRG=11, AK DS 16.5.6). */
    PG5CONbits.MODSEL = 2u;
    (void)board_override(NORA_PWM_GEN_5, true, false, false);
    PG5IOCON1bits.POLL = 1u;
    PG5PHASE = s_duty_l[0];
    PG5DC = s_3l_dc[0];
    PG5TRIGB = s_3l_tb[0];
    PG5TRIGA = s_3l_ta[0];
    PG5EVT1bits.UPDTRG = 3u;
    /* DMA trigger: TRIGA/B are waveform registers in Dual PWM and an EOC
     * write can miss the update window (CK PG4, memory 2026-10-14), so the
     * event is ADC trigger 1 = TRIGC compare at mid period. */
    PG5TRIGC = BOARD_PERIOD_Q4 / 2u;
    PG5EVT1bits.ADTR1EN1 = 0u;
    PG5EVT1bits.ADTR1EN2 = 0u;
    PG5EVT1bits.ADTR1EN3 = 1u;
    PG5EVT1bits.ADTR1PS = 0u;
    PG5EVT1bits.ADTR1OFS = 0u;
    PG5EVT1bits.IEVTSEL = 2u;
#endif
    return true;
}

/* Fine edge PLL: HRRDY after HREN; HRERR = lost lock, HW forces outputs low.
 * Writing HRERR=0 re-arms the PLL (DS 16.2). */
static bool board_hr_ready(void)
{
#if PWM_AUDIO_AK_HREN
    unsigned i;
    if (PCLKCONbits.DIVSEL != 0u) return false; /* HR forbids the divider */
    if (PCLKCONbits.HRERR) PCLKCONbits.HRERR = 0u;
    for (i = 0u; i < 100u && !PCLKCONbits.HRRDY; ++i) delay_us(10u);
    return PCLKCONbits.HRRDY && !PCLKCONbits.HRERR;
#else
    return true;
#endif
}

/* Preserve the proven CODEC XTAL -> REFI1 -> PLL2 -> CLKGEN5 PWM clock. */
static bool board_clock_ready(void)
{
    static bool prepared;
    if (!prepared) {
        nora_clock_status_t detail;
        if (sonora_clock_pwm_prepare(&detail) != SONORA_CLOCK_PWM_OK ||
            !nora_pwm_module_init(NORA_PWM_MCLK_HIGH_FREQ)) return false;
        prepared = true;
    }
    return OSCCTRLbits.PLL2RDY && CLK5CONbits.CLKRDY;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t channel_count,
                                                    nora_pwm_audio_modulator_t *mod)
{
    if (channel_count == 0u || channel_count > BOARD_CHANNELS) return NORA_PWM_AUDIO_UNSUPPORTED;
#if APP_PWM_OUTPUT == 1
    /* Keep the bridge asleep from here on, whatever happens next. */
    if (!nora_gpio_rp_config_digital_output(MPQ6612A_SLEEP_RP, false) ||
        !nora_gpio_rp_config_digital_input(MPQ6612A_FAULT_RP)) {
        stage_sleep_all();
        return NORA_PWM_AUDIO_FAULT;
    }
#if MPQ6612A_STEREO
    if (!nora_gpio_rp_config_digital_output(MPQ6612A_RIGHT_SLEEP_RP, false) ||
        !nora_gpio_rp_config_digital_input(MPQ6612A_RIGHT_FAULT_RP)) {
        stage_sleep_all();
        return NORA_PWM_AUDIO_FAULT;
    }
    if (channel_count == 1u &&
        (!nora_gpio_rp_config_digital_output(BOARD_R_RP_H, false) ||
         !nora_gpio_rp_config_digital_output(BOARD_R_RP_L, false))) return NORA_PWM_AUDIO_FAULT;
#endif
#else
    /* A stereo board run mono: hold the unused R driver inputs low. */
    if (channel_count == 1u &&
        (!nora_gpio_rp_config_digital_output(BOARD_R_RP_H, false) ||
         !nora_gpio_rp_config_digital_output(BOARD_R_RP_L, false))) {
        return NORA_PWM_AUDIO_FAULT;
    }
#endif
    if (!nora_pwm_audio_modulator_setup(mod, BOARD_DC_SPAN, BOARD_MIN_DUTY_Q4 / BOARD_DC_DIV)) {
        return NORA_PWM_AUDIO_FAULT;
    }
    s_channels = channel_count;
    return NORA_PWM_AUDIO_OK;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_start(void)
{
    bool ok;
    nora_pwm_audio_status_t st;
    s_released = false;
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();
#endif
    /* Own all resources before any partial failure can occur. */
    s_hw_up = true;
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
#if BOARD_CHANNELS == 2
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
#if PWM_AUDIO_AK_INTERP
    shape_reset();
#endif
    s_refill_cnt_max = s_refill_cnt_sum = s_refill_n = 0u;
#if PWM_AUDIO_AK_3LEVEL
    fill_neutral_3level();
#else
    fill_neutral(s_duty_l, 0u, BOARD_DMA_WORDS);
#endif
#if BOARD_CHANNELS == 2
    fill_neutral(s_duty_r, 0u, BOARD_DMA_WORDS);
#endif
    if (!board_clock_ready()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
        return NORA_PWM_AUDIO_FAULT;
    }
    ok = board_pg_init(NORA_PWM_GEN_5, BOARD_L_RP_H, BOARD_L_RP_L, false);
#if BOARD_CHANNELS == 2
    if (ok && s_channels == 2u) {
        ok = board_pg_init(NORA_PWM_GEN_6, BOARD_R_RP_H, BOARD_R_RP_L, true) &&
             board_dma_config(NORA_DMA_CHANNEL_5, NORA_DMA_TRIGGER_PWM_GEN5,
                              s_duty_r, &PG6DC, false);
    }
#endif
#if PWM_AUDIO_AK_3LEVEL
    {
        static volatile uint32_t *const dst[3] = { &PG5DC, &PG5TRIGB, &PG5TRIGA };
        unsigned c;
        for (c = 0u; ok && c < 3u; ++c)
            ok = board_dma_config(s_3l_ch[c], NORA_DMA_TRIGGER_PWM_GEN5,
                                  s_3l_buf[c], dst[c], false);
    }
    if (ok) ok = board_dma_config(NORA_DMA_CHANNEL_4, NORA_DMA_TRIGGER_PWM_GEN5,
                                 s_duty_l, &PG5PHASE, true);
#else
    if (ok) ok = board_dma_config(NORA_DMA_CHANNEL_4, NORA_DMA_TRIGGER_PWM_GEN5,
                                 s_duty_l, &PG5DC, true);
#endif
#if BOARD_CHANNELS == 2
    if (ok && s_channels == 2u) ok = nora_pwm_generator_run(NORA_PWM_GEN_6, true);
#endif
    if (ok) ok = nora_pwm_generator_run(NORA_PWM_GEN_5, true);
    if (!ok) goto fail;
    /* Outputs are still overridden low; never release on an unlocked HR PLL. */
    if (!board_hr_ready()) {
        ++s_hr_errors;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
        goto fail;
    }
#if APP_PWM_OUTPUT == 1
    if (!PWM_AUDIO_SCOPE_ONLY || s_lab_mode) {
        nora_pwm_audio_get_status(&st);
        if (st.fault_flags) goto fail;
        (void)nora_gpio_rp_set(MPQ6612A_SLEEP_RP);
#if MPQ6612A_STEREO
        if (s_channels == 2u) (void)nora_gpio_rp_set(MPQ6612A_RIGHT_SLEEP_RP);
#endif
        delay_us(1000u); /* coast wake: do not enter low-IQ brake */
        if (stage_fault()) {
            board_emergency_safe();
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
            goto fail;
        }
    }
#endif
    nora_pwm_audio_get_status(&st);
    if (st.fault_flags) goto fail;
#if BOARD_CHANNELS == 2
    if (s_channels == 2u &&
        !nora_pwm_generator_override(NORA_PWM_GEN_6, false, false, true)) goto fail;
#endif
    if (!board_override(NORA_PWM_GEN_5, false, false, true)) goto fail;
    nora_pwm_audio_get_status(&st);
    if (st.fault_flags) goto fail;
    pwm_audio_board_service(); /* establish progress baseline before release */
    s_released = true;
    return NORA_PWM_AUDIO_OK;
fail:
    board_emergency_safe();
    nora_pwm_audio_backend_stop();
    return NORA_PWM_AUDIO_FAULT;
}

void nora_pwm_audio_backend_stop(void)
{
    s_released = false; /* core has stopped producer before calling backend */
#if APP_PWM_OUTPUT == 1
    if (!s_hw_up) { stage_sleep_all(); return; }
#else
    if (!s_hw_up) return;
#endif
    s_hw_up = false;
    (void)board_override(NORA_PWM_GEN_5, true,
                         APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY, true);
#if BOARD_CHANNELS == 2
    (void)nora_pwm_generator_override(NORA_PWM_GEN_6, true,
                                     APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY, true);
#endif
    delay_us(50u);
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();
    delay_us(1000u);
#endif
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
#if BOARD_CHANNELS == 2
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
#if PWM_AUDIO_AK_3LEVEL
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_6, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_7, false);
#endif
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
}

void pwm_audio_board_service(void)
{
    static uint32_t last_ms, last_events;
    uint32_t now = GetTicks();
    if (!s_released) { last_ms = now; last_events = s_half + s_done; return; }
    if (!PG5CONbits.ON || !DMA4CHbits.CHEN
#if BOARD_CHANNELS == 2
        || (s_channels == 2u && (!PG6CONbits.ON || !DMA5CHbits.CHEN))
#endif
#if PWM_AUDIO_AK_3LEVEL
        || !DMA5CHbits.CHEN || !DMA6CHbits.CHEN || !DMA7CHbits.CHEN
#endif
        ) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    if ((uint32_t)(now - last_ms) >= 1000u) {
        uint32_t events = s_half + s_done;
        if (events == last_events) {
            board_emergency_safe();
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
            return;
        }
        last_events = events;
        last_ms = now;
    }
    if (!OSCCTRLbits.PLL2RDY || !CLK5CONbits.CLKRDY
#if PWM_AUDIO_AK_HREN
        || !PCLKCONbits.HRRDY || PCLKCONbits.HRERR
#endif
        ) {
#if PWM_AUDIO_AK_HREN
        ++s_hr_errors;
#endif
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
    }
#if APP_PWM_OUTPUT == 1
    if (s_released && stage_fault()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
    }
#if PWM_AUDIO_SCOPE_ONLY
    if (s_released && !s_lab_mode &&
        (nora_gpio_rp_read(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW ||
         nora_gpio_rp_read_output(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
#if MPQ6612A_STEREO
         || nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
         || nora_gpio_rp_read_output(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
#endif
        )) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
    }
#endif
#endif
}

void pwm_audio_board_set_lab_mode(bool enabled)
{
    s_lab_mode = enabled;
}

void pwm_audio_board_control_print(void)
{
    nora_pwm_audio_status_t st;
    nora_pwm_audio_get_status(&st);
    printf(" PWMcontrol lab=%u pg=%u dma=%u fault=%lu written=%lu underrun=%lu",
        (unsigned)s_lab_mode, (unsigned)PG5CONbits.ON,
        (unsigned)DMA4CHbits.CHEN, (unsigned long)st.fault_flags,
        (unsigned long)st.written_blocks, (unsigned long)st.underrun_blocks);
#if APP_PWM_OUTPUT == 1
    printf(" nSLEEP=%u nFAULT=%u",
        (unsigned)(nora_gpio_rp_read_output(MPQ6612A_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)(nora_gpio_rp_read(MPQ6612A_FAULT_RP) == NORA_GPIO_LEVEL_HIGH));
#if MPQ6612A_STEREO
    printf(" nSLEEP_R=%u nFAULT_R=%u",
        (unsigned)(nora_gpio_rp_read_output(MPQ6612A_RIGHT_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)(nora_gpio_rp_read(MPQ6612A_RIGHT_FAULT_RP) == NORA_GPIO_LEVEL_HIGH));
#endif
#endif
    printf(" carrier=%lu per=%lu pll2=%u clk5=%u err=%lu overlap=%lu phase_err=%lu\n",
        (unsigned long)(SAMPLE_RATE * BOARD_FACTOR), (unsigned long)PG5PER,
        (unsigned)OSCCTRLbits.PLL2RDY, (unsigned)CLK5CONbits.CLKRDY,
        (unsigned long)s_dma_errors, (unsigned long)s_overlaps, (unsigned long)s_phase_errors);
    printf(" PWMalign centre=%u hren=%u triga=%u dc_span=%lu per_reg=%lu modsel=%u hren_bit=%u ievt=%u hrrdy=%u hrerr=%u hr_err=%lu\n",
        (unsigned)PWM_AUDIO_AK_CENTRE, (unsigned)PWM_AUDIO_AK_HREN, (unsigned)PWM_AUDIO_AK_TRIGA,
        (unsigned long)BOARD_DC_SPAN, (unsigned long)BOARD_PER_REG,
        (unsigned)PG5CONbits.MODSEL, (unsigned)PG5CONbits.HREN, (unsigned)PG5EVT1bits.IEVTSEL,
        (unsigned)PCLKCONbits.HRRDY, (unsigned)PCLKCONbits.HRERR, (unsigned long)s_hr_errors);
    printf(" PWMshape 3level=%u interp=%u ns2=%u clips=%lu refill_max_us_x10=%lu refill_avg_us_x10=%lu block_us=%lu\n",
        (unsigned)PWM_AUDIO_AK_3LEVEL, (unsigned)PWM_AUDIO_AK_INTERP, (unsigned)PWM_AUDIO_AK_NS2,
#if PWM_AUDIO_AK_INTERP
        (unsigned long)s_clips,
#else
        0ul,
#endif
        (unsigned long)(nora_high_res_timer_is_initialized() ?
            nora_high_res_timer_count_to_us_x10(s_refill_cnt_max) : 0u),
        (unsigned long)(nora_high_res_timer_is_initialized() && s_refill_n ?
            nora_high_res_timer_count_to_us_x10(s_refill_cnt_sum / s_refill_n) : 0u),
        (unsigned long)(1000000ul * NORA_PWM_AUDIO_FRAMES_PER_BLOCK / SAMPLE_RATE));
}

void pwm_audio_board_scope_print(bool running, bool neutral)
{
#if PWM_AUDIO_SCOPE_ONLY
    static uint32_t last_ms;
    uint32_t now = GetTicks();
    nora_pwm_audio_status_t st;
    if ((uint32_t)(now - last_ms) < 1000u) return;
    last_ms = now;
    nora_pwm_audio_get_status(&st);
#define PWM_SCOPE_STRINGIFY_(x) #x
#define PWM_SCOPE_STRINGIFY(x) PWM_SCOPE_STRINGIFY_(x)
    printf(" PWMhal id=%s ms=%lu ready=%u pll2=%u clk5=%u pg=%u dma=%u half=%lu done=%lu err=%lu overlap=%lu phase_err=%lu written=%lu rejected=%lu underrun=%lu fault=%lu source=%s\n",
        PWM_SCOPE_STRINGIFY(PWM_AUDIO_SCOPE_TAG),
        (unsigned long)now, (unsigned)(running && s_released),
        (unsigned)OSCCTRLbits.PLL2RDY, (unsigned)CLK5CONbits.CLKRDY,
        (unsigned)PG5CONbits.ON, (unsigned)DMA4CHbits.CHEN,
        (unsigned long)s_half, (unsigned long)s_done, (unsigned long)s_dma_errors,
        (unsigned long)s_overlaps, (unsigned long)s_phase_errors,
        (unsigned long)st.written_blocks, (unsigned long)st.rejected_blocks,
        (unsigned long)st.underrun_blocks, (unsigned long)st.fault_flags,
        neutral ? "neutral" : "audio");
#undef PWM_SCOPE_STRINGIFY
#undef PWM_SCOPE_STRINGIFY_
    printf(" PWMhalL sleep=%u duty=%lu/%lu per=%lu route=%s\n",
        (unsigned)(nora_gpio_rp_read(MPQ6612A_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned long)s_duty_min[0], (unsigned long)s_duty_max[0], (unsigned long)BOARD_DC_SPAN,
#if defined(APP_TARGET_AK506) && APP_TARGET == APP_TARGET_AK506
        MPQ6612A_NANO_SLOT == 3 ? "nano-slot3" : "nano-slot2");
#else
        MPQ6612A_ENABLE_MIKROB ? "mikrob" : "xpro1");
#endif
#if MPQ6612A_STEREO
    printf(" PWMhalR sleep=%u pg=%u dma=%u duty=%lu/%lu\n",
        (unsigned)(nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)PG6CONbits.ON, (unsigned)DMA5CHbits.CHEN,
        (unsigned long)s_duty_min[1], (unsigned long)s_duty_max[1]);
#endif
#else
    (void)running; (void)neutral;
#endif
}

#else
typedef int pwm_audio_board_ak_unused_t;   /* ISO C: no empty translation unit */
#endif /* NORA_PWM_AUDIO_ENABLE */
