"""Host-check the actual scope TDM Q31 converter, including signed endpoints."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    source = (ROOT / 'src/app/apps/shared/float_conversion.c').read_text(encoding='utf-8-sig')
    begin = source.index('void convert_codec_q31_to_pwm_20bit(')
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    function = source[begin:end]
    gcc = shutil.which('gcc') or (r'C:\msys64\usr\bin\gcc.exe' if os.name == 'nt' else None)
    if not gcc or not Path(gcc).is_file():
        raise SystemExit('Host GCC is required for this numerical converter check.')
    harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
FUNCTION
int main(void) {
    const int32_t points[] = {INT32_MIN, -1073741824, 0, 1073741824, INT32_MAX};
    const int32_t expected[] = {256, 66688, 133120, 199552, 265984};
    int32_t interleaved[10], out[15];
    for (unsigned i=0; i<5; ++i) {
        interleaved[2*i] = points[i];
        interleaved[2*i+1] = INT32_MIN; /* R must never contaminate L. */
    }
    convert_codec_q31_to_pwm_20bit(interleaved, 2, out, 5, 3, 266240);
    for (unsigned i=0; i<5; ++i)
        for (unsigned k=0; k<3; ++k) assert(out[3*i+k] == expected[i]);
    /* Independently calculate the linear map in double precision, sweep the
       signed range and verify monotonicity for even/odd hardware periods. */
    const uint32_t periods[] = {266240, 256000, 133120, 266241};
    for (unsigned p=0; p<4; ++p) {
        int32_t previous = 0;
        for (unsigned i=0; i<=4096; ++i) {
            int32_t pcm = i == 4096 ? INT32_MAX :
                (int32_t)((int64_t)INT32_MIN + (int64_t)i * 1048576);
            int32_t result = -1;
            convert_codec_q31_to_pwm_20bit(&pcm, 1, &result, 1, 1, periods[p]);
            double x = (double)pcm / 2147483648.0;
            int32_t reference = (int32_t)floor(256.0 +
                (x + 1.0) * ((double)periods[p] - 512.0) / 2.0 + 0.5);
            assert(result == reference);
            assert(result >= 256 && result <= (int32_t)periods[p] - 256);
            if (i) assert(result >= previous);
            previous = result;
        }
    }
    int32_t untouched=123, zero=0;
    convert_codec_q31_to_pwm_20bit(NULL,1,&untouched,1,1,266240);
    convert_codec_q31_to_pwm_20bit(&zero,0,&untouched,1,1,266240);
    convert_codec_q31_to_pwm_20bit(&zero,1,&untouched,1,0,266240);
    convert_codec_q31_to_pwm_20bit(&zero,1,&untouched,1,1,512);
    assert(untouched==123);
    puts("PASS: Q31 signed endpoints, zero midpoint, quarter scales, L stride, ZOH,");
    puts("      16388 linear/monotonic sweep points and invalid-argument guards.");
}
'''.replace('FUNCTION', function)
    env = os.environ.copy()
    env['PATH'] = str(Path(gcc).parent) + os.pathsep + env['PATH']
    with tempfile.TemporaryDirectory(prefix='pwm-q31-') as temp:
        test = Path(temp) / 'check.c'
        binary = Path(temp) / ('check.exe' if os.name == 'nt' else 'check')
        test.write_text(harness, encoding='utf-8')
        subprocess.run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(test),
                        '-o', str(binary), '-lm'], check=True, env=env)
        subprocess.run([str(binary)], check=True, env=env)

if __name__ == '__main__':
    main()
