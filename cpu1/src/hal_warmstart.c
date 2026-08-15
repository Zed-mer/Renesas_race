/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "hal_data.h"
#include "display_bringup.h"

#if !BSP_CFG_EARLY_INIT
#error "CPU1 display reset requires BSP_CFG_EARLY_INIT enabled"
#endif

#define DISPLAY_STARTUP_BACKLIGHT_PIN  (BSP_IO_PORT_00_PIN_12)
#define DISPLAY_STARTUP_PANEL_RESET_PIN (PANEL_RESET)
#define DISPLAY_STARTUP_OUTPUT_LOW_CFG ((uint32_t) IOPORT_CFG_DRIVE_MID | \
                                        (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                        (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);

FSP_CPP_FOOTER

/*******************************************************************************************************************//**
 * This function is called at various points during the startup process.  This implementation uses the event that is
 * called right before main() to set up the pins.
 *
 * @param[in]  event    Where at in the start up process the code is currently at
 **********************************************************************************************************************/
void R_BSP_WarmStart (bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0

        /* Enable reading from data flash. */
        R_FACI_LP->DFLCTL = 1U;

        /* Would normally have to wait tDSTOP(6us) for data flash recovery. Placing the enable here, before clock and
         * C runtime initialization, should negate the need for a delay since the initialization will typically take more than 6us. */
#endif

        /* This event runs before the C runtime. Use only the early BSP pin API
         * so a warm reset cannot briefly expose the panel or release RESX. */
        R_BSP_PinAccessEnable();
        R_BSP_PinWrite(DISPLAY_STARTUP_BACKLIGHT_PIN, BSP_IO_LEVEL_LOW);
        R_BSP_PinWrite(DISPLAY_STARTUP_PANEL_RESET_PIN, BSP_IO_LEVEL_LOW);
        R_BSP_PinAccessDisable();
    }

#if BSP_CFG_OSPI_B_STARTUP_ENABLED && defined(BSP_CFG_OSPI_B_STARTUP_FN)
    if (BSP_WARM_START_POST_CLOCK == event)
    {
        /* Setup OSPI_B SiP flash and initialize it. */
        R_BSP_OspiBInit(BSP_CFG_OSPI_B_STARTUP_FN, true);
    }
#endif

    if (BSP_WARM_START_POST_C == event)
    {
        /* C runtime environment and system clocks are setup. */

        /* Keep the panel invisible and RESX asserted from the earliest phase
         * where the FSP I/O driver is available. */
        g_display_diag.startup_warmstart_backlight_cfg_error =
            (uint32_t) FSP_ERR_NOT_OPEN;
        g_display_diag.startup_warmstart_backlight_write_error =
            (uint32_t) FSP_ERR_NOT_OPEN;
        g_display_diag.startup_warmstart_reset_cfg_error =
            (uint32_t) FSP_ERR_NOT_OPEN;
        g_display_diag.startup_warmstart_reset_write_error =
            (uint32_t) FSP_ERR_NOT_OPEN;
        g_display_diag.startup_warmstart_reset_read_error =
            (uint32_t) FSP_ERR_NOT_OPEN;
        g_display_diag.startup_warmstart_reset_level =
            (uint32_t) BSP_IO_LEVEL_HIGH;
        fsp_err_t const ioport_err =
            R_IOPORT_Open(&IOPORT_CFG_CTRL, &IOPORT_CFG_NAME);
        g_display_diag.startup_warmstart_ioport_error =
            (uint32_t) ioport_err;
        if (FSP_SUCCESS == ioport_err)
        {
            const fsp_err_t backlight_cfg_err =
                R_IOPORT_PinCfg(&IOPORT_CFG_CTRL,
                                DISPLAY_STARTUP_BACKLIGHT_PIN,
                                DISPLAY_STARTUP_OUTPUT_LOW_CFG);
            g_display_diag.startup_warmstart_backlight_cfg_error =
                (uint32_t) backlight_cfg_err;
            if (FSP_SUCCESS == backlight_cfg_err)
            {
                const fsp_err_t backlight_write_err =
                    R_IOPORT_PinWrite(&IOPORT_CFG_CTRL,
                                      DISPLAY_STARTUP_BACKLIGHT_PIN,
                                      BSP_IO_LEVEL_LOW);
                g_display_diag.startup_warmstart_backlight_write_error =
                    (uint32_t) backlight_write_err;
                if (FSP_SUCCESS == backlight_write_err)
                {
                    g_display_diag.startup_backlight_low_asserted = 1U;
                }
            }

            const fsp_err_t reset_cfg_err =
                R_IOPORT_PinCfg(&IOPORT_CFG_CTRL,
                                DISPLAY_STARTUP_PANEL_RESET_PIN,
                                DISPLAY_STARTUP_OUTPUT_LOW_CFG);
            g_display_diag.startup_warmstart_reset_cfg_error =
                (uint32_t) reset_cfg_err;
            if (FSP_SUCCESS == reset_cfg_err)
            {
                const fsp_err_t reset_write_err =
                    R_IOPORT_PinWrite(&IOPORT_CFG_CTRL,
                                      DISPLAY_STARTUP_PANEL_RESET_PIN,
                                      BSP_IO_LEVEL_LOW);
                g_display_diag.startup_warmstart_reset_write_error =
                    (uint32_t) reset_write_err;
                if (FSP_SUCCESS == reset_write_err)
                {
                    display_startup_diag_note_reset_asserted();
                    bsp_io_level_t reset_level = BSP_IO_LEVEL_HIGH;
                    const fsp_err_t reset_read_err =
                        R_IOPORT_PinRead(&IOPORT_CFG_CTRL,
                                        DISPLAY_STARTUP_PANEL_RESET_PIN,
                                        &reset_level);
                    g_display_diag.startup_warmstart_reset_read_error =
                        (uint32_t) reset_read_err;
                    g_display_diag.startup_warmstart_reset_level =
                        (uint32_t) reset_level;
                }
            }
        }

#if BSP_CFG_SDRAM_ENABLED && !BSP_SECONDARY_CORE_BUILD

        /* CPU0 initializes the shared SDRAM controller before releasing CPU1. */
        R_BSP_SdramInit(true);
#endif
    }
}
