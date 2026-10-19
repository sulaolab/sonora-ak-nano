"""Firmware-compiler checks for AK-nano PWM defaults, routes and conflicts."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, default=Path(os.environ.get('ProgramFiles', '/opt')) / 'Microchip/xc-dsc/v3.31.01/bin/xc-dsc-gcc.exe')
    parser.add_argument('--dfp', type=Path, default=Path.home() / '.mchp_packs/Microchip/dsPIC33AK-MP_DFP/1.5.269/xc16')
    args = parser.parse_args()
    mpq = ['APP_PWM_OUTPUT=1']
    cases = [
        ('classic1_pwm_off', [], {'APP_USE_PWM_AUDIO': 0, 'APP_PWM_AUDIO_HAL': 0, 'APP_USE_SPI2_AUDIO': 0}, None),
        ('hal_slot3', mpq, {'APP_PWM_AUDIO_HAL': 1, 'MPQ6612A_NANO_SLOT': 3, 'MPQ6612A_STEREO': 0, 'MPQ6612A_IN1_RP': 23, 'MPQ6612A_IN2_RP': 25, 'MPQ6612A_SLEEP_RP': 52, 'MPQ6612A_FAULT_RP': 51, 'MPQ6612A_VISEN_RP': 2, 'NORA_PWM_AUDIO_ENABLE': 1}, None),
        ('hal_slot2', mpq + ['MPQ6612A_NANO_SLOT=2'], {'MPQ6612A_IN1_RP': 9, 'MPQ6612A_IN2_RP': 19, 'MPQ6612A_SLEEP_RP': 42, 'MPQ6612A_FAULT_RP': 41, 'MPQ6612A_VISEN_RP': 18}, None),
        ('direct_slot3', mpq + ['APP_PWM_AUDIO_HAL=0'], {'APP_USE_PWM_AUDIO': 1, 'NORA_PWM_AUDIO_ENABLE': 0}, None),
        ('hal_scope', mpq + ['PWM_AUDIO_SCOPE_ONLY=1'], {'APP_PWM_AUDIO_HAL': 1, 'PWM_AUDIO_SCOPE_ONLY': 1}, None),
        ('direct_scope', mpq + ['APP_PWM_AUDIO_HAL=0', 'PWM_AUDIO_SCOPE_ONLY=1'], {'NORA_PWM_AUDIO_ENABLE': 0}, None),
        ('classic_drc', ['APP_BUILD=APP_BUILD_DRC_DEMO'], {'APP_USE_PWM_AUDIO': 0, 'APP_PWM_AUDIO_HAL': 0, 'BIQUAD_CASCADE_4CH_NUM_STAGE': 156}, None),
        ('legacy_on_nano', ['APP_USE_PWM_AUDIO=1'], {}, 'Nano PWM supports MPQ6612A only'),
        ('stereo_on_nano', mpq + ['MPQ6612A_STEREO=1'], {}, 'stereo requires Sonora AK512'),
        ('slot1', mpq + ['MPQ6612A_NANO_SLOT=1'], {}, 'Nano slot must be 2 or 3'),
        ('slot4', mpq + ['MPQ6612A_NANO_SLOT=4'], {}, 'Nano slot must be 2 or 3'),
        ('drc_mpq', mpq + ['APP_BUILD=APP_BUILD_DRC_DEMO'], {}, 'Classic 1 with PWM enabled'),
        ('secondary_bare', mpq + ['PWM_AUDIO_ENABLE_SECONDARY_PAIR'], {}, 'cannot use PWM_AUDIO_ENABLE_SECONDARY_PAIR'),
        ('secondary_zero', mpq + ['PWM_AUDIO_ENABLE_SECONDARY_PAIR=0'], {}, 'cannot use PWM_AUDIO_ENABLE_SECONDARY_PAIR'),
        ('invalid_hal', mpq + ['APP_PWM_AUDIO_HAL=2'], {}, 'APP_PWM_AUDIO_HAL must be 0 or 1'),
        ('invalid_scope', mpq + ['PWM_AUDIO_SCOPE_ONLY=2'], {}, 'PWM_AUDIO_SCOPE_ONLY must be 0 or 1'),
        ('invalid_output', ['APP_PWM_OUTPUT=2'], {}, 'APP_PWM_OUTPUT must be 0'),
        ('scope_without_mpq', ['PWM_AUDIO_SCOPE_ONLY=1'], {}, 'scope-only requires the MPQ6612A route'),
        ('disabled_pwm', mpq + ['APP_USE_PWM_AUDIO=0'], {}, 'Classic 1 with PWM enabled'),
        ('sonora_mikrob', mpq + ['MPQ6612A_ENABLE_MIKROB=1'], {}, 'Nano PWM uses MPQ6612A_NANO_SLOT'),
        ('sonora_xpro', mpq + ['MPQ6612A_ENABLE_XPRO1=1'], {}, 'Nano PWM uses MPQ6612A_NANO_SLOT'),
        ('pinned_profile_mismatch', ['SONORA_MPLAB_APP_BUILD=APP_BUILD_DRC_DEMO', 'APP_BUILD=APP_BUILD_STD_DEMO_1'], {}, 'pinned application profile'),
    ]
    cases += [
        ('pot_slot2_pwm_slot3', mpq + ['ENA_NANO_POT=1'], {'ENA_NANO_POT':1, 'MPQ6612A_NANO_SLOT':3}, None),
        ('pot_pwm_conflict', mpq + ['ENA_NANO_POT=1','MPQ6612A_NANO_SLOT=2'], {}, 'slot2 POT conflicts'),
        ('pot_invalid_span', mpq + ['ENA_NANO_POT=1','NANO_POT_SPAN_COUNTS=0'], {}, 'SPAN_COUNTS must be'),
        ('pot_invalid_direction', mpq + ['ENA_NANO_POT=1','NANO_POT_INVERT=2'], {}, 'INVERT must be 0 or 1'),
        ('pot_invalid_enable', mpq + ['ENA_NANO_POT=2'], {}, 'ENA_NANO_POT must be'),
        ('carrier_default', mpq, {'MPQ6612A_CARRIER_FACTOR': 4, 'PWM_AUDIO_UPSAMPLE_FACTOR': 4, 'PWM_AUDIO_PERIOD_COUNT_Q4': 66560, 'PWM_AUDIO_DUTY_WORD_COUNT': 256}, None),
        ('carrier48', mpq + ['MPQ6612A_CARRIER_FACTOR=1'], {'PWM_AUDIO_UPSAMPLE_FACTOR': 1, 'PWM_AUDIO_PERIOD_COUNT_Q4': 266240}, None),
        ('carrier96', mpq + ['MPQ6612A_CARRIER_FACTOR=2'], {'PWM_AUDIO_UPSAMPLE_FACTOR': 2, 'PWM_AUDIO_PERIOD_COUNT_Q4': 133120}, None),
        ('carrier192', mpq + ['MPQ6612A_CARRIER_FACTOR=4'], {'PWM_AUDIO_UPSAMPLE_FACTOR': 4, 'PWM_AUDIO_PERIOD_COUNT_Q4': 66560, 'PWM_AUDIO_DUTY_WORD_COUNT': 256}, None),
        ('carrier_zero', mpq + ['MPQ6612A_CARRIER_FACTOR=0'], {}, 'CARRIER_FACTOR must be 1, 2 or 4'),
        ('carrier_non_supported', mpq + ['MPQ6612A_CARRIER_FACTOR=3'], {}, 'CARRIER_FACTOR must be 1, 2 or 4'),
        ('carrier_over_ic_limit', mpq + ['MPQ6612A_CARRIER_FACTOR=5'], {}, 'exceeds the 200 kHz'),
    ]
    with tempfile.TemporaryDirectory(prefix='nano-pwm-config-') as temp:
        source = Path(temp) / 'config.c'
        for name, defines, expected, error in cases:
            checks = ''.join(f'\n#if ({key}) != ({value})\n#error "unexpected {key}"\n#endif\n' for key, value in expected.items())
            source.write_text('#include "app_specific_config_defs.h"\n#include "board/audio/mpq6612a_board.h"\n#include "board/audio/nora_pwm_audio_conf.h"\n#include "pwm_audio_dma_buffer.h"\n' + checks)
            includes = [ROOT / 'src/app', *sorted((ROOT / 'src/app').glob('hal_*'))]
            cmd = [str(args.cc), '-E', '-P', '-mcpu=33AK512MPS506', f'-mdfp={args.dfp}', *['-I' + str(p) for p in includes], '-DAPP_REQ_MIKROB_WM8904=0', '-x', 'c', *['-D' + d for d in defines], str(source)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if error:
                assert result.returncode != 0 and error in result.stderr, (name, result.stderr)
            else:
                assert result.returncode == 0, (name, result.stderr)
            print(f'PASS {name}: ' + ('rejected as intended' if error else 'resolved defaults and pins'))


if __name__ == '__main__':
    main()
