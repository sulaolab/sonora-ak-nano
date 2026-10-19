"""Spectrum of the REAL board shaping code (shape_frame() in pwm_audio_board_ak.c).

Extracts the INTERP/NS2 block from the firmware source verbatim, compiles it
on the host with float32 semantics (gcc -ffp-contract=off, no x87), feeds a
48 kHz sine through the real HAL modulator (nora_pwm_audio_modulate_q31) and
writes the 192 kHz PGxDC words, then runs the exact closed-form pulse
spectrum from pwm_spur_sim.py on them.

Rows: ZOH = old path (4 x repeat) at centre+HREN, INTERP, INTERP+NS2.
usage: python pwm_shape_c_sim.py [levels_dBFS ...]
"""
import os, re, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
from pwm_spur_sim import spectrum

ROOT = Path(__file__).resolve().parents[2]
CC = Path(r'C:\msys64\usr\bin\gcc.exe')
CLK, PER = 798.72e6, 66560
SPAN, MIN = PER // 2, (((int(CLK) + 1999999) // 2000000) << 4) // 2

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "nora_pwm_audio_modulator.h"
#define BOARD_CHANNELS 1u
#define BOARD_NEUTRAL_Q4 %(neutral)du
#define BOARD_DC_SPAN %(span)du
#define BOARD_MIN_DUTY_Q4 %(min2)du
#define BOARD_DC_DIV 2u
#define PWM_AUDIO_AK_HREN %(hren)d
#define PWM_AUDIO_AK_NS2 %(ns2)d
%(block)s
int main(int argc, char **argv){
    double f=atof(argv[1]), amp=atof(argv[2]); int n=atoi(argv[3]), zoh=atoi(argv[4]);
    nora_pwm_audio_modulator_t mod; volatile uint32_t out[4];
    nora_pwm_audio_modulator_setup(&mod, BOARD_DC_SPAN, BOARD_MIN_DUTY_Q4/BOARD_DC_DIV);
    shape_reset();
    for(int i=0;i<n;i++){
        double x=amp*sin(6.283185307179586*f*i/48000.0);
        int32_t q=(int32_t)lrint(x*2147483647.0);
        uint32_t d=nora_pwm_audio_modulate_q31(q,&mod);
        if(zoh){ for(int k=0;k<4;k++) out[k]=d; } else shape_frame(&s_shape[0], d, out);
        fwrite((const void*)out,4,4,stdout);
    }
    return 0;
}
'''


def build(tmp, hren, ns2):
    src = (ROOT / 'src/app/board/audio/pwm_audio_board_ak.c').read_text()
    a = src.index('#include "board/audio/pwm_audio_interp_coeffs.h"\n')
    b = src.index('\n}\n#endif', src.index('static void shape_frame(')) + 2
    block = src[a:b].replace('board/audio/', '')
    # The block starts inside the 3LEVEL #else and ends inside a second one;
    # build the 2-level path with 3LEVEL off and close what the cut left open.
    block = block.replace('#endif\n#define BOARD_DC_MIN', '#define BOARD_DC_MIN', 1)
    block = '#define PWM_AUDIO_AK_3LEVEL 0\n' + block + '\n#endif\n'
    c = tmp / f'shape_{hren}{ns2}.c'
    c.write_text(HARNESS % dict(neutral=SPAN // 2, span=SPAN, min2=MIN * 2, hren=hren, ns2=ns2, block=block))
    exe = tmp / f'shape_{hren}{ns2}.exe'
    hal = ROOT / 'src/app/hal_pwm_audio'
    r = subprocess.run([str(CC), '-std=c99', '-O2', '-ffp-contract=off', '-msse2', '-mfpmath=sse',
                        '-I', str(hal), '-I', str(ROOT / 'src/app/board/audio'), str(c),
                        str(hal / 'nora_pwm_audio_modulator.c'), '-lm', '-o', str(exe)],
                       capture_output=True, text=True,
                       env={**os.environ, 'PATH': str(CC.parent) + os.pathsep + os.environ['PATH']})
    if r.returncode:
        raise RuntimeError(r.stderr)
    return exe


def measure(exe, f, amp, zoh, n=4808):
    raw = subprocess.run([str(exe), str(f), str(amp), str(n), str(zoh)], capture_output=True,
                         env={**os.environ, 'PATH': str(CC.parent) + os.pathsep + os.environ['PATH']}).stdout
    dc = np.frombuffer(raw, np.uint32).astype(np.float64)[4 * 200:]   # skip FIR start-up
    T = PER / 16.0 / CLK
    pulse = dc * 2 / 16.0 / CLK
    freqs = np.arange(10.0, 24000.0, 5.0)
    s = spectrum(np.arange(len(pulse)) * T, T, pulse, freqs, True)
    fund = s[np.abs(freqs - f) <= 40].max()
    mask = (freqs >= 20) & (freqs <= 20000) & (np.abs(freqs - f) > 60)
    return 10 * np.log10((s[mask] ** 2).sum() / fund ** 2 / 1.9638)


if __name__ == '__main__':
    levels = [float(a) for a in sys.argv[1:]] or [-6.0, -20.0, -40.0, -60.0]
    tones = [997.0, 6900.0, 15000.0, 19000.0]
    rows = [('ZOH (PR #12)', 1, 0, 1), ('INTERP x4 FIR', 1, 0, 0), ('INTERP + TPDF/NS2', 1, 1, 0),
            ('INTERP + NS2, HREN off', 0, 1, 0)]
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        print('centre-aligned, 192 kHz; residual dBFS, worst of', tones)
        print('case'.ljust(28) + ''.join(f'{l:>8.0f}' for l in levels))
        pick = os.environ.get('PWM_SHAPE_ROWS')
        for name, hren, ns2, zoh in ([rows[int(i)] for i in pick.split(',')] if pick else rows):
            exe = build(tmp, hren, ns2)
            vals = []
            for lv in levels:
                vals.append(max(measure(exe, f, 10 ** (lv / 20), zoh) + lv for f in tones))
            print(name.ljust(28) + ''.join(f'{v:8.1f}' for v in vals), flush=True)
