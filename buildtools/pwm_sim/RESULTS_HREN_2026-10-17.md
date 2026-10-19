# Theme 4: HREN (high-resolution PWM) quality gain -- host sim 2026-10-17

Model: `pwm_hren_sim.py` (exact closed-form pulse spectrum, ZOH x4, 192 kHz carrier, 798.72 MHz PG clock).
Metric: in-band (20 Hz..20 kHz) residual except tone +/-60 Hz, dBFS, worst of 997 / 6900 / 15000 Hz. Analog limits (supply, jitter, bridge) not modelled.

| case | -6 dBFS | -20 | -40 | -60 |
|---|---|---|---|---|
| edge std (4160 steps) | -34.1 | -61.2 | -70.0 | -70.6 |
| edge HREN (66560) | -34.1 | -62.0 | -82.6 | -82.7 |
| centre std (2080) | -55.9 | -63.7 | -64.1 | -62.4 |
| **centre HREN (33280, PR #12 default)** | -56.7 | -82.4 | -88.1 | -85.9 |
| centre std + TPDF/NS2 | -56.7 | -77.4 | -78.3 | -77.9 |
| centre HREN + TPDF/NS2 | -56.7 | -84.6 | -102.5 | -102.1 |

Findings:
- HREN is required once centre-aligned is used: centre without HREN has only 2080 steps and is worse than edge at -40/-60 dBFS.
- Centre+HREN vs the old edge std: -22 dB at -6 dBFS (2nd harmonic), -18..-21 dB at -20..-60 dBFS (quantisation).
- Remaining step: TPDF + 2nd-order noise shaping on top of HREN gains another ~14-16 dB at -40/-60 dBFS (-88 -> -102 dBFS). -6 dBFS stays at -57 (ZOH/edge distortion, not resolution).
- Next candidates: NS2 in refill_half (CPU cost per 192 kHz word), real x4 interpolation (Theme 1 sim: removes the 2f-48k folds).

## Follow-up: real C shaping code (pwm_shape_c_sim.py, 2026-10-17)
Firmware `shape_frame()` extracted verbatim, host-compiled (float32), fed through the real HAL modulator, centre+HREN 192 kHz. Worst of 997/6900/15000/19000 Hz.

| case | -6 | -20 | -40 | -60 dBFS |
|---|---|---|---|---|
| ZOH (PR #12) | -56.5 | -82.2 | -87.7 | -84.3 |
| INTERP x4 FIR (new default) | -60.5 | -84.6 | -86.1 | -85.1 |
| INTERP + TPDF/NS2 | -60.5 | -85.2 | -87.4 | -86.3 |
| INTERP + NS2, HREN off | -60.5 | -78.5 | -78.7 | -78.7 |

- INTERP: -4 dB at -6 dBFS and removes the 2f-48k fold of 15-19 kHz tones (worst tone was 19 kHz).
- NS2 gives only ~1 dB: the floor is the HAL modulator rounding each 48 kHz duty to a whole Q4 count (white, ~-86 dBFS), already present before the board shaper. The -102 dBFS in the idealised sim assumed a fractional duty. => NS2 default OFF; the real lever is a fractional duty from the HAL (common HAL change, sonora first).
- Ideal-sim gap at -6 dBFS (-57 vs this -60.5) is the remaining centre-aligned uniform-sampling distortion.
