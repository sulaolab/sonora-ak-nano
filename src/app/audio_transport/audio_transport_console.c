#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "app_console.h"
#include "audio_transport_console.h"
#include "audio_transport.h"   // audio_transport_restart(), audio_transport_frmerr_force_trip()
#include "audio_transport_snapshot.h"   // ?te : per-leg harm counters
#include "timer_app.h"                  // ?te : GetTicks() for the observation-window length

//===========================================================
// audio_transport_console.c
//
// Common transport console module 't'. App-agnostic: does not include any application
// private header and does not branch on application identity.
//===========================================================

//===========================================================
// "?te" / "*te" -- the framed-transport HARM counters, on demand.
//
// Why a console verb and not more periodic output: the fault this exists for (an audible click
// exactly on the periodic report) is SYNCHRONISED TO THE PRINT, so the periodic line is the
// stimulus and every byte added to it changes the experiment. The periodic line therefore carries
// only the 10-byte rtf= field; the full picture, with a delta and the window it was measured
// over, is pulled here after the observation instead of streamed during it.
//
// ?te prints, per built leg: the ABSOLUTE counts since start() and the DELTA since the previous
// ?te / *te on this image, with the elapsed time of that window. The delta is what answers "did
// anything step while I was listening"; the absolute is what survives a missed reading.
//
// *te zeroes rov/tur/frm (and the consecutive-FRMERR run) and re-bases the delta. It cannot zero
// blk or miss -- see audio_transport_clear_error_counts().
//
//===========================================================
static struct {
    bool     valid;
    uint32_t at_ms;
    uint32_t blk[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
    uint32_t miss[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
    uint32_t rov[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
    uint32_t tur[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
    uint32_t frm[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
} s_te_base;

static void audio_transport_console_errcnt_rebase(
    const audio_transport_snapshot_t* snap )
{
    uint8_t i;

    s_te_base.valid = true;
    s_te_base.at_ms = GetTicks();
    for( i = 0u; i < (uint8_t)AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS; i++ )
    {
        const audio_transport_leg_snapshot_t* leg = &snap->legs[i];

        s_te_base.blk[i]  = leg->block_count;
        s_te_base.miss[i] = leg->deadline_miss_count;
        s_te_base.rov[i]  = leg->rx_overrun_block_count;
        s_te_base.tur[i]  = leg->tx_underrun_block_count;
        s_te_base.frm[i]  = leg->frame_error_block_count;
    }
}

static void audio_transport_console_errcnt( app_console_msg_t* msg )
{
    audio_transport_snapshot_t snap;

    if( msg->data_len != 0u )
    {
        printf(" \"*te/?te\" takes no value\n");
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
        return;
    }
    if( ( msg->kind != '?' ) && ( msg->kind != '*' ) )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    /* Read WITHOUT clearing the ISR load peaks: the periodic report owns that window, and a
     * console reader that silently consumed the peak would leave a hole in the resp=/margin=
     * series it is being compared against. */
    if( !audio_transport_snapshot_get( &snap ) )
    {
        printf(" \"%cte\" transport snapshot unavailable\n", (char)msg->kind );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }

    if( msg->kind == '*' )
    {
        const bool cleared = audio_transport_clear_error_counts();

        /* Re-base the delta from the PRE-clear snapshot: the counters are about to read zero, so
         * a base of zero and a next reading of zero would report "no change" either way. Basing
         * on what was there keeps the first ?te after a *te an honest "0 since the clear". */
        audio_transport_console_errcnt_rebase( &snap );
        if( cleared )
        {
            /* Only rov/tur/frm were zeroed in the HAL, so only THEIR bases go to zero: the next
             * ?te then reads them as "since the clear", while blk/miss keep the pre-clear base
             * and read as "since the *te". Both are deltas over the same window. */
            uint8_t i;
            for( i = 0u; i < (uint8_t)AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS; i++ )
            {
                s_te_base.rov[i] = 0u;
                s_te_base.tur[i] = 0u;
                s_te_base.frm[i] = 0u;
            }
        }
        printf(" \"*te\" rov/tur/frm cleared on all legs: %s"
               " (blk/miss/load peaks untouched by design)\n",
               cleared ? "ok" : "PARTIAL -- a leg refused" );
        msg->data_len = 0u;
        msg->status   = cleared ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        return;
    }

    {
        const uint32_t now_ms = GetTicks();
        const uint32_t dt_ms  = s_te_base.valid
            ? (uint32_t)( now_ms - s_te_base.at_ms ) : 0u;
        uint8_t i;

        printf(" ?te harm counters, absolute since start() | d = delta over the last %lums%s\n",
               (unsigned long)dt_ms,
               s_te_base.valid ? "" : " (no previous read -- delta is from boot)" );

        for( i = 0u; i < snap.leg_count; i++ )
        {
            const audio_transport_leg_snapshot_t* leg = &snap.legs[i];

            if( !leg->present ) { continue; }

            printf("  leg%u spi%u run=%d blk=%lu miss=%lu rov=%lu tur=%lu frm=%lu\n",
                   (unsigned)i,
                   (unsigned)leg->physical_spi_instance,
                   (int)leg->running,
                   (unsigned long)leg->block_count,
                   (unsigned long)leg->deadline_miss_count,
                   (unsigned long)leg->rx_overrun_block_count,
                   (unsigned long)leg->tx_underrun_block_count,
                   (unsigned long)leg->frame_error_block_count);
            printf("  leg%u d(blk,miss,rov,tur,frm)=(%lu,%lu,%lu,%lu,%lu)\n",
                   (unsigned)i,
                   (unsigned long)( leg->block_count             - s_te_base.blk[i]  ),
                   (unsigned long)( leg->deadline_miss_count     - s_te_base.miss[i] ),
                   (unsigned long)( leg->rx_overrun_block_count  - s_te_base.rov[i]  ),
                   (unsigned long)( leg->tx_underrun_block_count - s_te_base.tur[i]  ),
                   (unsigned long)( leg->frame_error_block_count - s_te_base.frm[i]  ));
        }

        audio_transport_console_errcnt_rebase( &snap );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
    }
}


void audio_transport_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    switch( msg->name )
    {
    case 'e':   // ?te : per-leg harm counters (absolute + delta since the previous read)
                // *te : clear rov/tur/frm on every leg and re-base the delta
        audio_transport_console_errcnt( msg );
        break;

    case 's':   // *ts : verified analog mute + terminal stop before flash/reset
                // ?ts : report the result of the last *ts
        if( msg->data_len != 0u )
        {
            printf(" \"*ts/?ts\" takes no value\n");
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        if( msg->kind == '?' )
        {
            audio_transport_stop_for_flash_report();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        msg->data_len = 0u;
        msg->status = audio_transport_stop_for_flash()
            ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'r':   // *tr : mute-bounded same-rate restart (was *nt03)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            printf(" \"*tr\" force audio stop/restart (same rate)\n");
            const bool restarted = audio_transport_restart();
            msg->data_len = 0u;
            msg->status   = restarted
                ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        break;

    case 'd':   // *td<NN> : declick research one-shot restart with strategy bitmask NN (HEX byte)
                // ?td      : print the bitmask legend + captured-servo status
        if( msg->kind == '?' )
        {
            printf(" \"?td\" declick restart strategy bitmask (one-shot, *td<NN>):\n");
            // The legend belongs to the driver's build policy, not to this console: when the
            // strategies are compiled out this prints one line saying so instead of a menu.
            audio_transport_declick_print_help();
            audio_transport_declick_print_status();   // per-codec captured-servo status (WARM_SERVO)
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            // Default mask 0 (baseline) when no payload byte was supplied, so "*td" == "*tr".
            const uint8_t mask = ( msg->data_len >= 1u ) ? msg->data[0] : 0x00u;
            if( ( mask != 0x00u ) && !audio_transport_declick_research_available() )
            {
                // Refuse rather than silently run the default: the strategies are compiled out.
                printf(" \"*td\" mask=0x%02x not available -- declick research compiled out;"
                       " use \"*td00\" or \"*tr\"\n", (unsigned)mask );
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
                break;
            }
            printf(" \"*td\" declick one-shot restart mask=0x%02x\n", (unsigned)mask );
            const bool restarted = audio_transport_restart_declick( mask );
            msg->data_len = 0u;
            msg->status   = restarted
                ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        break;

    case 'q':   // Telemetry control. Payload = mode word (2 bytes), optional period word (2 bytes):
                //   *tq0000        -> OFF
                //   *tq0001        -> ON  (resolved default period)
                //   *tq0002 XXXX   -> ON  with period = 0xXXXX ms  (e.g. *tq00020BB8 = 3000ms)
                // Short forms map by value: *tq00 -> OFF, *tq01 -> ON. No payload -> OFF.
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            const uint16_t mode = ( msg->data_len >= 2u )
                                  ? (uint16_t)( ( (uint16_t)msg->data[0] << 8 ) | msg->data[1] )
                                  : ( ( msg->data_len == 1u ) ? msg->data[0] : 0u );
            if( mode == 2u )
            {
                const uint16_t ms = ( msg->data_len >= 4u )
                                    ? (uint16_t)( ( (uint16_t)msg->data[2] << 8 ) | msg->data[3] )
                                    : 0u;
                audio_transport_set_dbg_period_ms( (uint32_t)ms );   // 0 also disables
                printf(" \"*tq\" telemetry %s period=%ums\n", ( ms == 0u ) ? "OFF" : "ON", (unsigned)ms );
            }
            else
            {
                const bool on = ( mode != 0u );
                audio_transport_dbg_enable( on );                    // ON = resolved default period
                printf(" \"*tq\" telemetry %s\n", on ? "ON" : "OFF" );
            }
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;

#if APP_PRINTF_CLICK_DIAG
    case 'j':   // ?tj      : block-ISR entry-to-entry interval (bucketed by "was a report running"),
                //            report duration, main-loop worst gap, and the two interrupt-blind
                //            windows the report opens. NOTHING here is added to the periodic line:
                //            the report is the stimulus, so its byte count is part of the
                //            experiment.
                // *tj      : clear every accumulator (the late threshold is kept)
                // *tj<XXXX>: clear, and arm late-entry counting at 0xXXXX microseconds
        if( msg->kind == '?' )
        {
            if( msg->data_len != 0u )
            {
                printf(" \"?tj\" takes no value\n");
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
                break;
            }
            audio_transport_click_diag_print();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            /* A threshold is set only when one was actually given. The brief forbids inventing one
             * before the quiet-board jitter has been read, so the default stays DISABLED and the
             * operator arms it from the min/max ?tj already printed. */
            const uint32_t us = ( msg->data_len >= 2u )
                ? (uint32_t)( ( (uint32_t)msg->data[0] << 8 ) | (uint32_t)msg->data[1] )
                : ( ( msg->data_len == 1u ) ? (uint32_t)msg->data[0] : 0u );

            audio_transport_click_diag_clear();
            if( msg->data_len != 0u )
            {
                audio_transport_click_diag_set_late_threshold_us( us );
                printf(" \"*tj\" cleared; late-entry threshold = %luus\n", (unsigned long)us );
            }
            else
            {
                printf(" \"*tj\" cleared (late-entry threshold unchanged)\n");
            }
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;

    case 'm':   // *tm000<n> : select the periodic-report STIMULUS, so one flashed image can present
                //             every control condition to a listener:
                //               0 OFF  1 NORMAL  2 STALL_ONLY  3 FORMAT_ONLY  4 TX_ONLY  5 SNAP_ONLY
                //             *tm0002<MMMM> additionally fixes the STALL_ONLY length to 0xMMMM ms
                //             (0 = follow the measured report duration).
                // ?tm       : which stimulus is armed, and the stall length it would use
        if( msg->kind == '?' )
        {
            if( msg->data_len != 0u )
            {
                printf(" \"?tm\" takes no value\n");
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
                break;
            }
            audio_transport_click_diag_print_stimulus();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        if( msg->data_len < 1u )
        {
            printf(" \"*tm\" needs a stimulus id -- see ?tm\n");
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        {
            /* Same 2-byte-word payload shape as *tq, so the two read alike at the call site. */
            const uint16_t stim = ( msg->data_len >= 2u )
                ? (uint16_t)( ( (uint16_t)msg->data[0] << 8 ) | msg->data[1] )
                : (uint16_t)msg->data[0];
            const uint32_t stall_ms = ( msg->data_len >= 4u )
                ? (uint32_t)( ( (uint32_t)msg->data[2] << 8 ) | (uint32_t)msg->data[3] )
                : 0u;

            if( ( stim > 0xFFu ) ||
                !audio_transport_click_diag_set_stimulus( (uint8_t)stim, stall_ms ) )
            {
                printf(" \"*tm\" unknown stimulus %u -- see ?tm\n", (unsigned)stim );
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
                break;
            }
            audio_transport_click_diag_print_stimulus();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;
#endif // APP_PRINTF_CLICK_DIAG

    case 'f':   // *tf : TDM frame-slip force-trip, arms one recovery episode (was *nt43)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            // audio_transport_frmerr_force_trip() is a safe no-op when auto-recovery is not
            // built in, so calling it needs no gate -- but REPORTING it does. Announcing
            // "armed, fires on the next recover tick" and returning OK on a profile that
            // compiled the recovery path out describes something that will never happen, and
            // the resulting silence reads as a broken co-clocked build. UNSUPPORTED, matching
            // *cy's "known thing, not built into this target" convention, says which it is.
            if( !audio_transport_frmerr_autorecovery_available() )
            {
                printf(" \"*tf\" TDM frame-slip auto-recovery is not built into this"
                       " configuration (it needs the independent dual-clock-domain topology"
                       " with edge capture on both legs); nothing to force\n");
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
                break;
            }
            audio_transport_frmerr_force_trip();
            printf(" \"*tf\" TDM frame-slip force-trip armed (restart-until-healthy fires on next recover tick)\n");
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
