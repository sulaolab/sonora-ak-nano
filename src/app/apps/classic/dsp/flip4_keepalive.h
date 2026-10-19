#ifndef _FLIP4_KEEPALIVE_H
#define _FLIP4_KEEPALIVE_H

#include <stdint.h>

#if defined(ENA_FLIP4_KEEPALIVE)

/*
 * JBL Flip4 auto-mute workaround.
 *
 * This is not a generic tone generator.
 * It continuously injects a very small keep-alive signal.
 *
 * Target signal:
 *   - about 24 kHz
 *   - 48 kHz Fs : +gain, -gain, +gain, -gain, ...
 *   - 96 kHz Fs : +gain, +gain, -gain, -gain, ...
 *
 * It is added to the interleaved Q31 CODEC TX frames only (after
 * convert_codec_float_to_int()), so the PWM output, which reads the float
 * mix, never carries it. Level at the CODEC: GAIN_LIN x Post_Gain_CODEC FS.
 *
 * Remove this module when the target speaker is changed.
 */

#ifndef FLIP4_KEEPALIVE_GAIN_LIN
#define FLIP4_KEEPALIVE_GAIN_LIN    (0.01f)
#endif

extern void flip4_keepalive_init(uint32_t sample_rate_Hz);

/* tx[n * stride + ch], ch < num_ch: saturating add, 24-bit aligned. */
extern void flip4_keepalive_process_q31(int32_t* tx,
                                        int      stride,
                                        int      num_ch,
                                        int      samples);

#endif /* defined(ENA_FLIP4_KEEPALIVE) */

#endif /* _FLIP4_KEEPALIVE_H */
