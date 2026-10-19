"""Legacy scope logs remain accepted; HAL logs cannot be misreported as legacy."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix='pwm-analyzer-') as temp:
        log, out = Path(temp) / 'uart.log', Path(temp) / 'analysis'
        cmd = [sys.executable, str(ROOT / 'buildtools/analyze_pwm_scope_log.py'), '--log', str(log), '--tag', 'test_id', '--out', str(out)]
        log.write_text('[source=uart] PWMscope id=test_id ms=1000 ready=1 sleep=0 pll2=1 clk5=1 pg=1 dma=1 cnt=0 half=10 done=10 err=0 fail=0\n'
                       '[source=uart] PWMscope pcmL_q31=0 pcm_blk=10 duty=133120/133120 per=266240 wr=10 overlap=0 tdm=1/10 miss=0 rtf=0/0/0 source=neutral map=tdm_tx_q31\n')
        result = subprocess.run(cmd, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        summary = json.loads((out / 'summary.json').read_text())
        assert summary['exact_half_duty_records'] == 1
        assert summary['mapping_modes'] == ['tdm_tx_q31']
        # Even a mixed log with valid legacy records cannot pass as the HAL ID.
        log.write_text(log.read_text() + '[source=uart] PWMhal id=test_id ms=2000 ready=1\n')
        result = subprocess.run(cmd, capture_output=True, text=True)
        assert result.returncode != 0 and 'supports only direct PWMscope' in result.stderr, result.stderr
        print('PASS scope analyzer: complete legacy records, explicit HAL rejection')


if __name__ == '__main__':
    main()
