"""Theme 4: does PWM high-resolution (HREN) mode buy audio quality on AK?

Same exact closed-form pulse spectrum as pwm_spur_sim.py, shipped ZOH x4 at a
192 kHz carrier, and duty quantised the way the PG hardware does it:
  standard resolution: PGxDC bits[3:0] ignored (floor to 16 Q4 = 1 clock)
  HREN:                every Q4 count is a real 78 ps edge step
Edge-aligned pulse = DC, centre-aligned pulse = 2 * DC (so centre without
HREN has half the steps of edge). Prints in-band (20 Hz..20 kHz) residual
power excluding +/-60 Hz around the tone, relative to full scale (dBFS),
for several levels. Analog limits (supply ripple, clock jitter, bridge
nonlinearity) are not modelled.

usage: python pwm_hren_sim.py [levels_dBFS ...]
"""
import sys
import numpy as np
from pwm_spur_sim import spectrum, sine48

CLK = 798.72e6
PER = 66560                                   # Q4 counts per 192 kHz period
MIN = ((int(CLK) + 1999999) // 2000000) << 4  # 500 ns floor, Q4


def chain(x48, centre, hren, ns2=False, rng=None):
    span = PER // 2 if centre else PER        # DC register span
    mn = MIN // 2 if centre else MIN
    xs = np.repeat(x48, 4)
    d = span / 2 + xs * (span / 2 - mn)       # ideal DC, Q4
    step = 1.0 if hren else 16.0
    if ns2:                                   # TPDF + 2nd-order error feedback at 192 kHz
        out = np.empty_like(d); e1 = e2 = 0.0
        tp = rng.random(len(d)) - rng.random(len(d))
        for i, v in enumerate(d / step):
            u = v - (2 * e1 - e2)
            q = np.round(u + tp[i]); e2, e1 = e1, q - u; out[i] = q
        d = out * step
    else:
        d = np.round(d)                       # modulator rounds to Q4
        if not hren:
            d = np.floor(d / 16.0) * 16.0     # HW drops bits[3:0]
    d = np.clip(d, mn, span - mn)
    T = PER / 16.0 / CLK
    pulse = d * (2 if centre else 1) / 16.0 / CLK
    return T, pulse


CASES = [
    ('edge std   (4160 steps, shipped before PR#12)', dict(centre=False, hren=False)),
    ('edge HREN  (66560 steps)', dict(centre=False, hren=True)),
    ('centre std (2080 steps)', dict(centre=True, hren=False)),
    ('centre HREN (33280 steps, PR#12 default)', dict(centre=True, hren=True)),
    ('centre std + TPDF/NS2', dict(centre=True, hren=False, ns2=True)),
    ('centre HREN + TPDF/NS2', dict(centre=True, hren=True, ns2=True)),
]

if __name__ == '__main__':
    # PWM_HREN_CASES=2,3 runs a subset (each case takes minutes; run in parallel).
    import os
    pick = os.environ.get('PWM_HREN_CASES')
    if pick:
        CASES = [CASES[int(i)] for i in pick.split(',')]
    levels = [float(a) for a in sys.argv[1:]] or [-6.0, -20.0, -40.0, -60.0]
    tones = [997.0, 6900.0, 15000.0]
    freqs = np.arange(10.0, 24000.0, 5.0)
    inband = (freqs >= 20) & (freqs <= 20000)
    print('residual = in-band power except tone +/-60 Hz, dB re full scale (BH4 ENBW corrected)')
    print('case'.ljust(48) + ''.join(f'{l:>6.0f}dB' for l in levels) + '   (worst of tones)')
    for name, kw in CASES:
        row = []
        for lv in levels:
            amp = 10 ** (lv / 20)
            worst = -999.0
            for f in tones:
                rng = np.random.default_rng(1)
                T, p = chain(sine48(f, amp, 0.1), rng=rng, **kw)
                s = spectrum(np.arange(len(p)) * T, T, p, freqs, kw['centre'])
                fund = s[np.abs(freqs - f) <= 40].max()
                mask = inband & (np.abs(freqs - f) > 60)
                rel = 10 * np.log10((s[mask] ** 2).sum() / fund ** 2 / 1.9638)
                worst = max(worst, rel + lv)
            row.append(worst)
        print(name.ljust(48) + ''.join(f'{v:8.1f}' for v in row), flush=True)
