"""Exact-spectrum host model of the AK Nano / CK PWM audio chain.

The bipolar BTL output is +1 for the duty, -1 for the rest of each carrier
period (edge-aligned). Its Fourier transform is summed pulse by pulse in
closed form, so no time-grid aliasing enters the result. Only 0..24 kHz bins.
"""
import sys
import numpy as np

FS = 48000.0

def window(n):
    k = np.arange(n) / n
    return (0.35875 - 0.48829*np.cos(2*np.pi*k) + 0.14128*np.cos(4*np.pi*k)
            - 0.01168*np.cos(6*np.pi*k))

def spectrum(t0, period, duty_s, freqs, centred=False):
    """t0: pulse start times, duty_s: high time (s), period: carrier T (s)."""
    w = window(len(t0))
    out = np.zeros(len(freqs), complex)
    for i in range(0, len(freqs), 64):
        om = 2*np.pi*freqs[i:i+64, None]
        ea = np.exp(-1j*om*t0)
        if centred:
            a = (period - duty_s)/2
            y = ea*(-(1 - np.exp(-1j*om*period)) + 2*(np.exp(-1j*om*a) - np.exp(-1j*om*(a + duty_s))))
        else:
            y = ea*(1 - 2*np.exp(-1j*om*duty_s) + np.exp(-1j*om*period))
        out[i:i+64] = (y*w).sum(1) / (1j*om[:, 0])
    return np.abs(out)

def ak_chain(x48, factor=4, quant=True, interp=False, dither_ns=0, rng=None):
    clk = 798.72e6
    per = 66560 // factor * 4 // 4   # Q4 at 192k: 66560
    per = {1: 266240, 2: 133120, 4: 66560}[factor]
    mn = ((int(clk) + 1999999)//2000000) << 4
    centre, amp = per/2, per/2 - mn
    if interp:   # ideal band-limited x4: recompute the sine at the carrier rate
        xs = x48  # caller passes the carrier-rate signal directly
    else:
        xs = np.repeat(x48, factor)
    d = centre + xs*amp          # Q4 fractional
    if quant:
        if dither_ns:
            d = noise_shape(d/16.0, dither_ns, rng)*16.0
        else:
            d = np.floor(np.round(d)/16.0)*16.0   # modulator rounds Q4, HREN=0 drops 4 bits
    d = np.clip(d, mn, per - mn)
    T = per/16.0/clk
    return T, d/16.0/clk

def noise_shape(d, order, rng):
    out = np.empty_like(d); e1 = e2 = 0.0
    tp = rng.random(len(d)) - rng.random(len(d))
    for i, v in enumerate(d):
        u = v - (2*e1 - e2 if order == 2 else e1)
        q = np.round(u + tp[i])
        e2, e1 = e1, q - u
        out[i] = q
    return out

def ck_chain(x48, interp=True):
    fosc = 200e6; per = 1042; mn = 100; factor = 4
    centre, amp = per/2, per/2 - mn
    dq = np.round((centre + x48*amp)*16)            # core Q4
    if interp:   # elastic FIFO: output samples at fosc/per/4, linear in duty
        step = FS / (fosc/per/factor)
        pos = np.arange(0, len(dq) - 2, step)
        i = pos.astype(int); ph = np.floor((pos - i)*65536)
        v = dq[i] + np.floor((dq[i+1] - dq[i])*ph/65536)
    else:
        v = dq
    duty = np.clip(np.floor((v + 8)/16), mn, per - mn)
    return per/fosc, np.repeat(duty, factor)/fosc

def run(name, make, tones, level, dur=0.1, centred=False):
    freqs = np.arange(10.0, 24000.0, 5.0)
    inband = (freqs >= 20) & (freqs <= 20000)
    rows = []
    for f in tones:
        T, dsec = make(f, level, dur)
        t0 = np.arange(len(dsec))*T
        s = spectrum(t0, T, dsec, freqs, centred)
        fund = s[np.abs(freqs - f) <= 40].max()
        mask = inband & (np.abs(freqs - f) > 60)
        sd = 20*np.log10(s[mask]/fund + 1e-30); fr = freqs[mask]
        # local peaks only, top 3
        pk = [j for j in range(1, len(sd)-1) if sd[j] >= sd[j-1] and sd[j] >= sd[j+1]]
        pk = sorted(pk, key=lambda j: -sd[j])[:3]
        tot = 10*np.log10((s[mask]**2).sum()/fund**2/ (1.9638))  # BH4 ENBW in bins
        rows.append((f, tot, [(fr[j], sd[j]) for j in pk]))
    print(f'## {name}  level={20*np.log10(level):.0f} dBFS')
    print('tone_Hz  spur_total_dBc  top spurs (Hz : dBc : origin)')
    for f, tot, pk in rows:
        print(f'{f:7.0f}  {tot:7.1f}   ' + '  '.join(f'{p:7.0f}:{v:6.1f}:{label(f, p)}' for p, v in pk))
    print()

def label(f, p):
    best = ''
    for m in range(-4, 5):
        for k in range(-7, 8):
            if k == 0 and m == 0: continue
            if abs(abs(k*f + m*FS) - p) <= 12:
                c = f"{k}f{'+' if m >= 0 else '-'}{abs(m)}*48k" if m else f'{k}f'
                if not best or len(c) < len(best): best = c
    return best or '?'

def sine48(f, level, dur, fs=FS):
    n = int(dur*fs) + 8
    return level*np.sin(2*np.pi*f*np.arange(n)/fs)

if __name__ == '__main__':
    level = 10**(float(sys.argv[1])/20) if len(sys.argv) > 1 else 0.5
    tones = [1000, 3000, 5000, 6900, 8000, 9600, 11000, 12000, 13000, 14000,
             15000, 16000, 17000, 18000, 19000, 20000]
    rng = np.random.default_rng(1)
    which = sys.argv[2:] or ['A0', 'A1', 'A2', 'A3', 'C0', 'C1']
    S = {
     'A0': ('AK now: ZOH x4, 4160 counts', lambda f, l, d: ak_chain(sine48(f, l, d))),
     'A1': ('AK ZOH x4, float duty (no quantization)', lambda f, l, d: ak_chain(sine48(f, l, d), quant=False)),
     'A2': ('AK ideal x4 interpolation, float duty', lambda f, l, d: ak_chain(sine48(f, l, d, 4*FS), quant=False, interp=True)),
     'A3': ('AK ideal x4 + TPDF + NS2 at 192k', lambda f, l, d: ak_chain(sine48(f, l, d, 4*FS), interp=True, dither_ns=2, rng=rng)),
     'C0': ('CK now: FRC elastic linear interp, 1042', lambda f, l, d: ck_chain(sine48(f, l, d))),
     'C1': ('CK no elastic interp (sync clock), 1042', lambda f, l, d: ck_chain(sine48(f, l, d), interp=False)),
    }
    S['A4'] = ('AK ZOH x4, float duty, CENTRE-aligned', S['A1'][1], True)
    S['A5'] = ('AK ideal x4, float duty, CENTRE-aligned', S['A2'][1], True)
    S['A6'] = ('AK ideal x4 + TPDF + NS2, CENTRE-aligned', S['A3'][1], True)
    for k in which:
        run(S[k][0], S[k][1], tones, level, centred=len(S[k]) > 2)
