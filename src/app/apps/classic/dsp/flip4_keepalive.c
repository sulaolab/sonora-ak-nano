#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#include <stdint.h>
#include <stddef.h>

#include "flip4_keepalive.h"

#if defined(ENA_FLIP4_KEEPALIVE)

#include "apps/shared/float_conversion.h"   /* Post_Gain_CODEC */

/*
 * JBL Flip4 auto-mute workaround.
 *
 * Keep this module intentionally small and product-specific.
 * No enable/disable.
 * No frequency API.
 * No phase API.
 * No sine calculation.
 */

#define FLIP4_KEEPALIVE_TARGET_HZ   (24000UL)
#define FLIP4_KEEPALIVE_MIN_HALF    (1U)
#define FLIP4_KEEPALIVE_Q31_MASK    ((int32_t)0xFFFFFF00UL)   /* WM8904 24-bit */

static uint16_t s_half_period_samples = 1U;
static uint16_t s_half_count          = 0U;
static int8_t   s_sign                = 1;

static uint16_t local_calc_half_period(uint32_t sample_rate_Hz)
{
    uint32_t half_period;

    if (sample_rate_Hz == 0U)
    {
        return FLIP4_KEEPALIVE_MIN_HALF;
    }

    /*
     * half period [samples] = Fs / (2 * target_freq)
     *
     * 48 kHz -> 1 sample
     * 96 kHz -> 2 samples
     *
     * Rounded to nearest integer.
     */
    half_period = (sample_rate_Hz + FLIP4_KEEPALIVE_TARGET_HZ)
                / (2UL * FLIP4_KEEPALIVE_TARGET_HZ);

    if (half_period < FLIP4_KEEPALIVE_MIN_HALF)
    {
        half_period = FLIP4_KEEPALIVE_MIN_HALF;
    }

    if (half_period > 65535UL)
    {
        half_period = 65535UL;
    }

    return (uint16_t)half_period;
}

void flip4_keepalive_init(uint32_t sample_rate_Hz)
{
    s_half_period_samples = local_calc_half_period(sample_rate_Hz);
    s_half_count          = 0U;
    s_sign                = 1;
}

/* Saturating add, kept on the WM8904 24-bit boundary (k is already aligned). */
static inline int32_t local_add_sat_q31(int32_t x, int32_t k)
{
    const int64_t s = (int64_t)x + (int64_t)k;

    if (s > (int64_t)(INT32_MAX & FLIP4_KEEPALIVE_Q31_MASK))
    {
        return (int32_t)(INT32_MAX & FLIP4_KEEPALIVE_Q31_MASK);
    }
    if (s < (int64_t)INT32_MIN)
    {
        return INT32_MIN;
    }
    return (int32_t)s;
}

void flip4_keepalive_process_q31(int32_t* tx,
                                 int      stride,
                                 int      num_ch,
                                 int      samples)
{
    uint16_t count = s_half_count;
    int8_t   sign  = s_sign;
    int32_t  k;

    if ((tx == NULL) || (stride <= 0) || (num_ch <= 0) || (num_ch > stride) || (samples <= 0))
    {
        return;
    }

    /*
     * Same level at the CODEC as the old float-domain add before
     * convert_codec_float_to_int(): gain x Post_Gain_CODEC of full scale.
     */
    k = (int32_t)(FLIP4_KEEPALIVE_GAIN_LIN * Post_Gain_CODEC * 2147483648.0f);
    k &= FLIP4_KEEPALIVE_Q31_MASK;

    /*
     * 48 kHz Fs (half period 1): +k, -k, +k, -k, ...
     * 96 kHz Fs (half period 2): +k, +k, -k, -k, ...
     * All channels of one frame get the same sign.
     */
    for (int n = 0; n < samples; n++)
    {
        int32_t*      frame = &tx[n * stride];
        const int32_t add   = (sign > 0) ? k : -k;

        for (int ch = 0; ch < num_ch; ch++)
        {
            frame[ch] = local_add_sat_q31(frame[ch], add);
        }

        count++;
        if (count >= s_half_period_samples)
        {
            count = 0U;
            sign  = (int8_t)-sign;
        }
    }

    s_half_count = count;
    s_sign       = sign;
}

#endif /* defined(ENA_FLIP4_KEEPALIVE) */
