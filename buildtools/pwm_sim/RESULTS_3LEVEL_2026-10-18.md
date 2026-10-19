# Theme 3 P1: MPQ6612A 3-level (brake-zero) PWM -- host sim 2026-10-18

Model: `pwm_3level_sim.py`. Each bridge node (OUT1/OUT2) edge by edge: per-edge delay
(t3/t4), side mismatch, current-dependent dead time with the exact R-L load current;
exact closed-form spectrum of V(t), per-period BH4 window (same metric as
`RESULTS_HREN_2026-10-17.md`). 192 kHz, Q4 HREN step (2 Q4 per window), tmin 500.8 ns,
guard 1 us, load 8 ohm / 47 uH, Vin 12 V. Ideal sine at the carrier rate (= ideal x4
interp), except row 0. Residual: in-band 20 Hz..20 kHz except tone +/-60 Hz, dBFS re
2-level full scale, worst of 997 / 6900 / 15000 Hz.

Validation: row 0 (2L ZOH x4) at -20 dBFS / 997 Hz = -82.4 dBFS, same as the PR#12 sim.

## Modulator variants (ideal bridge)
| case | -6 | -20 | -40 | -60 dBFS |
|---|---|---|---|---|
| 2L ZOH x4 (PR#12) | -56.7 | -79.3 | -82.1 | -80.6 |
| 2L ideal interp (reference) | -60.7 | -82.1 | -83.1 | -83.1 |
| 3L-A PG5/PG6 same cycle, -V pulse split over the boundary | -57.8 | -65.1 | -81.9 | -85.8 |
| 3L-B PG5 shifted T/2, both from one sample | -29.3 | -43.3 | -63.3 | -81.4 |
| 3L-C PG5 shifted T/2, each pulse from its own centre time | -41.7 | -55.7 | -75.4 | -85.4 |
| **3L-S = 3L-C + smooth split (a = 250 ns)** | **-59.7** | **-85.5** | **-85.6** | **-85.3** |
| 3L-C smooth split a = 1 us | -4.2 (headroom) | -85.5 | | |

- Each pulse must use the signal at its own centre time (3L-B -> 3L-C). Firmware: tn from the
  odd phase of a 48 -> 384 kHz polyphase interpolator.
- The hard split `tp = tmin + max(d,0)` has a kink at d = 0, so each pulse carries |d|/2
  harmonics. Because +V and -V are sampled at different times, these do not cancel and they
  fold into the band (6.9/15 kHz worst). Smooth split
  `c = sqrt(a^2 + (d/2)^2) - a, tp = tmin + c + d/2, tn = tmin + c - d/2` fixes it
  (a = 250 ns). A larger a eats headroom.

## Bridge non-idealities (3L-S unless noted)
| case | -6 | -20 | -40 | -60 dBFS | THD@997 -20 |
|---|---|---|---|---|---|
| 2L delays 420/300 ns | -60.7 | -81.9 | -82.9 | -82.8 | -95.7 |
| 3L-S delays 420/300 ns | -59.7 | -94.6 | -95.3 | -93.4 | -95.9 |
| 3L-S + side2 shift +50 ns | -59.7 | -84.6 | -95.4 | -93.5 | -84.0 |
| 3L-C (hard split) + side2 width +50 ns | -40.9 | -48.1 | -48.9 | -48.9 | -81.6 |
| 2L dead time 50 ns hard | -38.7 | -81.9 | -82.9 | -82.8 | -95.7 |
| 3L-S dead time 50 ns hard | -35.0 | -39.2 | -95.3 | -93.4 | -22.6 |
| 3L-S dead time 50 ns soft (Qoss 5 nC) | -38.8 | -39.7 | -95.3 | -93.4 | -23.0 |
| 2L dead 50 ns soft, 8R/22uH | -37.4 | -81.9 | -82.9 | -82.9 | -76.2 |
| 3L-S dead 50 ns soft, 8R/22uH | -42.1 | -40.5 | -94.0 | -93.7 | -23.8 |

- Common delays cancel as expected. With delays, 3L-S comes out 9 dB *better* than ideal
  (-94.6 vs -85.5). This is not explained yet. Do not count it as a gain; take -85.5 as
  the 3L floor.
- A side width mismatch (+50 ns on one side only) puts a constant DC/even error on every
  period: -49 dBFS flat. A side *shift* is harmless (-84.6). On HW, tmin per side must be
  trimmed if the widths differ.
- **Dead time is the decisive risk.** Where the load current changes sign within each
  period (3L at -20 dBFS and above: the ripple is smaller than the audio current only
  intermittently), a 50 ns dead time gives -40 dBFS / THD -23 dBc. 2L keeps -82 at -20
  (the ripple current dominates, so the sign is fixed per edge) and fails only at -6
  (-37..-39, both). At -40/-60, 3L-S stays -93..-95 under dead time.
  The MPQ6612A datasheet gives no dead time figure; whether its internal shoot-through
  protection behaves like this model must be measured (P2/P3).
- tmin 1 us does not fit at 192 kHz (-6 dBFS clips: tp+tn > T-2*guard).

## Headroom and 96 kHz
- 192 kHz max |m|: 2L 0.808; 3L 0.424 with 1 us guards (-5.6 dB), 0.616 with 0.5 us.
- 3L-S 96 kHz ideal: -33.8 / -63.4 / -86.7 / -86.6. Worse than 192 k at -6/-20: the
  sampling-time offset per pulse (T/2) grows with T. 96 kHz is not a free fallback.

## Idle ripple (ideal bridge, Vin 12 V, `python pwm_3level_sim.py ripple`)
| fc | load | 2L i_pp / rms / I2R | 3L i_pp / rms / I2R |
|---|---|---|---|
| 192k | 8R 47uH | 654 mA / 190 mA / 289 mW | 149 mA / 59 mA / 28 mW |
| 192k | 4R 22uH | 1394 / 406 / 658 | 322 / 126 / 64 |
| 96k | 8R 47uH | 1249 / 370 / 1093 | 174 / 60 / 29 |
rms ratio 0.31 at 192 k, 0.16 at 96 k (independent of the load); I2R 1/10 and 1/40.

## Verdict
- Ideal: 3L-S matches 2L at -6 dBFS and is a little better at -20..-60 (-85.5 vs -83).
  The audible gain is idle current / heat (1/10), not in-band residual.
- Adopt only if HW shows the MPQ6612A dead time / zero-current edge is small (< ~10 ns
  equivalent). Otherwise 3L is worse than 2L at -20 dBFS.
- Also needed: smooth split + per-pulse sampling (384 kHz polyphase), PG5 shifted T/2,
  guard 1 us, headroom -5.6 dB.
