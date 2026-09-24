
//===========================================================
// INCLUDES
//===========================================================
#include "resolved_board_config.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "hal_touch/nora_touch.h"

#include "nora_gpio.h"   /* RP-first GPIO API (LEDs/buttons are RP pins) */
#include "timer_app.h"

#include "board/devices/button_led.h"


//===========================================================
// Definition
//===========================================================

#if !defined(BUTTON_LONG_PRESS_MS)
// #define BUTTON_LONG_PRESS_MS     (1000UL)
 #define BUTTON_LONG_PRESS_MS     (300UL)
#endif //!defined(BUTTON_LONG_PRESS_MS)

#if !defined(BUTTON_DEBOUNCE_MS)
// Mechanical-switch contact settle time. A raw level must hold this long before
// the edge state machine accepts it, so contact bounce on a quick/sloppy press
// no longer produces spurious release->press->release events.
 #define BUTTON_DEBOUNCE_MS       (15UL)
#endif //!defined(BUTTON_DEBOUNCE_MS)


//===========================================================
// Board pin map (the only board-specific GPIO knowledge here)
//===========================================================
//
// LEDs and buttons are normal board GPIO with RP numbers, so they are addressed
// by RP (the preferred interface) and driven through the RP-first GPIO HAL. RP
// values are the pin's RPn (= GPIO packed pin + 1); the port/bit name is kept in
// the comment for readability.
//
//   LED polarity is a per-board fact -- see LED_ACTIVE_LOW in each arm.
//   Buttons are active-low (pressed == pin reads low).
//
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE

  #define LED_COUNT      (8u)
  #define LED_ACTIVE_LOW (0)
  #define BUTTON_COUNT   (3u)

  static const nora_gpio_rp_t s_led_rp[LED_COUNT] =
  {
      36u,   // LED0  RP36/RC3
      37u,   // LED1  RP37/RC4
      38u,   // LED2  RP38/RC5
      39u,   // LED3  RP39/RC6
      40u,   // LED4  RP40/RC7
      41u,   // LED5  RP41/RC8
      42u,   // LED6  RP42/RC9
      43u,   // LED7  RP43/RC10
  };

  // index 0 unused (button id is 1..BUTTON_COUNT)
  static const nora_gpio_rp_t s_button_rp[1u + BUTTON_COUNT] =
  {
      0u,
      22u,   // button 1  RP22/RB5
      21u,   // button 2  RP21/RB4
      7u,    // button 3  RP7/RA6
  };

#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE

  #define LED_COUNT      (8u)
  #define LED_ACTIVE_LOW (0)
  #define BUTTON_COUNT   (3u)

  static const nora_gpio_rp_t s_led_rp[LED_COUNT] =
  {
      41u,   // LED0  RP41/RC8
      42u,   // LED1  RP42/RC9
      43u,   // LED2  RP43/RC10
      44u,   // LED3  RP44/RC11
      45u,   // LED4  RP45/RC12
      46u,   // LED5  RP46/RC13
      47u,   // LED6  RP47/RC14
      48u,   // LED7  RP48/RC15
  };

  // index 0 unused (button id is 1..BUTTON_COUNT)
  static const nora_gpio_rp_t s_button_rp[1u + BUTTON_COUNT] =
  {
      0u,
      84u,   // button 1  RP84/RF3
      81u,   // button 2  RP81/RF0
      19u,   // button 3  RP19/RB2
  };

#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE

  // dsPIC33AK512MPS506 Curiosity Nano (EV17P63A), User Guide DS70005634A Table 4-4.
  //
  // ONE LED, AND IT IS ACTIVE-LOW. The board wires the LED anode to VDD, so the
  // pin sinks the current: "Driving the connected I/O line to GND can also
  // activate the LED" (UG section 4.2.1). Every other board in this repo is
  // active-high, which is why LED_ACTIVE_LOW exists at all -- an active-high
  // LEDs_Init() on this board seeds LAT Low and leaves the LED permanently ON,
  // i.e. indistinguishable from a hung image.
  //
  // NO BUTTONS DECLARED, deliberately. SW0 is on RC3, which the Nano also uses as
  // the debugger's DBG2 line, and it has no external pull-up (an internal one is
  // mandatory). Claiming it here would have BUTTON_Init() reconfigure a debug pin
  // on every boot for a button nothing currently reads. Add it when a caller wants
  // it, with pull=UP -- not before.
  #define LED_COUNT      (1u)
  #define LED_ACTIVE_LOW (1)
  #define BUTTON_COUNT   (0u)

  static const nora_gpio_rp_t s_led_rp[LED_COUNT] =
  {
      49u,   // LED0  RP49/RD0  (active-LOW)
  };

  static const nora_gpio_rp_t s_button_rp[1] = { 0u };   // unused

#else

  #define LED_COUNT      (0u)
  #define LED_ACTIVE_LOW (0)
  #define BUTTON_COUNT   (0u)

  static const nora_gpio_rp_t s_led_rp[1]    = { 0u };   // unused
  static const nora_gpio_rp_t s_button_rp[1] = { 0u };   // unused

#endif // RESOLVED_BOARD_TARGET

// Physical pin level that lights / extinguishes an LED on this board. Everything
// below is written in terms of these two, never in terms of set/clear, so an
// active-low board needs no special case anywhere else in the file.
#if LED_ACTIVE_LOW
  #define LED_LEVEL_ON   (false)
  #define LED_LEVEL_OFF  (true)
#else
  #define LED_LEVEL_ON   (true)
  #define LED_LEVEL_OFF  (false)
#endif

// Heartbeat half-period: 1 s per state -> a 2 s full cycle, the "slow-ish" rate
// asked for. Slow on purpose -- it is a liveness sign, not an activity indicator,
// and a fast blink here would leave no visual room for the activity cue that is
// meant to go on top of it later (fast, HDD-access-lamp style, while the DRC gain
// detector is working; back to this heartbeat when it is idle).
#define LED_HEARTBEAT_HALF_PERIOD_MS   (1000UL)

// Resync threshold. The deadline normally ADVANCES by the period rather than being
// resampled, so the heartbeat is an honest measure of the 1 ms tick (a wrong system
// clock shows up as a wrong blink rate -- the same trick the CK lab profile uses).
// But a foreground iteration that overruns by seconds -- a long console dump, a
// blocking bring-up -- would then be paid back as a burst of fast toggles, which
// reads as the activity cue. Past this much lateness the deadline is resampled and
// the missed beats are dropped instead.
#define LED_HEARTBEAT_RESYNC_MS        (LED_HEARTBEAT_HALF_PERIOD_MS * 4UL)


//===========================================================
// Function Prototype
//===========================================================

//===========================================================
// Variables
//===========================================================




//===========================================================
// Global Function
//===========================================================

bool BUTTON_Init(void)
{
    bool ok = true;
    for( uint8_t id = 1u; id <= BUTTON_COUNT; id++ )
    {
        // Digital input, no pull (external pull assumed by the board).
        ok = nora_gpio_rp_config_digital_input( s_button_rp[id] ) && ok;
    }
    return ok;
}


// id: button = 1, 2, 3
bool BUTTON_IsPressed( uint8_t id )
{
    if( (id < 1u) || (id > BUTTON_COUNT) )
    {
        return false;
    }
    // Active low: pressed when the pin reads Low. A read ERROR (-1) or High is
    // treated as not-pressed (the 3-state level must not be used as a plain bool).
    return (nora_gpio_rp_read( s_button_rp[id] ) == NORA_GPIO_LEVEL_LOW);
}


BUTTON_EVENT_t BUTTON_GetEvent( uint8_t id )
{
    static bool     previous_state[4]        = {false/*dummy*/, false, false, false}; // not used of id=0
    static bool     long_press_reached[4]    = {false/*dummy*/, false, false, false}; // not used of id=0
    static uint32_t press_start_tick[4]      = {0, 0, 0, 0};

    // Per-id debounce (stable-confirm): the raw level must hold for
    // BUTTON_DEBOUNCE_MS before it is accepted as the current level.
    static bool     db_level[4]              = {false, false, false, false}; // confirmed level
    static bool     db_cand[4]               = {false, false, false, false}; // candidate raw level
    static uint32_t db_cand_tick[4]          = {0, 0, 0, 0};                  // when candidate first seen

    if( (id == 0)||(id >= 4) )  return BUTTON_EVENT_NONE;

    uint32_t now = GetTicks();
    bool     raw = BUTTON_IsPressed(id);
    if( raw != db_cand[id] )
    {
        db_cand[id]      = raw;
        db_cand_tick[id] = now;   // raw changed -> restart settle window
    }
    else if( (uint32_t)(now - db_cand_tick[id]) >= BUTTON_DEBOUNCE_MS )
    {
        db_level[id]     = raw;   // stable long enough -> accept
    }
    bool current = db_level[id];  // feed edge logic with the debounced level

    if( !previous_state[id] && current )
    {
        press_start_tick[id]   = now;
        long_press_reached[id] = false;
        previous_state[id]     = current;
        return BUTTON_EVENT_PRESSED;
    }

    if( previous_state[id] && current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);

        if( !long_press_reached[id] && (pressed_time >= BUTTON_LONG_PRESS_MS) )
        {
            long_press_reached[id] = true;
            previous_state[id]     = current;
            return BUTTON_EVENT_LONG_PRESS_REACHED;
        }
    }

    if( previous_state[id] && !current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);
        bool     long_pressed = (pressed_time >= BUTTON_LONG_PRESS_MS);

        long_press_reached[id] = false;
        previous_state[id]     = current;

        if( long_pressed )
        {
            return BUTTON_EVENT_LONG_PRESSED;
        }
        return BUTTON_EVENT_RELEASED;
    }

    previous_state[id] = current;

    return BUTTON_EVENT_NONE;
}




// id: touch = 1, 2, 3
bool TOUCH_IsPressed( uint8_t id )
{
    switch( id )
    {
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE

#elif (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE) && defined(ENA_OPEN_TOUCH_EXCLUSIVE)
    /* Same three pads, open library instead of the vendor one. Sourcing them
     * here rather than letting the application talk to nora_touch directly is
     * what keeps the two builds comparable: TOUCH_GetEvent() below, and every
     * consumer above it, is then identical code in both images, so a difference
     * in behaviour is a difference between the touch libraries and not between
     * two event layers. It is also the answer to "who owns long press" — this
     * layer already does, for buttons and for the vendor build. */
    case 1:
        return nora_touch_is_pressed(0);
    case 2:
        return nora_touch_is_pressed(1);
    case 3:
        return nora_touch_is_pressed(2);

#else
#endif // RESOLVED_BOARD_TARGET
    default:
        break;
    }
    return false;
}


/* Console trace of the touch events this layer produces, off unless the build
 * asks for it with -Define ENA_TOUCH_EVENT_TRACE=1.
 *
 * It sits here, above TOUCH_IsPressed()'s #if, on purpose: this is the one place
 * both the vendor build and the open build pass through, so the same code prints
 * the same line shape either way and the behavioural comparison in the tuning
 * manual's appendix A can be scored by counting lines instead of by watching
 * LEDs. Scoring it from each library's own logging would compare the logging.
 *
 * The vendor build is untouched by this — nothing here reads its source, and the
 * trace only observes the boolean TOUCH_IsPressed() already returned. */
#if defined(ENA_TOUCH_EVENT_TRACE)
static BUTTON_EVENT_t touch_trace( uint8_t id, BUTTON_EVENT_t ev )
{
    switch( ev )
    {
    case BUTTON_EVENT_PRESSED:            printf(" TOUCH_EV(%u): press\n",      (unsigned)id ); break;
    case BUTTON_EVENT_RELEASED:           printf(" TOUCH_EV(%u): release\n",    (unsigned)id ); break;
    case BUTTON_EVENT_LONG_PRESS_REACHED: printf(" TOUCH_EV(%u): long\n",       (unsigned)id ); break;
    case BUTTON_EVENT_LONG_PRESSED:       printf(" TOUCH_EV(%u): release_long\n", (unsigned)id ); break;
    default: break;
    }
    return ev;
}
#else
 #define touch_trace(id, ev)   (ev)
#endif //defined(ENA_TOUCH_EVENT_TRACE)


BUTTON_EVENT_t TOUCH_GetEvent( uint8_t id )
{
    static bool     previous_state[4]        = {false, false, false, false}; // not used of id=0
    static bool     long_press_reached[4]    = {false, false, false, false}; // not used of id=0
    static uint32_t press_start_tick[4]      = {0, 0, 0, 0};

    if( (id == 0)||(id >= 4) )  return BUTTON_EVENT_NONE;

    bool     current = TOUCH_IsPressed(id);
    uint32_t now     = GetTicks();

    if( !previous_state[id] && current )
    {
        press_start_tick[id]   = now;
        long_press_reached[id] = false;
        previous_state[id]     = current;
        return touch_trace(id, BUTTON_EVENT_PRESSED);
    }

    if( previous_state[id] && current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);

        if( !long_press_reached[id] && (pressed_time >= BUTTON_LONG_PRESS_MS) )
        {
            long_press_reached[id] = true;
            previous_state[id]     = current;
            return touch_trace(id, BUTTON_EVENT_LONG_PRESS_REACHED);
        }
    }

    if( previous_state[id] && !current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);
        bool     long_pressed = (pressed_time >= BUTTON_LONG_PRESS_MS);

        long_press_reached[id] = false;
        previous_state[id]     = current;

        if( long_pressed )
        {
            return touch_trace(id, BUTTON_EVENT_LONG_PRESSED);
        }
        return touch_trace(id, BUTTON_EVENT_RELEASED);
    }

    previous_state[id] = current;

    return BUTTON_EVENT_NONE;
}




bool LEDs_Init(void)
{
    // Standard digital outputs. config_digital_output seeds LAT with the OFF
    // level BEFORE enabling the driver, so no LED briefly lights during
    // bring-up. LED_LEVEL_OFF -- not a literal Low -- because the AK506 Nano's
    // single LED is active-low and a Low seed there means "lit from reset".
    bool ok = true;
    for( uint8_t i = 0u; i < LED_COUNT; i++ )
    {
        ok = nora_gpio_rp_config_digital_output( s_led_rp[i], LED_LEVEL_OFF ) && ok;
    }
    return ok;
}

void LED_On( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_write( s_led_rp[led], LED_LEVEL_ON );
    }
    else
    {
        // any out-of-range id (e.g. 0xFF) means "all"
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_write( s_led_rp[i], LED_LEVEL_ON );
        }
    }
}

void LED_Off( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_write( s_led_rp[led], LED_LEVEL_OFF );
    }
    else
    {
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_write( s_led_rp[i], LED_LEVEL_OFF );
        }
    }
}

void LED_Toggle( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_toggle( s_led_rp[led] );
    }
    else
    {
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_toggle( s_led_rp[i] );
        }
    }
}


void LED_Set_Mask( uint8_t led )
{
    // bit i -> LEDi (bit0 = first LED); a set bit means lit, whatever the polarity
    for( uint8_t i = 0u; i < LED_COUNT; i++ )
    {
        (void)nora_gpio_rp_write( s_led_rp[i],
                                  (((led >> i) & 0x01u) != 0u) ? LED_LEVEL_ON
                                                               : LED_LEVEL_OFF );
    }
}


void LED_heartbeat_tick( void )
{
#if !APP_USE_LED_HEARTBEAT || (LED_COUNT == 0u)
    // Nothing to blink, or LED0 belongs to another consumer on this board (the
    // level meter). Compiled to nothing; see APP_USE_LED_HEARTBEAT.
#else
    static uint32_t next_edge_ms;
    static bool     seeded;

    uint32_t now = GetTicks();

    if( !seeded )
    {
        seeded       = true;
        next_edge_ms = now + LED_HEARTBEAT_HALF_PERIOD_MS;
        LED_Off( 0u );
        return;
    }

    // Unsigned wrap-safe lateness. Not yet due -> nothing to do.
    uint32_t late = (uint32_t)(now - next_edge_ms);
    if( late >= 0x80000000UL )   // now is still before the deadline
    {
        return;
    }

    LED_Toggle( 0u );

    if( late >= LED_HEARTBEAT_RESYNC_MS )
    {
        next_edge_ms = now + LED_HEARTBEAT_HALF_PERIOD_MS;   // drop the missed beats
    }
    else
    {
        next_edge_ms += LED_HEARTBEAT_HALF_PERIOD_MS;        // keep the phase honest
    }
#endif // APP_USE_LED_HEARTBEAT && LED_COUNT
}


void LED_fault_indicate_forever( uint8_t code )
{
#if (LED_COUNT == 0u)
    // No LEDs on this board: nothing to show, just halt.
    (void)code;
    for( ;; )
    {
        Nop();
    }
#else
    // Half-period of the LED0 heartbeat, in busy-loop iterations. Deliberately a
    // raw spin count (no timer / clock dependency): at the nominal system clock
    // this is a sub-second blink; at a clock fault the true clock is unknown so
    // the rate only approximates -- acceptable for a visual "alive" indicator.
    static const uint32_t LED_FAULT_HEARTBEAT_LOOPS = 4000000UL;

    // Bring the LED pins up ourselves -- this may run before LEDs_Init().
    // Idempotent: re-seeding the same digital outputs is harmless.
    (void)LEDs_Init();

    // LED1..LEDn: static binary encoding of the fault code (bit0 -> LED1).
    // LED0 is reserved for the heartbeat below.
    for( uint8_t i = 1u; i < LED_COUNT; i++ )
    {
        if( ((code >> (uint8_t)(i - 1u)) & 0x01u) != 0u )
        {
            LED_On( i );
        }
        else
        {
            LED_Off( i );
        }
    }

    // LED0: heartbeat forever.
    for( ;; )
    {
        LED_Toggle( 0u );
        for( volatile uint32_t d = 0u; d < LED_FAULT_HEARTBEAT_LOOPS; d++ )
        {
            Nop();
        }
    }
#endif // LED_COUNT
}




//===========================================================
// Local Function
//===========================================================
