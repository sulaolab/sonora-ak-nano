"""Theme 3 P1: MPQ6612A 3-level (brake-zero) PWM vs the shipped 2-level centre+HREN.

Exact closed-form spectrum of a piecewise-constant bridge output. Each half-bridge
node (OUT1, OUT2) is simulated edge by edge, so propagation delays, side
mismatch and current-dependent dead time move real edges; the load current is
integrated exactly (R + L, exponential between edges). Spectrum: sum over edges
of dV * exp(-jwt) / (jw), with a per-period Blackman-Harris weight (same window
and metric as pwm_hren_sim.py, so the numbers are comparable).

Modulators (one carrier period T, sample time t0 = n*T):
  2L    IN2 = !IN1, centre pulse w = T(1 + m)/2, net mean = m.
  3L-A  IN1 = PG5, IN2 = PG6 in the SAME cycle; OUT1 high (+V) for tp centred in
        the cycle, OUT2 high (-V) split across the cycle ends -> the -V pulse at a
        boundary is (tn[n] + tn[n+1]) / 2 wide (2-tap average on the - path only).
  3L-B  PG5 shifted half a period: the -V pulse is contiguous, centred at the
        cycle end, still from the sample at t0 (path delay T vs T/2 for +V).
  3L-C  as B, but tn is computed from the signal at t0 + T/2, i.e. each pulse
        uses the signal at its own centre time (firmware: one extra
        interpolator phase, 48 -> 384 kHz polyphase, odd phase for tn).
  3L: tp = tmin + max(d, 0), tn = tmin + max(-d, 0), d = m*T; each window
      quantised to 2 Q4 counts (centre-aligned HREN step).
Levels are dBFS re the 2-level full scale (m_fs = 1 - 2*tmin/T), so both
schemes are compared at the same output amplitude.

Bridge non-idealities (per node k, k=1 OUT1 sources the load current i, k=2 sinks it):
  rise = LS->HS, fall = HS->LS; base delays rise_k / fall_k (t3/t4, <= 420 ns);
  dead time td: a rise is late by td when the node current flows OUT of the
  node (body diode of the LS holds it low), a fall is late when it flows IN.
  Hard sign switch = worst-case bound. 'qoss' > 0: a favourable current slews
  the node by itself in Qoss/|i| (soft model; near i = 0 both edges lose td).

usage: python pwm_3level_sim.py            all cases, 4 levels, 3 tones (pool)
       PWM3_CASES=0,1 python ...           subset
       python pwm_3level_sim.py ripple     idle ripple table only
"""
import math
import os
import sys
from multiprocessing import Pool

import numpy as np

from pwm_spur_sim import window, sine48

CLK = 798.72e6
Q4 = 1.0 / (16.0 * CLK)
TMIN = (((int(CLK) + 1999999) // 2000000) << 4) * Q4      # 500.8 ns (HAL MIN)
VIN = 12.0
DUR = 0.1
FREQS = np.arange(10.0, 24000.0, 5.0)
INBAND = (FREQS >= 20) & (FREQS <= 20000)


def period(fc):
    return {192: 66560, 96: 133120}[fc] * Q4


def modulate(kind, fc, tone, level, guard, interp=True, tmin=TMIN, soft=0.0):
    """Input (pre-bridge) node edges: list of arrays (t, node, rise)."""
    T = period(fc)
    n = int(DUR / T)
    t0 = np.arange(n) * T
    mfs = 1.0 - 2.0 * TMIN / T
    amp = 10 ** (level / 20) * mfs
    if kind == '2L':
        if interp:
            m = amp * np.sin(2 * np.pi * tone * t0)
        else:   # shipped ZOH x4 (pwm_hren_sim validation)
            x48 = sine48(tone, 1.0, DUR + 0.001)
            m = amp * np.repeat(x48, fc // 48)[:n]
        w = np.round(T * (1 + m) / 2 / (2 * Q4)) * 2 * Q4
        w = np.clip(w, TMIN, T - TMIN)
        a, b = t0 + (T - w) / 2, t0 + (T + w) / 2
        ev = [(a, 1, 1), (b, 1, 0), (a, 2, 0), (b, 2, 1)]
        init = (0, 1)
        clips = int(np.sum((w <= TMIN) | (w >= T - TMIN)))
    else:
        mp = amp * np.sin(2 * np.pi * tone * t0)
        mn = amp * np.sin(2 * np.pi * tone * (t0 + T / 2)) if kind == '3L-C' else mp
        q = lambda x: np.round(x / (2 * Q4)) * 2 * Q4
        if soft:   # smooth common part c(d) >= |d|/2: band-limits what max() makes
            cp = np.sqrt(soft ** 2 + (mp * T / 2) ** 2) - soft
            cn = np.sqrt(soft ** 2 + (mn * T / 2) ** 2) - soft
            tp = q(tmin + cp + mp * T / 2)
            tn = q(tmin + cn - mn * T / 2)
        else:
            tp = q(tmin + np.maximum(mp * T, 0))
            tn = q(tmin + np.maximum(-mn * T, 0))
        lim = T - 2 * guard
        over = tp + tn > lim
        clips = int(over.sum())
        if clips:   # keep the larger pulse, shrink to the guard
            tp = np.where(over & (tp > tn), lim - tn, tp)
            tn = np.where(over & (tn >= tp), lim - tp, tn)
        c = t0 + T / 2
        ev = [(c - tp / 2, 1, 1), (c + tp / 2, 1, 0)]
        e = t0 + T
        if kind == '3L-A':
            nxt = np.append(tn[1:], tn[-1])
            ev += [(e - tn / 2, 2, 1), (e + nxt / 2, 2, 0)]
        else:
            ev += [(e - tn / 2, 2, 1), (e + tn / 2, 2, 0)]
        init = (0, 0)
    return T, n, ev, init, clips


def bridge(ev, init, br, R, L):
    """Apply delays / dead time edge by edge with the exact R-L current."""
    t = np.concatenate([e[0] for e in ev])
    node = np.concatenate([np.full(len(e[0]), e[1]) for e in ev])
    rise = np.concatenate([np.full(len(e[0]), e[2]) for e in ev])
    base = np.where(rise == 1,
                    np.where(node == 1, br['rise1'], br['rise2']),
                    np.where(node == 1, br['fall1'], br['fall2']))
    tn = t + base
    order = np.argsort(tn, kind='stable')
    td = br['td']
    tau = L / R
    st = [None, init[0], init[1]]
    v = st[1] - st[2]
    cur, tc = 0.0, 0.0
    out_t = np.empty(len(order)); out_dv = np.empty(len(order)); out_i = np.empty(len(order))
    rms_acc = 0.0; rms_t = 0.0; t_half = DUR / 2
    for j, k in enumerate(order):
        tk = tn[k]
        nd = node[k]
        r = rise[k]
        if td:
            iinf = v * VIN / R
            ia = iinf + (cur - iinf) * math.exp(-max(tk - tc, 0.0) / tau)
            ik = ia if nd == 1 else -ia
            late = (r == 1 and ik > 0) or (r == 0 and ik < 0)
            if late or not br.get('qoss'):
                ta = tk + (td if late else 0.0)
            else:   # favourable current slews the node within td: Qoss/|i|
                ta = tk + max(0.0, td - br['qoss'] / max(abs(ik), 1e-9))
        else:
            ta = tk
        ta = max(ta, tc)
        dt = ta - tc
        iinf = v * VIN / R
        c0 = cur - iinf
        ex = math.exp(-dt / tau)
        if tc >= t_half:   # exact integral of i^2 over the segment
            rms_acc += (iinf * iinf * dt + 2 * iinf * c0 * tau * (1 - ex)
                        + c0 * c0 * tau / 2 * (1 - ex * ex))
            rms_t += dt
        cur = iinf + c0 * ex
        tc = ta
        st[nd] = r
        nv = st[1] - st[2]
        out_t[j] = ta; out_dv[j] = nv - v; out_i[j] = cur
        v = nv
    return out_t, out_dv, out_i, (rms_acc / rms_t if rms_t else 0.0)


def spectrum(T, n, t, dv, v0):
    """Per-period-windowed transform of the piecewise-constant V(t)."""
    w = window(n)
    p = np.minimum((t / T).astype(int), n - 1)
    keep = (t >= 0) & (t < n * T)
    t, dv, p = t[keep], dv[keep], p[keep]
    # V at each boundary k*T (k = 1..n-1), amplitude V*(w[k] - w[k-1])
    vcum = v0 + np.cumsum(dv)
    kb = np.arange(1, n)
    idx = np.searchsorted(t, kb * T) - 1
    vb = np.where(idx >= 0, vcum[np.maximum(idx, 0)], v0)
    tt = np.concatenate([t, kb * T])
    aa = np.concatenate([dv * w[p], vb * (w[kb] - w[kb - 1])])
    out = np.empty(len(FREQS))
    for i in range(0, len(FREQS), 16):
        om = 2 * np.pi * FREQS[i:i + 16, None]
        out[i:i + 16] = np.abs((aa * np.exp(-1j * om * tt)).sum(1)) / om[:, 0]
    return out


IDEAL = dict(rise1=0.0, fall1=0.0, rise2=0.0, fall2=0.0, td=0.0)
DLY = dict(rise1=420e-9, fall1=300e-9, rise2=420e-9, fall2=300e-9, td=0.0)
DLY_POS = dict(DLY, rise2=470e-9, fall2=350e-9)     # side 2 +50 ns both edges
DLY_WID = dict(DLY, fall2=350e-9)                   # side 2 pulse +50 ns wider
TD50 = dict(DLY, td=50e-9)
TD100 = dict(DLY, td=100e-9)
TD50S = dict(DLY, td=50e-9, qoss=5e-9)
LOAD = (8.0, 47e-6)

# name, kind, fc, bridge, load, extra
CASES = [
    ('2L ZOH x4 (PR#12 check)',            '2L',   192, IDEAL, LOAD, dict(interp=False)),
    ('2L ideal interp',                    '2L',   192, IDEAL, LOAD, {}),
    ('3L-A same cycle, - split',           '3L-A', 192, IDEAL, LOAD, {}),
    ('3L-B PG5 +T/2, same sample',         '3L-B', 192, IDEAL, LOAD, {}),
    ('3L-C PG5 +T/2, own-centre sample',   '3L-C', 192, IDEAL, LOAD, {}),
    ('2L delays 420/300',                  '2L',   192, DLY,   LOAD, {}),
    ('3L-C delays 420/300',                '3L-C', 192, DLY,   LOAD, {}),
    ('3L-C + side2 shift 50ns',            '3L-C', 192, DLY_POS, LOAD, {}),
    ('3L-C + side2 width +50ns',           '3L-C', 192, DLY_WID, LOAD, {}),
    ('2L dead time 50ns, 8R/47uH',         '2L',   192, TD50,  LOAD, {}),
    ('3L-C dead time 50ns, 8R/47uH',       '3L-C', 192, TD50,  LOAD, {}),
    ('2L dead time 100ns, 8R/47uH',        '2L',   192, TD100, LOAD, {}),
    ('3L-C dead time 100ns, 8R/47uH',      '3L-C', 192, TD100, LOAD, {}),
    ('2L dead time 50ns soft Qoss 5nC',    '2L',   192, TD50S, LOAD, {}),
    ('3L-C dead time 50ns soft Qoss 5nC',  '3L-C', 192, TD50S, LOAD, {}),
    ('3L-C tmin 1.0us, dead 50ns soft',    '3L-C', 192, TD50S, LOAD, dict(tmin=1.0e-6)),
    ('3L-C tmin 1.0us, dead 50ns hard',    '3L-C', 192, TD50,  LOAD, dict(tmin=1.0e-6)),
    ('3L-C soft split a=250ns',            '3L-C', 192, IDEAL, LOAD, dict(soft=250e-9)),
    ('3L-C soft split a=1us',              '3L-C', 192, IDEAL, LOAD, dict(soft=1e-6)),
    ('3L-C linear split (c = |m|max/2)',   '3L-C', 192, IDEAL, LOAD, dict(soft=1e3)),
    ('3L-S (soft 250ns) delays 420/300',   '3L-C', 192, DLY,   LOAD, dict(soft=250e-9)),
    ('3L-S + side2 shift 50ns',            '3L-C', 192, DLY_POS, LOAD, dict(soft=250e-9)),
    ('3L-S dead time 50ns hard',           '3L-C', 192, TD50,  LOAD, dict(soft=250e-9)),
    ('3L-S dead time 50ns soft Qoss 5nC',  '3L-C', 192, TD50S, LOAD, dict(soft=250e-9)),
    ('3L-S dead 50ns soft, 8R/22uH',       '3L-C', 192, TD50S, (8.0, 22e-6), dict(soft=250e-9)),
    ('2L dead 50ns soft, 8R/22uH',         '2L',   192, TD50S, (8.0, 22e-6), {}),
    ('3L-S 96k ideal',                     '3L-C', 96,  IDEAL, LOAD, dict(soft=250e-9)),
    ('2L 96k ideal interp',                '2L',   96,  IDEAL, LOAD, {}),
    ('3L-C 96k ideal',                     '3L-C', 96,  IDEAL, LOAD, {}),
    ('3L-C 96k dead time 50ns',            '3L-C', 96,  TD50,  LOAD, {}),
    ('2L 96k dead time 50ns',              '2L',   96,  TD50,  LOAD, {}),
]
LEVELS = [-6.0, -20.0, -40.0, -60.0]
TONES = [997.0, 6900.0, 15000.0]
GUARD = 1e-6


def one(job):
    ci, lv, f = job
    name, kind, fc, br, (R, L), extra = CASES[ci]
    T, n, ev, init, clips = modulate(kind, fc, f, lv, GUARD, **extra)
    t, dv, _, _ = bridge(ev, init, br, R, L)
    s = spectrum(T, n, t, dv, init[0] - init[1])
    fund = s[np.abs(FREQS - f) <= 40].max()
    mask = INBAND & (np.abs(FREQS - f) > 60)
    res = 10 * np.log10((s[mask] ** 2).sum() / fund ** 2 / 1.9638) + lv
    hp = 0.0
    for h in range(2, 10):
        if h * f > 20000:
            break
        hp += s[np.abs(FREQS - h * f) <= 10].max() ** 2
    thd = 10 * np.log10(hp / fund ** 2 + 1e-30) if hp else float('nan')
    return ci, lv, f, res, thd, clips


def ripple():
    print(f'idle ripple, Vin = {VIN} V, tmin = {TMIN*1e9:.0f} ns, ideal bridge')
    print('fc    load          2L i_pp   2L i_rms  2L I2R   | 3L i_pp   3L i_rms  3L I2R   | rms ratio')
    for fc in (192, 96):
        for R, L in ((4.0, 22e-6), (4.0, 100e-6), (8.0, 22e-6), (8.0, 47e-6), (8.0, 100e-6)):
            row = []
            for kind in ('2L', '3L-C'):
                T, n, ev, init, _ = modulate(kind, fc, 1000.0, -300.0, GUARD)
                t, dv, i, ms = bridge(ev, init, IDEAL, R, L)
                sel = t > DUR / 2
                row.append((i[sel].max() - i[sel].min(), math.sqrt(ms), ms * R))
            (a, b, c), (d, e, g) = row
            print(f'{fc:3d}k  {R:.0f}R {L*1e6:5.0f}uH   {a*1e3:6.0f}mA  {b*1e3:6.0f}mA  {c*1e3:6.1f}mW'
                  f' | {d*1e3:6.0f}mA  {e*1e3:6.0f}mA  {g*1e3:6.1f}mW | {e/b:.3f}')
    for fc in (192, 96):
        T = period(fc)
        print(f'{fc}k: T = {T*1e6:.3f} us; max |m| 2L = {1-2*TMIN/T:.3f}, '
              f'3L (guard {GUARD*1e6:.1f} us/gap) = {(T-2*GUARD-2*TMIN)/T:.3f}, '
              f'3L guard 0.5 us = {(T-1e-6-2*TMIN)/T:.3f}')


if __name__ == '__main__':
    if sys.argv[1:] == ['ripple']:
        ripple()
        sys.exit(0)
    pick = os.environ.get('PWM3_CASES')
    cis = [int(c) for c in pick.split(',')] if pick else range(len(CASES))
    jobs = [(c, lv, f) for c in cis for lv in LEVELS for f in TONES]
    with Pool(int(os.environ.get('PWM3_PROCS', '10'))) as pool:
        res = pool.map(one, jobs, chunksize=1)
    tab = {}
    for ci, lv, f, r, thd, clips in res:
        tab[(ci, lv, f)] = (r, thd, clips)
    print('residual = in-band power except tone +/-60 Hz, dBFS re 2L full scale, worst of '
          + '/'.join(f'{t:.0f}' for t in TONES) + ' Hz;  THD = H2..H9 re fundamental at 997 Hz')
    print('case'.ljust(36) + ''.join(f'{l:>7.0f}' for l in LEVELS) + '  |'
          + ''.join(f' THD{l:>4.0f}' for l in LEVELS) + '  clips')
    for ci in cis:
        r = [max(tab[(ci, lv, f)][0] for f in TONES) for lv in LEVELS]
        h = [tab[(ci, lv, 997.0)][1] for lv in LEVELS]
        cl = sum(tab[(ci, lv, f)][2] for lv in LEVELS for f in TONES)
        print(CASES[ci][0].ljust(36) + ''.join(f'{v:7.1f}' for v in r) + '  |'
              + ''.join(f'{v:8.1f}' for v in h) + f'  {cl}', flush=True)
