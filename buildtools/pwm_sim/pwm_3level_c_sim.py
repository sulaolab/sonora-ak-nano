"""Theme 3: the REAL firmware 3-level shaper on the host.

Extracts shape_frame_3level() / put_3level() and the 3-level constants from
pwm_audio_board_ak.c verbatim, compiles them (float32, as pwm_shape_c_sim.py),
feeds a 48 kHz sine through the real HAL modulator, and decodes the four DMA
words per period back into IN1/IN2 edges exactly as Dual PWM + POLL=1 will
drive them:
  IN2 (PWM5L, active-low) low  from TRIGA to TRIGB           -> +V
  IN1 (PWM5H, active-high) low from SOC to PHASE, DC to EOC  -> -V
Checks per period: windows inside the period, guards >= 1 us, no LL (coast),
then runs the exact edge spectrum of pwm_3level_sim.py (ideal bridge).

usage: python pwm_3level_c_sim.py [levels_dBFS ...]
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import pwm_3level_sim as m3

ROOT = Path(__file__).resolve().parents[2]
CC = Path(r'C:\msys64\usr\bin\gcc.exe')
CLK, PER = 798.72e6, 66560
SPAN = PER                            # 3LEVEL builds CENTRE=0: edge modulator span
HALMIN = ((int(CLK) + 1999999) // 2000000) << 4

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "nora_pwm_audio_modulator.h"
#define BOARD_CHANNELS 1u
#define BOARD_PERIOD_Q4 %(per)du
#define BOARD_NEUTRAL_Q4 %(neutral)du
#define BOARD_DMA_WORDS 4u
#define SONORA_CLOCK_PWM_MCLK_HZ %(clk)du
#define PWM_AUDIO_INTERP_TAPS PWM_AUDIO_INTERP8_TAPS
#include "pwm_audio_interp8_coeffs.h"
typedef struct { float hist[2u * PWM_AUDIO_INTERP_TAPS]; uint8_t pos; float e1, e2; } board_shape_t;
static board_shape_t s_shape[1];
static uint32_t s_clips;
static volatile uint32_t s_duty_l[4], s_3l_dc[4], s_3l_tb[4], s_3l_ta[4];
%(defs)s
%(state)s
%(reset)s
%(block)s
int main(int argc, char **argv){
    double f=atof(argv[1]), amp=atof(argv[2]); int n=atoi(argv[3]);
    nora_pwm_audio_modulator_t mod;
    nora_pwm_audio_modulator_setup(&mod, %(span)du, %(halmin)du);
    shape_reset();
    for(int i=0;i<n;i++){
        double x=amp*sin(6.283185307179586*f*i/48000.0);
        int32_t q=(int32_t)lrint(x*2147483647.0);
        uint32_t d=nora_pwm_audio_modulate_q31(q,&mod);
        shape_frame_3level(&s_shape[0], d, 0);
        for(int k=0;k<4;k++){ uint32_t w[4]={s_duty_l[k],s_3l_dc[k],s_3l_ta[k],s_3l_tb[k]}; fwrite(w,4,4,stdout); }
    }
    fprintf(stderr, "clips=%%u\n", (unsigned)s_clips);
    return 0;
}
'''


def grab(src, start, end_marker):
    a = src.index(start)
    b = src.index(end_marker, a) + len(end_marker)
    return src[a:b]


def build(tmp):
    src = (ROOT / 'src/app/board/audio/pwm_audio_board_ak.c').read_text().replace('\r\n', '\n')
    defs = '\n'.join(l for l in src.split('\n') if re.match(r'#define BOARD_3L_(TMIN|GUARD|SOFT)_Q4', l))
    state = grab(src, 'static float s_3l_dmax;', '\n') + grab(src, 'static int32_t s_3l_tn;', '\n')
    reset = '''static void shape_reset(void) {
    unsigned j;
''' + grab(src, '    {   /* tp + tn = 2 tmin', '\n    }\n') + '''    for (j = 0u; j < 2u * PWM_AUDIO_INTERP_TAPS; ++j) s_shape[0].hist[j] = 0.0f;
    s_shape[0].pos = 0u;
}
'''
    a = src.index('static void put_3level(')
    b = src.index('\n}\n#else\n', src.index('static void shape_frame_3level(')) + 3
    block = src[a:b]
    c = tmp / 'shape3.c'
    c.write_text(HARNESS % dict(per=PER, neutral=SPAN // 2, clk=int(CLK), defs=defs, state=state,
                                reset=reset, block=block, span=SPAN, halmin=HALMIN))
    exe = tmp / 'shape3.exe'
    hal = ROOT / 'src/app/hal_pwm_audio'
    r = subprocess.run([str(CC), '-std=c99', '-O2', '-ffp-contract=off', '-msse2', '-mfpmath=sse',
                        '-I', str(hal), '-I', str(ROOT / 'src/app/board/audio'), str(c),
                        str(hal / 'nora_pwm_audio_modulator.c'), '-lm', '-o', str(exe)],
                       capture_output=True, text=True,
                       env={**os.environ, 'PATH': str(CC.parent) + os.pathsep + os.environ['PATH']})
    if r.returncode:
        raise RuntimeError(r.stderr)
    return exe


def run(exe, f, amp, n):
    r = subprocess.run([str(exe), str(f), str(amp), str(n)], capture_output=True,
                       env={**os.environ, 'PATH': str(CC.parent) + os.pathsep + os.environ['PATH']})
    w = np.frombuffer(r.stdout, np.uint32).reshape(-1, 4).astype(np.int64)
    return w, int(r.stderr.decode().split('=')[1])


def check(w):
    """Decode, validate the schedule, return per-period edges in seconds."""
    phase, dc, ta, tb = w.T
    guard = round(CLK * 16 / 1e6)
    tmin = ((int(CLK) + 1999999) // 2000000) << 4
    assert (ta < tb).all() and (phase < ta).all() and (tb < dc).all() and (dc <= PER).all()
    gap = np.minimum(ta - phase, dc - tb)
    assert gap.min() >= guard, f'guard broken: {gap.min()} < {guard} Q4'
    # soft split: the weaker window shrinks towards tmin - a (250 ns), never below
    floor = tmin - round(CLK * 4 / 1e6)
    assert (tb - ta).min() >= floor and phase.min() * 2 >= floor and (PER - dc).min() * 2 >= floor
    # IN1 low around each boundary must be one window: previous tail + this head
    assert ((PER - dc[:-1]) == phase[1:]).all(), 'tn split mismatch across boundary'
    return gap.min() / 16 / CLK


def edges(w):
    phase, dc, ta, tb = w.T.astype(np.float64)
    T = PER / 16 / CLK
    q = 1 / 16 / CLK
    t0 = np.arange(len(phase)) * T
    # differential V = OUT1 - OUT2; idle brake = 0. IN2 low -> +V, IN1 low -> -V.
    ev = [(t0 + ta * q, 1, 1), (t0 + tb * q, 1, 0),           # node 1 = OUT1 high (+V)
          (t0 + dc * q, 2, 1), (t0 + T + phase[np.r_[1:len(phase), len(phase) - 1]] * q, 2, 0)]
    return T, len(phase), ev


if __name__ == '__main__':
    levels = [float(a) for a in sys.argv[1:]] or [-6.0, -20.0, -40.0, -60.0]
    tones = [997.0, 6900.0, 15000.0]
    freqs = m3.FREQS
    with tempfile.TemporaryDirectory() as td:
        exe = build(Path(td))
        print('firmware shape_frame_3level, ideal bridge; residual dBFS (worst of tones) / clips / min gap')
        row = []
        for lv in levels:
            worst = -999.0; clips = 0; gmin = 1.0
            for f in tones:
                w, c = run(exe, f, 10 ** (lv / 20), 4808)
                w = w[4 * 200:]          # FIR start-up
                gmin = min(gmin, check(w)); clips += c
                T, n, ev = edges(w)
                t, dv, _, _ = m3.bridge(ev, (0, 0), m3.IDEAL, 8.0, 47e-6)
                s = m3.spectrum(T, n, t, dv, 0)
                fund = s[np.abs(freqs - f) <= 40].max()
                mask = m3.INBAND & (np.abs(freqs - f) > 60)
                worst = max(worst, 10 * np.log10((s[mask] ** 2).sum() / fund ** 2 / 1.9638) + lv)
            row.append(f'{worst:7.1f} / {clips} / {gmin*1e6:.2f}us')
            print(f'{lv:6.0f} dBFS: ' + row[-1], flush=True)
