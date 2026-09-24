#ifndef UART_PLATFORM_BOARD_H
#define UART_PLATFORM_BOARD_H

#include <stdbool.h>
#include "app_specific_config_defs.h"
#include "../hal_uart/nora_uart.h"
#include "../hal_gpio/nora_gpio.h"   /* nora_gpio_rp_t -- the U1TX route control below */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sonora board UART platform layer.
 *
 * This is NOT the reusable UART HAL core.  It is the Sonora project-specific
 * layer that holds this board's UART routing (PPS / GPIO idle) and the
 * recommended UART config (baud / clock).  It lives under
 * src/uart_platform/ so that project-specific details stay out of the public
 * driver (src/hal_uart/).
 *
 * Dependency direction (one-way):
 *     uart_platform_board  ->  nora_uart (public HAL API) + xc.h (PPS/GPIO)
 * It must NOT know about printf, app_console, app_uart_process, or the legacy
 * UART1_* internals.
 *
 * The values here are COPIED (not moved) from the legacy src/uart/uart1.c so
 * the legacy driver stays byte-for-byte unchanged.
 */

/*
 * UART1 clock policy/facts for the Sonora board.
 *
 * CLKGEN8 is configured from PLL1 with divide-by-1 by
 * uart_platform_board_apply_pins_and_clock(). Consumers pass the resulting
 * frequency to the UART HAL; they do not maintain another 200 MHz UART fact.
 */
#define UART_PLATFORM_CLKGEN8_DIVIDE_BY     (1u)
#define UART_PLATFORM_CLKGEN8_CLK_HZ        (PLL1_CLK_HZ / UART_PLATFORM_CLKGEN8_DIVIDE_BY)
#define UART_PLATFORM_UART1_CLK_HZ          UART_PLATFORM_CLKGEN8_CLK_HZ

/*
 * Boot-fault console: UART1 running off the reset-default FRC instead of PLL1,
 * for the one case where the clock bring-up itself failed and there is no PLL1
 * to run the normal console from. The CPU is still alive on the FRC there, and
 * CLKGEN source switching still works, so this is reachable.
 *
 * The baud has to drop: from an 8 MHz UART clock, 230400 lands 3.6% off and is
 * unusable, while 19200 lands 0.16% off. So when investigating a boot failure,
 * start the monitor with --baud 19200.
 */
#define UART_PLATFORM_FRC_CLK_HZ            (8000000UL)
#define UART_PLATFORM_BOOT_FAULT_BAUD       (19200u)

typedef struct {
    nora_uart_instance_t inst;
    nora_uart_config_t config;
} uart_platform_board_config_t;

/*
 * Return the default UART config for the Sonora board/project.
 *
 * This is a board/project helper, not a generic UART HAL API.
 * Returns false for instances that the board does not define.
 */
bool uart_platform_board_get_default_config(
    nora_uart_instance_t inst,
    uart_platform_board_config_t *out);

/*
 * Configure CLKGEN8, PPS, GPIO direction, idle state, and UART clock source for
 * the selected UART instance on the Sonora board.
 *
 * This is intentionally separate from nora_uart_init() so that the HAL
 * core remains reusable and board-specific pin routing stays isolated.
 * Returns false for unsupported instances / unknown device.
 */
bool uart_platform_board_apply_pins_and_clock(
    nora_uart_instance_t inst);

/*
 * Same, but source CLKGEN8 from the FRC instead of PLL1 -- for the boot-fault
 * console only, when PLL1 is not available. See UART_PLATFORM_BOOT_FAULT_BAUD.
 */
bool uart_platform_board_apply_pins_and_frc_clock(
    nora_uart_instance_t inst);

/*
 * DIAGNOSTIC U1TX ROUTE CONTROL -- move the console's TRANSMIT PIN at run time without
 * touching the UART peripheral, the baud rate, the printf content or the CPU cost of printing.
 *
 * This exists for one experiment: an audible click that appears exactly when the periodic
 * console report is printed has two equally ranked explanations -- the CPU/real-time cost of
 * the print, and capacitive/inductive crosstalk from the UART TX trace into the nearby TDM
 * clock/data lines. Nothing in software can separate them while both change together, so the
 * control is to keep EVERYTHING the same and remove only the edges on the physical TX trace.
 *
 * "Detach" (rp = 0) is the strongest form of that control and the reason this is not simply a
 * second pin number: with the PPS output code cleared, the pad reverts to the GPIO the routing
 * step configured -- a driven, STATIC HIGH level, which is also the UART idle level -- so there
 * are no UART edges anywhere on the board, not merely on a different trace. The UART itself is
 * untouched: the transmitter still shifts every byte at the same baud and the blocking TX still
 * costs the foreground the same wall time, because the peripheral neither knows nor cares
 * whether its output reaches a pad. The PC simply sees nothing.
 *
 * U1RX is deliberately NOT touched, so the console can still be commanded (and the route
 * restored) while its own replies are invisible.
 *
 * set_rp() accepts the board default (restore) or 0 (detach); any other pin is refused, because
 * choosing a live pin on this board is a hardware decision, not a software one. get_rp() reports
 * what the PPS map actually holds, not what was last asked for.
 *
 */
#define UART_PLATFORM_UART1_TX_RP_DETACHED   ((nora_gpio_rp_t)0u)

nora_gpio_rp_t uart_platform_board_uart1_tx_default_rp(void);
bool uart_platform_board_uart1_tx_get_rp(nora_gpio_rp_t *rp);
bool uart_platform_board_uart1_tx_set_rp(nora_gpio_rp_t rp);

#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_BOARD_H */
