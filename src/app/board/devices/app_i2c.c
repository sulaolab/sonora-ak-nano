#include "app_i2c.h"

#include "resolved_board_config.h"
#include "app_runtime_overrides.h"
#include "timer_app.h"
#include "nora_i2c_master.h"
#include "nora_gpio.h"

#if RESOLVED_BOARD_USE_CMSIS_I2C
#include <stdint.h>
#include <stdio.h>
#include "Driver_I2C_dsPIC33AK.h"
#endif


#if RESOLVED_BOARD_USE_CMSIS_I2C
// Millisecond tick source for the CMSIS I2C driver timeout handling.
// Overrides the weak default in Driver_I2C_dsPIC33AK.c.
uint32_t Driver_I2C_dsPIC33AK_GetMs(void)
{
    return GetTicks();
}
#endif // RESOLVED_BOARD_USE_CMSIS_I2C


void app_i2c_cmsis_init(void)
{
#if RESOLVED_BOARD_USE_CMSIS_I2C
    int32_t ret;

#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE
    ret = Driver_I2C0.Initialize(NULL);
    printf("Driver_I2C0 Initialize ret=%ld\n", (long)ret);

    ret = Driver_I2C0.PowerControl(ARM_POWER_FULL);
    printf("Driver_I2C0 PowerControl FULL ret=%ld\n", (long)ret);

    ret = Driver_I2C0.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_FAST);
    printf("Driver_I2C0 BUS_SPEED FAST ret=%ld\n", (long)ret);
#else
    ret = Driver_I2C1.Initialize(NULL);
    printf("Driver_I2C1 Initialize ret=%ld\n", (long)ret);

    ret = Driver_I2C1.PowerControl(ARM_POWER_FULL);
    printf("Driver_I2C1 PowerControl FULL ret=%ld\n", (long)ret);

    ret = Driver_I2C1.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_FAST);
    printf("Driver_I2C1 BUS_SPEED FAST ret=%ld\n", (long)ret);

#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
    ret = Driver_I2C2.Initialize(NULL);
    printf("Driver_I2C2 Initialize ret=%ld\n", (long)ret);

    ret = Driver_I2C2.PowerControl(ARM_POWER_FULL);
    printf("Driver_I2C2 PowerControl FULL ret=%ld\n", (long)ret);

    ret = Driver_I2C2.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_FAST);
    printf("Driver_I2C2 BUS_SPEED FAST ret=%ld\n", (long)ret);
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE
#endif // RESOLVED_BOARD_USE_CMSIS_I2C
}


void app_i2c_hal_init(void)
{
    nora_i2c_config_t i2c_cfg;

    i2c_cfg.fcy_hz             = (uint32_t)(FCY);
    i2c_cfg.bus_hz             = 400000u;
    i2c_cfg.timeout_ms         = 10u;
    i2c_cfg.get_ms             = GetTicks;
    i2c_cfg.pending_timeout_ms = 0u;

#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE
    /*
     * I2C1 on the MPS506 sits on SDA1 = RB3 and SCL1 = RB4 (primary pins, so
     * FDEVOPT_ALTI2C1 stays OFF -- the alternate pair ASCL1/ASDA1 is RD5/RD6 and
     * goes nowhere useful on this board). Those are the Curiosity Nano's standard
     * SDA/SCL positions, which the Nano Base wires to every mikroBUS slot.
     *
     * BUT RB3/RB4 also carry AD4AN3 / AD4AN0, so they have an ANSEL bit, and
     * ANSELx comes out of reset set: the pin is an ANALOG input and its digital
     * input buffer is OFF. The I2C module then cannot read SDA or SCL, so every
     * transfer fails on the read side -- the codec answers and the master is deaf
     * (measured 2026-09-17: every wm8904 register read returned E32 = I2C HAL read
     * failed, so confirm_device_id failed and bring-up cancelled itself).
     *
     * This codebase has no bulk ANSELx=0 clear -- "each pin owner configures
     * analog/digital mode explicitly" (see init_ports() in main.c) -- and the I2C
     * HAL deliberately touches no pins at all. So the board layer must hand the
     * two pins over as digital. dir=INPUT leaves the output driver off and lets
     * the I2C module own the lines; only ANSEL is what we are here to change,
     * exactly as main.c does for the ITC-driven shield pin.
     */
    {
        static const nora_gpio_config_t i2c_pin_cfg =
        {
            .dir          = NORA_GPIO_DIR_INPUT,
            .pull         = NORA_GPIO_PULL_NONE,   /* Base board carries the pull-ups */
            .analog       = false,
            .open_drain   = false,
            .initial_high = false,
        };
        (void)nora_gpio_config( NORA_GPIO_PIN( NORA_GPIO_PORT_B, 3 ), &i2c_pin_cfg ); /* SDA1 */
        (void)nora_gpio_config( NORA_GPIO_PIN( NORA_GPIO_PORT_B, 4 ), &i2c_pin_cfg ); /* SCL1 */
    }

    (void)nora_i2c_init( NORA_I2C_INST_1, &i2c_cfg );   /* Nano Base Slot 1 */
#else
    (void)nora_i2c_init( NORA_I2C_INST_2, &i2c_cfg );   /* MikroBUS-A */
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
    (void)nora_i2c_init( NORA_I2C_INST_3, &i2c_cfg );   /* MikroBUS-B */
#elif RESOLVED_BOARD_AK128_J3_TDM_B
    (void)nora_i2c_init( NORA_I2C_INST_1, &i2c_cfg );   /* MikroBUS-B: DIM-P4/P6 */
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE
}
