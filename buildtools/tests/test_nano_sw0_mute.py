"""Exercise Nano SW0 short/long dispatch, master mute ramp, and LED timing."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = Path(r'C:\msys64\usr\bin\gcc.exe')


def function(text, signature):
    start = text.index(signature)
    end, depth = text.index('{', start) + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    gain = (ROOT / 'src/app/apps/classic/dsp/gain_ctrl.c').read_text(encoding='utf-8-sig')
    for inc in ('app_specific_config_defs.h', 'app_runtime_overrides.h', 'xc.h'):
        gain = gain.replace(f'#include "{inc}"', '').replace(f'#include <{inc}>', '')
    button = (ROOT / 'src/app/board/devices/button_led.c').read_text(encoding='utf-8-sig')
    start = button.index('#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE')
    board = button[button.index('\n', start) + 1:button.index('\n#else', start)]
    functions = '\n'.join(function(button, name) for name in (
        'bool BUTTON_Init(void)', 'bool BUTTON_IsPressed( uint8_t id )',
        'BUTTON_EVENT_t BUTTON_GetEvent( uint8_t id )',
        'void LED_On( uint8_t led )', 'void LED_Off( uint8_t led )',
        'void LED_heartbeat_set_muted( bool muted )',
        'void LED_heartbeat_tick( void )'))
    controls = (ROOT / 'src/app/apps/classic/classic_controls.c').read_text(encoding='utf-8-sig')
    functions += '\n' + function(controls, 'void UsrOperate_mute( void )')
    functions += '\n' + function(controls, 'void UsrOperate_avas_synth_button( void )')
    functions += '\n' + function(controls, 'static void local_button_handler( void )\n{')
    gain = gain.replace('#include "gain_ctrl.h"',
                        (ROOT / 'src/app/apps/classic/dsp/gain_ctrl.h').read_text(encoding='utf-8-sig'))
    mock = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "button_led.h"
#define APP_TARGET 3
#define APP_TARGET_AK506 3
#define RESOLVED_BOARD_TARGET 3
#define RESOLVED_BOARD_TARGET_AK506_VALUE 3
#define APP_MUTE_RAMP_MS 800
#define APP_BLOCK_FRAMES 32
#define STAGE_1_PROC_CH 2
#define SAMPLE_RATE 48000
#define BUTTON_DEBOUNCE_MS 15u
#define APP_USE_LED_HEARTBEAT 1
#define LED_LEVEL_ON false
#define LED_LEVEL_OFF true
#define ENA_AVAS_TYPE_TY_SYNTH 1
#define ENA_AVAS_TYPE_LB_SYNTH 1
#define NORA_GPIO_LEVEL_LOW 0
#define NORA_GPIO_DIR_INPUT 0
#define NORA_GPIO_PULL_UP 1
typedef unsigned nora_gpio_rp_t;
typedef struct {int dir,pull;bool analog,open_drain,initial_high;} nora_gpio_config_t;
static uint32_t now;
static int level=1;
static unsigned configured;
static bool s_heartbeat_muted, led_high=true;
static unsigned led_writes, avas_calls;
static bool s_avas_type_ty_on, s_avas_type_lb_on, reject_start;
static void UsrOperate_avas_synth(void){++avas_calls;if(s_avas_type_ty_on)s_avas_type_ty_on=false;else if(!reject_start)s_avas_type_ty_on=true;}
static void UsrOperate_avas_synth_type_lb(void){++avas_calls;if(s_avas_type_lb_on)s_avas_type_lb_on=false;else if(!reject_start)s_avas_type_lb_on=true;}
static uint32_t GetTicks(void){return now;}
static bool nora_gpio_rp_config(unsigned rp,const nora_gpio_config_t *cfg){assert(rp==36&&cfg->pull==1&&!cfg->analog&&cfg->dir==0);++configured;return true;}
static int nora_gpio_rp_read(unsigned rp){assert(rp==36);return level;}
static bool nora_gpio_rp_write(unsigned rp,bool high){assert(rp==49);led_high=high;++led_writes;return true;}
'''
    test = r'''
static float in[64],out[64];
static void block(void){app_gain_process(in,out);for(unsigned i=0;i<32;i++)assert(out[i]==out[32+i]);}
static void poll(int raw,unsigned elapsed){level=raw;now+=elapsed;local_button_handler();}
static void finish(void){for(unsigned i=0;i<1201;i++)block();}
static void short_press(void){poll(0,1);poll(0,15);poll(1,100);poll(1,15);}
static void long_press(void){
 unsigned calls=avas_calls;uint32_t request=s_mute_request;
 poll(0,1);poll(0,15);poll(0,1499);assert(avas_calls==calls);
 poll(0,1);assert(avas_calls==calls+1);
 poll(0,4000);assert(avas_calls==calls+1);
 poll(1,1);poll(1,15);assert(avas_calls==calls+1&&s_mute_request==request);
}
static void led_at(uint32_t tick,bool lit){now=tick;LED_heartbeat_tick();assert(led_high==!lit);}
int main(void){
 assert(BUTTON_Init()&&configured==1);app_gain_init(48000);
 for(unsigned i=0;i<64;i++)in[i]=1;
 poll(1,20);poll(0,1);poll(1,5);poll(0,5);poll(0,15);
 assert(s_mute_request==0&&My_Gain.mute_on==0&&avas_calls==0);
 poll(0,100);poll(1,1);poll(1,15);
 assert(s_mute_request==1601&&app_mute_requested());
 block();assert(My_Gain.mute_on==1&&out[0]>0.99f&&out[31]<out[0]);
 assert(My_Gain.rampTotalSamples==38400);
 finish();assert(My_Gain.prevGain==0&&My_Gain.rampRemainSamples==0);
 for(unsigned i=0;i<64;i++)assert(out[i]==0);
 short_press();assert(s_mute_request==1600&&!app_mute_requested());
 block();assert(out[0]>0&&out[31]>out[0]&&out[31]<0.01f);
 finish();assert(My_Gain.prevGain==1&&My_Gain.rampRemainSamples==0);
 app_mute_set(true,800);for(unsigned i=0;i<300;i++)block();float before=My_Gain.prevGain;
 assert(before>0.7f&&before<0.8f);app_mute_set(false,800);
 assert(My_Gain.prevGain==before);block();assert(out[0]>before&&out[0]-before<0.001f);
 finish();assert(My_Gain.prevGain==1);
 app_mute_set(true,0);block();assert(My_Gain.prevGain==0);
 app_mute_set(false,0);block();assert(My_Gain.prevGain==1);
 for(unsigned cycle=0;cycle<2;cycle++){
  long_press();assert(s_avas_type_ty_on&&!s_avas_type_lb_on);
  long_press();assert(!s_avas_type_ty_on&&!s_avas_type_lb_on);
  long_press();assert(!s_avas_type_ty_on&&s_avas_type_lb_on);
  long_press();assert(!s_avas_type_ty_on&&!s_avas_type_lb_on);
 }
 reject_start=true;long_press();assert(!s_avas_type_ty_on&&!s_avas_type_lb_on);
 reject_start=false;long_press();assert(s_avas_type_ty_on&&!s_avas_type_lb_on);
 long_press();
 short_press();assert(app_mute_requested());
 long_press();assert(s_avas_type_lb_on&&app_mute_requested());
 long_press();short_press();assert(!app_mute_requested());
 LED_heartbeat_set_muted(false);
 led_at(10000,false);led_at(10499,false);led_at(10500,true);
 unsigned writes=led_writes;led_at(10999,true);assert(led_writes==writes);
 led_at(11000,false);led_at(13580,true); // foreground skipped cycles
 LED_heartbeat_set_muted(true);led_at(13580,true);led_at(13679,true);
 led_at(13680,false);led_at(14579,false);led_at(14580,true);
 led_at(14680,false);led_at(16600,true); // skipped pulses, use current phase
 LED_heartbeat_set_muted(false);led_at(16601,false);led_at(17101,true);
 LED_heartbeat_set_muted(true);led_at(UINT32_MAX-50u,true);
 led_at(48u,true);led_at(49u,false);led_at(948u,false);led_at(949u,true);
 puts("PASS SW0 RP36 pull-up/debounce, 1500ms one-shot TY/OFF/LB/OFF, short release mute, 800ms stereo ramps/reversal, 500ms LED and 100ms/1s muted pulse including tick wrap");
}
'''
    env = os.environ.copy()
    env['PATH'] = str(CC.parent) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='nano-sw0-') as temp:
        directory = Path(temp)
        for name, relative in [('gain_ctrl.h', 'src/app/apps/classic/dsp/gain_ctrl.h'),
                               ('button_led.h', 'src/app/board/devices/button_led.h')]:
            (directory / name).write_bytes((ROOT / relative).read_bytes())
        source, binary = directory / 'test.c', directory / 'test.exe'
        mock = mock.replace('#include "button_led.h"',
                            (ROOT / 'src/app/board/devices/button_led.h').read_text(encoding='utf-8-sig'))
        source.write_text(mock + gain + board + functions + test, encoding='utf-8')
        subprocess.run([str(CC), '-std=c99', '-O3', '-ffast-math', '-Wall', '-Wextra',
                        '-Werror', '-Wno-format', '-Wno-unused-function', '-Wno-unused-variable',
                        str(source), '-lm', '-o', str(binary)], check=True, env=env)
        subprocess.run([str(binary)], check=True, env=env)


if __name__ == '__main__':
    main()
