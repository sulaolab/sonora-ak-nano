#ifndef SONORA_DIAG_CONSOLE_H
#define SONORA_DIAG_CONSOLE_H

#include "app_console.h"

// Common "diagnostics" console module (module 'd'). Low-level register/clock/perf dumps that do
// not belong to system, transport, or any application -- read-only.
//   ?dr : codec (WM8904) register dump (was ?ntCD); data[0] = codec instance
//   *dl / ?dl : DSPload measurement window length
//   *du / ?du : diagnostic U1TX physical-route control (detach / restore)
void diag_console_onmsg( app_console_msg_t* msg );

// Service the *du auto-restore deadline. Call every main-loop iteration; a no-op unless a
// timed U1TX detach is armed. It lives outside diag_console_onmsg() on purpose: the detach is
// meant to run WHILE the periodic report keeps printing, so the handler must not block through
// the observation window (the report is the stimulus under test).
void diag_console_route_tick( void );

#endif /* SONORA_DIAG_CONSOLE_H */
