// Sonora board button and LED device support.
#ifndef BUTTON_LED_H
#define BUTTON_LED_H

//===========================================================
// INCLUDES
//===========================================================
#include <stdbool.h>
#include <stdint.h>


//===========================================================
// Definition
//===========================================================

#ifndef BUTTON_LONG_PRESS_MS
#define BUTTON_LONG_PRESS_MS    (1000UL)
#endif

//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_LONG_PRESS_REACHED,
    BUTTON_EVENT_LONG_PRESSED,
} BUTTON_EVENT_t;


//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

extern bool           BUTTON_Init( void );   /* false if any pin config failed */
extern bool           BUTTON_IsPressed( uint8_t id );
extern BUTTON_EVENT_t BUTTON_GetEvent( uint8_t id );

extern bool           TOUCH_IsPressed( uint8_t id );
extern BUTTON_EVENT_t TOUCH_GetEvent( uint8_t id );

extern bool   LEDs_Init( void );   /* false if any pin config failed */
extern void   LED_On( uint8_t led );
extern void   LED_Off( uint8_t led );
extern void   LED_Toggle( uint8_t led );

extern void   LED_Set_Mask( uint8_t led );

// LED0 liveness heartbeat: call once per foreground iteration, unconditionally.
// Self-pacing against GetTicks() (1 ms tick), so the call rate does not matter as
// long as it is faster than the half-period; it toggles LED0 every 1 s, i.e. a 2 s
// cycle. Requires LEDs_Init() to have run.
//
// A no-op unless APP_USE_LED_HEARTBEAT is 1 -- which is only the AK506 Curiosity
// Nano, where LED0 is the board's one LED and nothing else claims it. On the
// AK512/AK128 DIM boards LED0 is the low bit of the level meter, so the call
// compiles away rather than fighting LED_Set_Mask() for the pin.
//
// Intended to grow into a two-state indicator: fast, HDD-access-lamp style blink
// while the DRC gain detector is working, falling back to this slow heartbeat when
// it is idle. See demo_output_activity_update() in the CK HAL lab's dm330030
// profile for the shape that was liked (an activity flag with a short hold tail so
// brief transients still register visibly).
extern void   LED_heartbeat_tick( void );

// Boot-fault indicator: show an error code on the LED bank and never return.
//   LED0        : heartbeat -- toggled forever so a lit-but-frozen board is
//                 distinguishable from a live one.
//   LED1..LEDn  : static binary encoding of `code` (bit0 -> LED1, bit1 -> LED2, ...).
// Self-contained and dependency-free: it configures the LED pins itself
// (idempotent with LEDs_Init) and paces the heartbeat with a busy-wait loop, so
// it is safe to call from the earliest boot faults -- before LEDs_Init(), the
// timers, or even a confirmed system clock (at a clock fault the heartbeat rate
// is only approximate, which is fine for a visual fault signal).
// On a board with no LEDs (LED_COUNT == 0) it degrades to an idle spin.
extern void   LED_fault_indicate_forever( uint8_t code );



#endif	//!_BUTTON_LED_H

