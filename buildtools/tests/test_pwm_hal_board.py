"""Host integration of unchanged hal_pwm_audio and the actual Sonora board backend.
Does not simulate electrical PWM edges or DMA timing on the MCU.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
HAL=ROOT/'src/app/hal_pwm_audio'
CC=Path(r'C:\msys64\usr\bin\gcc.exe')
os.environ['PATH']=str(CC.parent)+os.pathsep+os.environ.get('PATH','')
MOCK=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define SONORA_CLOCK_PWM_MCLK_HZ 798720000u
#define SAMPLE_RATE 48000u
#define APP_BLOCK_FRAMES 32u
#define PWM_AUDIO_UPSAMPLE_FACTOR (APP_PWM_OUTPUT == 1 ? TEST_CARRIER_FACTOR : 2u)
#define PWM_AUDIO_PERIOD_COUNT_Q4 ((798720000u/48000u/PWM_AUDIO_UPSAMPLE_FACTOR)<<4)
#include "mpq6612a_board.h"
#define NORA_PWM_GEN_5 5
#define NORA_PWM_GEN_6 6
#define NORA_DMA_CHANNEL_4 4
#define NORA_DMA_CHANNEL_5 5
#define NORA_DMA_TRIGGER_PWM_GEN5 5
#define NORA_DMA_TRMODE_REPEAT_ONESHOT 1
#define NORA_DMA_ADDR_INCREMENT 1
#define NORA_DMA_ADDR_FIXED 0
#define NORA_DMA_SIZE_WORD 4
#define NORA_PWM_MCLK_HIGH_FREQ 1
#define NORA_PWM_MODE_COMPLEMENTARY 1
#define NORA_GPIO_LEVEL_HIGH 1
#define NORA_GPIO_LEVEL_LOW 0
#define SONORA_CLOCK_PWM_OK 0
#define NORA_DMA_HALF_FIRST 1
#define NORA_DMA_HALF_SECOND 2
#define NORA_DMA_DSPIC33AK_STAT_HALF 1u
#define NORA_DMA_DSPIC33AK_STAT_DONE 2u
#define NORA_DMA_DSPIC33AK_STAT_OVERRUN 4u
typedef int nora_gpio_rp_t,nora_dma_channel_t,nora_dma_trigger_t,nora_pwm_generator_id_t,nora_clock_status_t,nora_dma_half_t;
typedef unsigned nora_dma_status_t;
typedef struct {volatile void *src,*dst;unsigned count,src_mode,dst_mode,size,tr_mode,trigger,irq_priority;bool reload_count,reload_src,reload_dst,half_int_en,done_int_en,irq_priority_set,irq_enable;} nora_dma_channel_cfg_t;
typedef struct {unsigned period,duty_init,dead_time_high,dead_time_low,rp_h,rp_l,output_mode,trigger_master;bool pen_h,pen_l,trigger_follows,is_module_master,start_overridden,start_disabled;} nora_pwm_generator_cfg_t;
static struct {unsigned PLL2RDY;} OSCCTRLbits={1};
static struct {unsigned CLKRDY;} CLK5CONbits={1};
static struct {unsigned ON,MODSEL,HREN;} PG5CONbits,PG6CONbits;
static struct {unsigned PER;} PG5PERbits,PG6PERbits;
static struct {unsigned IEVTSEL;} PG5EVT1bits,PG6EVT1bits;
static struct {unsigned DIVSEL,HRRDY,HRERR;} PCLKCONbits={0,1,0};
static uint32_t PG5TRIGA,PG6TRIGA;
static struct {unsigned CHEN;} DMA4CHbits,DMA5CHbits;
static volatile uint32_t PG5DC,PG6DC;
static bool pin[128],ovr[8];
static unsigned pin_access[128];
static uintptr_t dma_src[8];
static nora_dma_channel_cfg_t dma_cfg[8];
static nora_pwm_generator_cfg_t pg_cfg[8];
static unsigned fail_at,calls,events[128],event_count;
void pwm_audio_board_service(void);
static nora_dma_status_t irq_status;
static bool injected_stage_fault;
static bool maybe_fail(void){return ++calls!=fail_at;}
static bool nora_gpio_rp_clear(int p){++pin_access[p];pin[p]=false;return true;}
static bool nora_gpio_rp_set(int p){++pin_access[p];pin[p]=true;return true;}
static bool nora_gpio_rp_config_digital_output(int p,bool v){++pin_access[p];pin[p]=v;return maybe_fail();}
static bool nora_gpio_rp_config_digital_input(int p){++pin_access[p];pin[p]=true;return maybe_fail();}
static int nora_gpio_rp_read(int p){++pin_access[p];if(injected_stage_fault&&(p==82||p==24||p==51||p==41))return 0;return pin[p];}
static int nora_gpio_rp_read_output(int p){++pin_access[p];return pin[p];}
static bool nora_pwm_generator_override(int id,bool en,bool high,bool sync){(void)high;(void)sync;ovr[id]=en;return maybe_fail();}
static bool nora_pwm_generator_run(int id,bool on){if(id==5)PG5CONbits.ON=on;else PG6CONbits.ON=on;if(on)events[event_count++]=100+id;return maybe_fail();}
static uint32_t PG5PER;
static bool nora_pwm_generator_init(int id,const nora_pwm_generator_cfg_t *c){if(id==5)PG5PER=c->period;assert(c->start_disabled&&c->start_overridden);++pin_access[c->rp_h];++pin_access[c->rp_l];pg_cfg[id]=*c;ovr[id]=true;return maybe_fail();}
static bool nora_dma_channel_enable(int id,bool on){if(id==4)DMA4CHbits.CHEN=on;else DMA5CHbits.CHEN=on;return maybe_fail();}
static bool nora_dma_channel_config(int id,const nora_dma_channel_cfg_t *c){dma_cfg[id]=*c;dma_src[id]=(uintptr_t)c->src;return maybe_fail();}
static uintptr_t nora_dma_read_src(int id){return dma_src[id];}
static nora_dma_status_t nora_dma_isr_snapshot(int id){(void)id;return irq_status;}
static nora_dma_status_t nora_dma_read_status(int id){(void)id;return 0;}
static void nora_dma_clear_status(int id){(void)id;}
static bool nora_dma_status_has_overrun(nora_dma_status_t s){return (s&4)!=0;}
static bool nora_dma_status_has_half_done_conflict(nora_dma_status_t s){return (s&3)==3;}
static int nora_dma_half_from_status(nora_dma_status_t s){return (s&2)?2:((s&1)?1:0);}
static int sonora_clock_pwm_prepare(nora_clock_status_t *d){*d=0;return 0;}
static bool nora_pwm_module_init(int c){(void)c;return true;}
static uint32_t GetTicks(void){return 1000;}
static void delay_us(unsigned t){(void)t;}
static uint32_t hrt_now;
static bool nora_high_res_timer_is_initialized(void){return true;}
static uint32_t nora_high_res_timer_get_count(void){return hrt_now;}
static uint32_t nora_high_res_timer_elapsed_count(uint32_t s){hrt_now+=7;return hrt_now-s;}
static uint32_t nora_high_res_timer_count_to_us_x10(uint32_t c){return c;}
#include "pwm_audio_interp_coeffs.h"
'''
TEST=r'''
static void trigger(unsigned half){
    irq_status=half;
    unsigned active=half==1?BOARD_HALF_WORDS:0;
    dma_src[4]=(uintptr_t)(s_duty_l+active);
#if BOARD_CHANNELS == 2
    dma_src[5]=(uintptr_t)(s_duty_r+active);
#endif
    _DMA4Interrupt();
}
static void assert_safe(void){
    assert(!PG5CONbits.ON&&!DMA4CHbits.CHEN);
#if BOARD_CHANNELS == 2
    assert(!PG6CONbits.ON&&!DMA5CHbits.CHEN);
#endif
#if APP_PWM_OUTPUT == 1
    assert(pg_cfg[5].rp_h==MPQ6612A_IN1_RP&&pg_cfg[5].rp_l==MPQ6612A_IN2_RP);
    assert(!pin[MPQ6612A_SLEEP_RP]);
#if MPQ6612A_STEREO
    assert(!pin[MPQ6612A_RIGHT_SLEEP_RP]);
#endif
#endif
}
int main(void){
    nora_pwm_audio_config_t c={48000,32,BOARD_CHANNELS};
    int32_t l[32],r[32];
    nora_pwm_audio_block_t b={l,r};
    nora_pwm_audio_status_t st;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    calls=0;event_count=0;assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    assert(pg_cfg[5].period==BOARD_PERIOD_Q4);
    /* Carrier unchanged: (PER + 16) Q4 per timer cycle, centre = 2 cycles. */
    assert(BOARD_DC_DIV*(BOARD_PER_REG+16u)==BOARD_PERIOD_Q4);
#if BOARD_PG_TUNED
    assert(PG5CONbits.MODSEL==(PWM_AUDIO_AK_CENTRE?4u:0u)&&PG5CONbits.HREN==(unsigned)PWM_AUDIO_AK_HREN);
    assert(PG5PERbits.PER==BOARD_PER_REG&&PG5DC==BOARD_NEUTRAL_Q4);
    assert(PG5EVT1bits.IEVTSEL==(PWM_AUDIO_AK_TRIGA?1u:0u)&&(PG5TRIGA&0x8000000Fu)==0u);
    assert(PG5TRIGA>0u&&PG5TRIGA<BOARD_PER_REG);
#else
    assert(PG5CONbits.MODSEL==0u&&PG5CONbits.HREN==0u&&PG5TRIGA==0u);
#endif
    assert(dma_cfg[4].count==2u*32u*BOARD_FACTOR);
    assert(pg_cfg[5].dead_time_high==(APP_PWM_OUTPUT==1?0u:16u));
#if BOARD_CHANNELS == 2
    assert(events[0]==106&&events[1]==105);
    assert(dma_cfg[5].trigger==NORA_DMA_TRIGGER_PWM_GEN5);
    assert(!dma_cfg[5].irq_enable);
#endif
#if APP_PWM_OUTPUT == 1
    assert(pin[MPQ6612A_SLEEP_RP]==!PWM_AUDIO_SCOPE_ONLY);
#if MPQ6612A_STEREO
    assert(pin[MPQ6612A_RIGHT_SLEEP_RP]==!PWM_AUDIO_SCOPE_ONLY);
#endif
#endif
    for(unsigned i=0;i<32;i++){l[i]=INT32_MIN;r[i]=INT32_MAX;}
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    trigger(1);
#if PWM_AUDIO_AK_INTERP
    /* FIR: every word stays inside the 500 ns clamp; after the 24-tap
     * history fills a full-scale block settles on the clamp (NS2 may lift a
     * word by a few steps, never below). Underrun refills neutral input, so
     * the output returns to neutral within the same few steps. */
    for(unsigned i=0;i<BOARD_HALF_WORDS;i++){
        assert(s_duty_l[i]>=BOARD_DC_MIN&&s_duty_l[i]<=BOARD_DC_MAX);
#if BOARD_CHANNELS == 2
        assert(s_duty_r[i]>=BOARD_DC_MIN&&s_duty_r[i]<=BOARD_DC_MAX);
#endif
    }
    for(unsigned i=4*24;i<BOARD_HALF_WORDS;i++){
        assert(s_duty_l[i]-BOARD_DC_MIN<=(uint32_t)(4.0f*BOARD_DC_STEP));
#if BOARD_CHANNELS == 2
        assert(BOARD_DC_MAX-s_duty_r[i]<=(uint32_t)(4.0f*BOARD_DC_STEP));
#endif
    }
    trigger(2);trigger(1);
    for(unsigned i=4*24;i<BOARD_HALF_WORDS;i++){
        int32_t dev=(int32_t)s_duty_l[i]-(int32_t)BOARD_NEUTRAL_Q4;
        assert(dev>=-(int32_t)(4.0f*BOARD_DC_STEP)&&dev<=(int32_t)(4.0f*BOARD_DC_STEP));
    }
#else
    for(unsigned i=0;i<BOARD_HALF_WORDS;i++){
        /* Pulse width = BOARD_DC_DIV * PGxDC: the 500 ns floor is kept. */
        assert(s_duty_l[i]*BOARD_DC_DIV==BOARD_MIN_DUTY_Q4);
#if BOARD_CHANNELS == 2
        assert(s_duty_r[i]==BOARD_DC_SPAN-BOARD_MIN_DUTY_Q4/BOARD_DC_DIV);
#endif
    }
    trigger(2);trigger(1);
    assert(s_duty_l[0]==BOARD_NEUTRAL_Q4);
#endif
    nora_pwm_audio_get_status(&st);assert(st.written_blocks==2&&st.underrun_blocks==1&&st.fault_flags==0);
    /* A nonconstant block must keep sample order and repeat each duty for
     * exactly FACTOR carrier cycles, across both DMA halves. */
    for(unsigned i=0;i<32;i++){l[i]=(int32_t)((int64_t)i*100000000-1550000000);r[i]=-l[i];}
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
#if PWM_AUDIO_AK_INTERP
    /* A ramp through the FIR stays a ramp: 4-word means rise monotonically
     * once the sawtooth reset has left the 24-tap window. */
    trigger(2);trigger(1);
    for(unsigned i=25;i<32;i++){ /* sawtooth reset at i=0 leaves the window at i=24 */
        uint32_t a=0,p=0;
        for(unsigned k=0;k<4;k++){a+=s_duty_l[i*4+k];p+=s_duty_l[(i-1)*4+k];}
        assert(a>p);
    }
    for(unsigned half=0;half;){
#else
    for(unsigned half=2;half>=1;--half){
#endif
        trigger(half);
        unsigned off=half==1?0:BOARD_HALF_WORDS;
        for(unsigned i=0;i<32;i++){
            if(i)assert(s_duty_l[off+i*BOARD_FACTOR]>s_duty_l[off+(i-1)*BOARD_FACTOR]);
            for(unsigned k=1;k<BOARD_FACTOR;k++){
                assert(s_duty_l[off+i*BOARD_FACTOR+k]==s_duty_l[off+i*BOARD_FACTOR]);
#if BOARD_CHANNELS == 2
                assert(s_duty_r[off+i*BOARD_FACTOR+k]==s_duty_r[off+i*BOARD_FACTOR]);
#endif
            }
        }
    }
    irq_status=4;_DMA4Interrupt();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags==NORA_PWM_AUDIO_FAULT_DMA);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);
    nora_pwm_audio_stop();assert_safe();
    fail_at=0;assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    calls=0;nora_pwm_audio_stop();assert_safe();
    /* Inject every checked resource operation during start; every failure cleans both streams. */
    for(unsigned f=1;f<22;f++){
        fail_at=0;injected_stage_fault=false;assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
        calls=0;fail_at=f;
        nora_pwm_audio_result_t res=nora_pwm_audio_start();
        fail_at=0;
        if(res!=NORA_PWM_AUDIO_OK)assert_safe();
        nora_pwm_audio_stop();assert_safe();
    }
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    OSCCTRLbits.PLL2RDY=0;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
    OSCCTRLbits.PLL2RDY=1;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
#if BOARD_CHANNELS == 2
    dma_src[4]=(uintptr_t)(s_duty_l+BOARD_HALF_WORDS);
    dma_src[5]=(uintptr_t)(s_duty_r+BOARD_HALF_WORDS+8);
    irq_status=1;_DMA4Interrupt();nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_DMA);
    assert(s_phase_errors>0);nora_pwm_audio_stop();assert_safe();
#endif
#if PWM_AUDIO_AK_HREN
    /* HR PLL never ready: start must fault with the outputs still parked. */
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);PCLKCONbits.HRRDY=0;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_CLOCK);
    nora_pwm_audio_stop();PCLKCONbits.HRRDY=1;
    /* Lock lost while released: the service loop faults and parks. */
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    PCLKCONbits.HRERR=1;pwm_audio_board_service();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_CLOCK);
    nora_pwm_audio_stop();assert_safe();PCLKCONbits.HRERR=0;
#endif
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    CLK5CONbits.CLKRDY=0;pwm_audio_board_service();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_CLOCK);
    nora_pwm_audio_stop();assert_safe();CLK5CONbits.CLKRDY=1;
    /* A late refill must fault before overwriting the currently active half. */
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    dma_src[4]=(uintptr_t)s_duty_l;
#if BOARD_CHANNELS == 2
    dma_src[5]=(uintptr_t)s_duty_r;
#endif
    irq_status=1;_DMA4Interrupt();nora_pwm_audio_get_status(&st);
    assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_DMA);assert(s_overlaps>0);
    nora_pwm_audio_stop();assert_safe();
#if APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);injected_stage_fault=true;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
#endif
#if APP_PWM_OUTPUT == 1
    /* Explicit lab control works with nFAULT Low (absent/unpowered PCB),
     * including a fault latched by normal startup. Scope may wake only here. */
    nora_pwm_audio_stop();pwm_audio_board_set_lab_mode(true);
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    injected_stage_fault=true;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    assert(pin[MPQ6612A_SLEEP_RP]&&PG5CONbits.ON&&DMA4CHbits.CHEN);
    pwm_audio_board_service();nora_pwm_audio_get_status(&st);
    assert(st.fault_flags==0);
    nora_pwm_audio_stop();assert_safe();pwm_audio_board_set_lab_mode(false);
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
#if !PWM_AUDIO_SCOPE_ONLY
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
#endif
    injected_stage_fault=false;
#endif
    /* XPRO1-only must never configure, read or drive any MikroBUS B net,
     * including start/fault cleanup when the PWM PCB is absent. */
#if APP_PWM_OUTPUT == 1 && !MPQ6612A_ENABLE_MIKROB && !defined(APP_TARGET_AK506)
    assert(pg_cfg[5].rp_h==21&&pg_cfg[5].rp_l==18);
    assert(pin_access[98]==0&&pin_access[29]==0&&pin_access[102]==0&&pin_access[82]==0&&pin_access[4]==0&&pin_access[97]==0);
#endif
#if defined(APP_TARGET_AK506)
    /* WM8904 slot-1 SPI pins and its XTAL/RP24 belong to the codec/clock layer.
     * The slot-3 PWM backend must not touch slot-2 control nets reserved for POT. */
    assert(pin_access[24]==0&&pin_access[11]==0&&pin_access[55]==0&&pin_access[40]==0&&pin_access[39]==0);
#if MPQ6612A_NANO_SLOT == 3
    assert(pg_cfg[5].rp_h==23&&pg_cfg[5].rp_l==25);
    assert(pin_access[9]==0&&pin_access[19]==0&&pin_access[42]==0&&pin_access[41]==0&&pin_access[18]==0);
#else
    assert(pg_cfg[5].rp_h==9&&pg_cfg[5].rp_l==19);
    assert(pin_access[23]==0&&pin_access[25]==0&&pin_access[52]==0&&pin_access[51]==0&&pin_access[2]==0);
#endif
#endif
    printf("PASS board centre=%u hren=%u triga=%u interp=%u ns2=%u ",(unsigned)PWM_AUDIO_AK_CENTRE,(unsigned)PWM_AUDIO_AK_HREN,(unsigned)PWM_AUDIO_AK_TRIGA,(unsigned)PWM_AUDIO_AK_INTERP,(unsigned)PWM_AUDIO_AK_NS2);
    printf("channels=%u scope=%u route=%u: L/R mapping, neutral, start order, cleanup, DMA/clock/stage faults\n",BOARD_CHANNELS,PWM_AUDIO_SCOPE_ONLY,APP_PWM_OUTPUT);
}
'''
def main():
    source=(ROOT/'src/app/board/audio/pwm_audio_board_ak.c').read_text()
    source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    source=source.replace('__attribute__((interrupt, context))','')
    source=source.replace('(uint32_t)s_duty_l','(uintptr_t)s_duty_l').replace('(uint32_t)s_duty_r','(uintptr_t)s_duty_r')
    cases = [(1,1,1,1,0),(1,1,1,0,0),(1,1,0,1,0),(1,1,0,0,0),(1,0,1,1,0),(1,0,1,0,0),(0,0,0,0,0)]
    cases += [(1,0,0,scope,slot) for slot in (2,3) for scope in (0,1)]
    cases = [(*case,factor,(0,0)) for case in cases for factor in ((1,2,4) if case[0] else (2,))]
    # Alignment prototypes (centre, hren): 192 kHz Nano slot 3 and Sonora stereo.
    for align in ((0,1),(1,0),(1,1)):
        cases += [(1,0,0,scope,3,4,align) for scope in (0,1)] + [(1,1,1,0,0,4,align)]
    # No define at all: firmware defaults (MPQ centre+HREN+INTERP at 192 kHz,
    # NS2 off; MIC4607 edge ZOH).
    cases += [(1,0,0,0,3,4,None),(1,0,0,1,3,4,None),(1,1,1,0,0,4,None),(0,0,0,0,0,2,None)]
    # The alignment rows above keep the old ZOH path explicit.
    cases = [c if c[6] is None else (*c[:6], (*c[6], 0)) for c in cases]
    # INTERP without NS2, and INTERP+NS2 with standard resolution.
    cases += [(1,0,0,0,3,4,(1,1,1,1)),(1,0,0,0,3,4,(1,0,1,1)),(1,1,1,0,0,4,(1,1,1,1))]
    for output,b,xpro,scope,nano_slot,factor,align in cases:
        stereo=b and xpro
        with tempfile.TemporaryDirectory(prefix='pwm-hal-board-') as tmp:
            case=Path(tmp)
            ch=1+stereo if output==1 else 2
            conf=f'''#include <stdint.h>
#define NORA_PWM_AUDIO_ENABLE 1
#define NORA_PWM_AUDIO_MAX_CHANNELS {ch}
#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ 48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_FIFO_BLOCKS 4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u
#define NORA_PWM_AUDIO_DUTY_T uint32_t
'''
            (case/'nora_pwm_audio_conf.h').write_text(conf)
            (case/'app_specific_config_defs.h').write_text('/* Config supplied by this host case. */\n')
            nano = f'#define APP_TARGET_AK506 506\n#define APP_TARGET 506\n#define MPQ6612A_NANO_SLOT {nano_slot}\n' if nano_slot else ''
            pre=f'#define PWM_AUDIO_AK_CENTRE {align[0]}\n#define PWM_AUDIO_AK_HREN {align[1]}\n' if align else ''
            if align and len(align)>2:
                pre+=f'#define PWM_AUDIO_AK_INTERP {align[2]}\n'+(f'#define PWM_AUDIO_AK_NS2 {align[3]}\n' if len(align)>3 else '')
            harness=pre+nano+f'#define TEST_CARRIER_FACTOR {factor}\n#define APP_PWM_OUTPUT {output}\n#define MPQ6612A_ENABLE_MIKROB {b}\n#define MPQ6612A_ENABLE_XPRO1 {xpro}\n#define MPQ6612A_STEREO {int(stereo)}\n#define PWM_AUDIO_SCOPE_ONLY {scope}\n'+MOCK+'\n#include "nora_pwm_audio_backend.h"\n'+source+'\n'+TEST
            (case/'test.c').write_text(harness)
            cmd=[str(CC),'-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I',str(case),'-I',str(ROOT/'src/app/board/audio'),'-I',str(HAL),str(case/'test.c'),str(HAL/'nora_pwm_audio.c'),str(HAL/'nora_pwm_audio_modulator.c'),'-o',str(case/'t.exe')]
            r=subprocess.run(cmd,capture_output=True,text=True)
            if r.returncode:raise RuntimeError(r.stdout+r.stderr)
            r=subprocess.run([str(case/'t.exe')],capture_output=True,text=True)
            if r.returncode:raise RuntimeError(r.stdout+r.stderr+str(r.returncode))
            print(r.stdout.strip())
if __name__=='__main__':main()
