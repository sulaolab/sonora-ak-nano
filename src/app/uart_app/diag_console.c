#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_console.h"
#include "diag_console.h"
#include "board/devices/wm8904.h"
#include "audio_transport.h"        /* *dl : DSPload measurement window length */
#include "uart_platform_board.h"    /* *du : diagnostic U1TX physical-route control */
#include "timer_app.h"              /* *du : GetTicks() for the auto-restore deadline */

//===========================================================
// diag_console.c
//
// Common diagnostics console module 'd'. Low-level register/clock/perf dumps, plus the few
// settings that only exist to serve a measurement.
//===========================================================


// *dl<hex> / ?dl : DSPload measurement window length in microseconds.
//
// It is a SETTING, not a dump, so this module has to accept kind '*' -- hence the per-command
// kind check below rather than the blanket "queries only" rejection this module used to open with.
// The window is what makes the DSPload line readable at more than one time scale, and nothing
// about it may be hardcoded, so it must be reachable without a rebuild.
static void diag_console_dsploadprof_window( app_console_msg_t* msg )
{
    if( msg->kind == '?' )
    {
        printf( " ?dl window=%luus\n",
                (unsigned long)audio_transport_dbg_get_dsploadprof_window_us() );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        return;
    }

    /* Accept a 16-bit or a 32-bit big-endian hex payload: 2 bytes covers 1 us .. 65.535 ms, which
     * is every window worth using at FCY = 100 MHz (a 43 ms window already fills the 32-bit tick
     * counter's half-range), and 4 bytes is there so a deliberately long window is expressible
     * rather than silently truncated. */
    uint32_t us = 0u;

    if( msg->data_len == 2u )
    {
        us = (uint32_t)( ( (uint32_t)msg->data[0] << 8 ) | (uint32_t)msg->data[1] );
    }
    else if( msg->data_len == 4u )
    {
        us = ( (uint32_t)msg->data[0] << 24 ) | ( (uint32_t)msg->data[1] << 16 ) |
             ( (uint32_t)msg->data[2] << 8  ) |   (uint32_t)msg->data[3];
    }
    else
    {
        printf( " *dl wants a 16- or 32-bit value in us, e.g. *dl2710 (10ms), *dl0000 = default\n" );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
        return;
    }

    us = audio_transport_dbg_set_dsploadprof_window_us( us );

    printf( " *dl window=%luus\n", (unsigned long)us );
    msg->data_len = 0u;
    msg->status   = APP_CONSOLE_OK;
}


//===========================================================
// *du / ?du : diagnostic U1TX PHYSICAL-ROUTE control.
//
// The control experiment for a click that is synchronised to the periodic console report: keep
// the UART peripheral, the baud rate, the printf content and the blocking-TX CPU cost EXACTLY as
// they are, and remove only the switching edges from the physical TX trace. What is left is the
// difference between "the print costs CPU" and "the TX pin disturbs the TDM lines". See
// uart_platform_board.h for why detaching the pad is a stronger control than a second pin.
//
//   *du00        detach U1TX from its pad; console TX goes silent, RX still works
//   *du00<SS>    same, and restore automatically after 0xSS seconds (1..255)
//   *du01        restore U1TX to the board default pin now
//   ?du          which pad carries U1TX, the board default, and any pending auto-restore
//
// The auto-restore exists so the pin cannot be left detached by a lost session or a mistyped
// restore: with TX dead there is no reply to read, so the operator is working blind and the
// image must bring itself back. It is serviced from diag_console_route_tick() in the main loop,
// NOT by blocking here -- a handler that slept through the observation would also stop the
// periodic printf, which is the stimulus under test.
//
//===========================================================
static bool     s_du_restore_armed;
static uint32_t s_du_restore_at_ms;

void diag_console_route_tick( void )
{
    if( !s_du_restore_armed )
    {
        return;
    }
    if( (int32_t)( GetTicks() - s_du_restore_at_ms ) < 0 )
    {
        return;
    }

    s_du_restore_armed = false;
    if( uart_platform_board_uart1_tx_set_rp(
            uart_platform_board_uart1_tx_default_rp() ) )
    {
        /* First thing the operator sees when the console comes back, so it says what happened
         * rather than resuming mid-telemetry as if nothing had. */
        printf("\n *du U1TX route restored automatically (RP%u)\n",
               (unsigned)uart_platform_board_uart1_tx_default_rp() );
    }
}

static void diag_console_uart1_tx_route( app_console_msg_t* msg )
{
    const nora_gpio_rp_t def = uart_platform_board_uart1_tx_default_rp();
    nora_gpio_rp_t       cur = UART_PLATFORM_UART1_TX_RP_DETACHED;

    (void)uart_platform_board_uart1_tx_get_rp( &cur );

    if( msg->kind == '?' )
    {
        printf( " ?du U1TX pad=" );
        if( cur == UART_PLATFORM_UART1_TX_RP_DETACHED ) { printf( "DETACHED" ); }
        else                                            { printf( "RP%u", (unsigned)cur ); }
        printf( " default=RP%u auto_restore=", (unsigned)def );
        if( s_du_restore_armed )
        {
            const int32_t left = (int32_t)( s_du_restore_at_ms - GetTicks() );
            printf( "%ldms\n", (long)( ( left > 0 ) ? left : 0 ) );
        }
        else
        {
            printf( "off\n" );
        }
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        return;
    }

    if( ( msg->data_len < 1u ) || ( msg->data_len > 2u ) )
    {
        printf( " *du00 = detach U1TX pad, *du00<SS> = detach + auto-restore after 0xSS s,"
                " *du01 = restore now (default RP%u)\n", (unsigned)def );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
        return;
    }

    if( msg->data[0] == 0x01u )
    {
        const bool ok = uart_platform_board_uart1_tx_set_rp( def );

        s_du_restore_armed = false;
        printf( " *du U1TX -> RP%u %s\n", (unsigned)def, ok ? "restored" : "REFUSED" );
        msg->data_len = 0u;
        msg->status   = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }

    if( msg->data[0] != 0x00u )
    {
        printf( " *du takes 00 (detach) or 01 (restore); a different pin is a hardware"
                " decision, not a console one\n" );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    /*
     * Announce BEFORE detaching, and only then arm: this is the last text that reaches the PC,
     * so it has to carry the way back. Printing it after the detach would send it into a dead
     * pin and leave the operator with a silent console and no record of what to type.
     */
    {
        const uint8_t secs = ( msg->data_len == 2u ) ? msg->data[1] : 0u;

        if( secs != 0u )
        {
            printf( " *du detaching U1TX from RP%u for %us (auto-restore); \"*du01\" restores"
                    " early -- RX still works, replies will not be visible\n",
                    (unsigned)def, (unsigned)secs );
        }
        else
        {
            printf( " *du detaching U1TX from RP%u; console TX goes silent until \"*du01\""
                    " -- RX still works, replies will not be visible\n", (unsigned)def );
        }

        /* Let the announcement drain before the pad goes away. Blocking TX means the last
         * character is already on the wire when printf returns, so no extra wait is needed --
         * but the console's own "$00du" response line is emitted by the parser AFTER this
         * handler returns, and that one is deliberately sacrificed: waiting for it here would
         * mean detaching from outside the handler. */
        const bool ok =
            uart_platform_board_uart1_tx_set_rp( UART_PLATFORM_UART1_TX_RP_DETACHED );

        if( ok && ( secs != 0u ) )
        {
            s_du_restore_armed = true;
            s_du_restore_at_ms = GetTicks() + ( (uint32_t)secs * 1000u );
        }
        if( !ok )
        {
            printf( " *du detach REFUSED (no PPS output register for RP%u on this device)\n",
                    (unsigned)def );
        }
        msg->data_len = 0u;
        msg->status   = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
    }
}


void diag_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    if( ( msg->kind != '?' ) && ( msg->kind != '*' ) )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch( msg->name )
    {
    case 'l':   // *dl<us> / ?dl : DSPload measurement window length
        diag_console_dsploadprof_window( msg );
        break;

    case 'u':   // *du00 / *du00<SS> / *du01 / ?du : diagnostic U1TX physical-route control
        diag_console_uart1_tx_route( msg );
        break;

    case 'r':   // ?dr : codec (WM8904) register dump (was ?ntCD); data[0] = codec instance
        if( msg->kind != '?' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        wm8904_dump_reg( msg->data[0] );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
