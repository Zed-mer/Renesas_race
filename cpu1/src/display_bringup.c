#include "display_bringup.h"
#include "jd9165_panel.h"
#include <string.h>

#define DISPLAY_DIAG_MAGIC    (0x4A443936U)
#define DISPLAY_PANEL_READ_SKIPPED (UINT32_MAX)
#define DISPLAY_BACKLIGHT     (BSP_IO_PORT_00_PIN_12)
#define DISPLAY_PANEL_RESET   (PANEL_RESET)
#define DISPLAY_RESET_ACTIVE  (BSP_IO_LEVEL_LOW)
#define DISPLAY_RESET_IDLE    (BSP_IO_LEVEL_HIGH)
#define DISPLAY_RESET_PIN_CFG ((uint32_t) IOPORT_CFG_DRIVE_MID |          \
                               (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                               (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)
#define DISPLAY_BACKLIGHT_PIN_CFG ((uint32_t) IOPORT_CFG_DRIVE_MID |          \
                                   (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                   (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)
#define DISPLAY_RESET_LOW_HOLD_MS          (10U)
#define DISPLAY_RESET_READY_MIN_MS         (17U)
#define DISPLAY_RESET_RELEASE_WAIT_MS      (120U)
#define DISPLAY_SDRAM_TEST_OFFSET_BYTES (0x400U)
#define DISPLAY_SDRAM_TEST_BASE_VALUE   (0x11223344U)
#define DISPLAY_SDRAM_TEST_OFFSET_VALUE (0x55667788U)
#define DISPLAY_SDRAM_TEST_RETRIES      (20U)
#define DISPLAY_SDRAM_TEST_RETRY_MS         (10U)
#define DISPLAY_STARTUP_CLEAN_VSYNCS        (8U)
#define DISPLAY_STARTUP_WAIT_MAIN_FRAME     (1UL << 0)
#define DISPLAY_STARTUP_WAIT_LAYER2_FRAME   (1UL << 1)
#define DISPLAY_STARTUP_WAIT_LAYER_LATCH    (1UL << 2)
#define DISPLAY_STARTUP_WAIT_DISPLAY_ERROR  (1UL << 3)
#if (BSP_CFG_LCDCLK_SOURCE != BSP_CLOCKS_SOURCE_CLOCK_PLL2R) || \
    (BSP_CFG_LCDCLK_DIV != BSP_CLOCKS_LCD_CLOCK_DIV_2)
 #error "Display timing requires LCDCLK from PLL2R divided by 2"
#endif
#if (BSP_CFG_PLL2R_FREQUENCY_HZ != 480000000U)
 #error "Display timing requires a 480 MHz PLL2R clock"
#endif
#define DISPLAY_LCDCLK_HZ                    (BSP_CFG_PLL2R_FREQUENCY_HZ / 2U)
#define DISPLAY_DIAGNOSTIC_DSI_LANES         (2U)
#define DISPLAY_PANEL_CLOCK_DIVISOR           (6U)
#define DISPLAY_PANEL_CLOCK_HZ                (40000000U)
#define DISPLAY_HORIZONTAL_TOTAL_CYC          (1344U)
#define DISPLAY_HORIZONTAL_ACTIVE_CYC         (1024U)
#define DISPLAY_HORIZONTAL_BACK_PORCH_CYC     (112U)
#define DISPLAY_HORIZONTAL_SYNC_CYC           (24U)
#define DISPLAY_HORIZONTAL_BACK_START_CYC     (DISPLAY_HORIZONTAL_BACK_PORCH_CYC + \
                                               DISPLAY_HORIZONTAL_SYNC_CYC)
#define DISPLAY_HORIZONTAL_FRONT_PORCH_CYC    (184U)
#define DISPLAY_VERTICAL_TOTAL_CYC            (635U)
#define DISPLAY_VERTICAL_ACTIVE_CYC           (600U)
#define DISPLAY_VERTICAL_BACK_PORCH_CYC       (19U)
#define DISPLAY_VERTICAL_SYNC_CYC             (2U)
#define DISPLAY_VERTICAL_BACK_START_CYC       (DISPLAY_VERTICAL_BACK_PORCH_CYC + \
                                               DISPLAY_VERTICAL_SYNC_CYC)
#define DISPLAY_VERTICAL_FRONT_PORCH_CYC      (14U)
#define DISPLAY_DSI_HORIZONTAL_BACK_PORCH_CYC  (112U)
#define DISPLAY_DSI_HORIZONTAL_FRONT_PORCH_CYC (160U)
#define DISPLAY_DSI_VERTICAL_BACK_PORCH_CYC    (19U)
#define DISPLAY_DSI_VERTICAL_FRONT_PORCH_CYC   (12U)
#define DISPLAY_DSI_VIDEO_MODE_DELAY           (184U)

_Static_assert((DISPLAY_LCDCLK_HZ / DISPLAY_PANEL_CLOCK_DIVISOR) ==
               DISPLAY_PANEL_CLOCK_HZ,
               "The panel pixel clock must be exactly 40 MHz");
_Static_assert((DISPLAY_HORIZONTAL_ACTIVE_CYC + DISPLAY_HORIZONTAL_SYNC_CYC +
                DISPLAY_HORIZONTAL_BACK_PORCH_CYC +
                DISPLAY_HORIZONTAL_FRONT_PORCH_CYC) ==
               DISPLAY_HORIZONTAL_TOTAL_CYC,
               "Horizontal timing must match the panel total");
_Static_assert((DISPLAY_VERTICAL_ACTIVE_CYC + DISPLAY_VERTICAL_SYNC_CYC +
                DISPLAY_VERTICAL_BACK_PORCH_CYC +
                DISPLAY_VERTICAL_FRONT_PORCH_CYC) ==
               DISPLAY_VERTICAL_TOTAL_CYC,
               "Vertical timing must match the panel total");
_Static_assert(DISPLAY_RESET_RELEASE_WAIT_MS >= DISPLAY_RESET_READY_MIN_MS,
               "Panel reset release wait must meet the JD9165BA minimum");

volatile display_diag_t g_display_diag;

static mipi_dsi_cfg_t g_external_dsi_cfg;
static mipi_dsi_instance_t g_external_dsi_instance;
static glcdc_extended_cfg_t g_external_display_extend_cfg;
static display_cfg_t g_external_display_cfg;

typedef struct st_display_startup_health
{
    uint32_t underflows;
    uint32_t layer2_underflows;
    uint32_t buffer_errors;
    uint32_t overlay_errors;
    uint32_t video_status;
    uint32_t fatal_status;
    uint32_t phy_status;
} display_startup_health_t;

static display_startup_health_t g_startup_health;
static bool g_startup_health_valid;
static uint32_t g_startup_last_line_event;

static uint32_t display_startup_diag_next_sequence(void)
{
    uint32_t sequence = g_display_diag.startup_event_sequence + 1U;
    if (0U == sequence)
    {
        sequence = 1U;
    }
    g_display_diag.startup_event_sequence = sequence;
    return sequence;
}

void display_startup_diag_note_reset_asserted(void)
{
    g_display_diag.startup_reset_asserted = 1U;
    if (0U == g_display_diag.startup_reset_assert_sequence)
    {
        g_display_diag.startup_reset_assert_sequence =
            display_startup_diag_next_sequence();
    }
}

void display_startup_diag_note_first_dsi_command(uint8_t command)
{
    if (0U == g_display_diag.startup_first_dsi_command_sequence)
    {
        g_display_diag.startup_first_dsi_command = command;
        g_display_diag.startup_first_dsi_command_sequence =
            display_startup_diag_next_sequence();
    }
}

static display_startup_health_t display_startup_health_capture(void)
{
    const display_startup_health_t health = {
        .underflows = g_display_diag.glcdc_underflows,
        .layer2_underflows = g_display_diag.overlay_underflows,
        .buffer_errors = g_display_diag.animation_buffer_errors,
        .overlay_errors = g_display_diag.overlay_errors,
        .video_status = g_display_diag.video_status,
        .fatal_status = g_display_diag.fatal_status,
        .phy_status = g_display_diag.phy_status,
    };
    return health;
}

static bool display_startup_health_equal(
    const display_startup_health_t * left,
    const display_startup_health_t * right)
{
    return (left->underflows == right->underflows) &&
           (left->layer2_underflows == right->layer2_underflows) &&
           (left->buffer_errors == right->buffer_errors) &&
           (left->overlay_errors == right->overlay_errors) &&
           (left->video_status == right->video_status) &&
           (left->fatal_status == right->fatal_status) &&
           (left->phy_status == right->phy_status);
}

static void display_startup_health_record(
    const display_startup_health_t * health)
{
    g_display_diag.startup_last_underflows = health->underflows;
    g_display_diag.startup_last_layer2_underflows =
        health->layer2_underflows;
    g_display_diag.startup_last_buffer_errors = health->buffer_errors;
    g_display_diag.startup_last_overlay_errors = health->overlay_errors;
    g_display_diag.startup_last_video_status = health->video_status;
    g_display_diag.startup_last_fatal_status = health->fatal_status;
    g_display_diag.startup_last_phy_status = health->phy_status;
}

void display_underflow_context_enter(uint32_t context_mask)
{
    g_display_diag.underflow_context |= context_mask;
    __DMB();
}

void display_underflow_context_leave(uint32_t context_mask)
{
    __DMB();
    g_display_diag.underflow_context &= ~context_mask;
}

static void display_fps_counter_start(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    g_display_diag.fps_core_clock_hz = SystemCoreClock;
    g_display_diag.fps_counter_enabled =
        ((0U == (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)) &&
         (0U != (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk))) ? 1U : 0U;
}

static void display_fail(fsp_err_t err)
{
    g_display_diag.last_error = (int32_t) err;
    g_display_diag.stage = DISPLAY_STAGE_FAILED;
    g_display_diag.running = 0U;
    R_IOPORT_PinWrite(g_ioport.p_ctrl, DISPLAY_BACKLIGHT, BSP_IO_LEVEL_LOW);
}

static void display_clear_framebuffers(void)
{
    const size_t framebuffer_bytes =
        (size_t)DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * DISPLAY_VSIZE_INPUT0;
    for (uint32_t buffer = 0U; buffer < 2U; buffer++)
    {
        memset(&fb_background[buffer][0], 0, framebuffer_bytes);
    }
}

static fsp_err_t display_test_framebuffer_sdram(void)
{
    volatile uint32_t * const p_base = (volatile uint32_t *) &fb_background[0][0];
    volatile uint32_t * const p_offset =
        (volatile uint32_t *) (&fb_background[0][0] + DISPLAY_SDRAM_TEST_OFFSET_BYTES);

    *p_base = DISPLAY_SDRAM_TEST_BASE_VALUE;
    *p_offset = DISPLAY_SDRAM_TEST_OFFSET_VALUE;
    __DSB();

    g_display_diag.sdram_test_base = *p_base;
    g_display_diag.sdram_test_offset = *p_offset;
    if ((DISPLAY_SDRAM_TEST_BASE_VALUE != g_display_diag.sdram_test_base) ||
        (DISPLAY_SDRAM_TEST_OFFSET_VALUE != g_display_diag.sdram_test_offset))
    {
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    g_display_diag.sdram_alias_test_passed = 1U;
    return FSP_SUCCESS;
}

static fsp_err_t display_prepare_external_video_config(void)
{
    g_external_dsi_cfg = g_mipi_dsi0_cfg;

    g_external_dsi_instance = g_mipi_dsi0;
    g_external_dsi_instance.p_cfg = &g_external_dsi_cfg;

    g_external_display_cfg = g_display_cfg;

    g_external_display_extend_cfg = *((glcdc_extended_cfg_t const *) g_display_cfg.p_extend);
    g_external_display_extend_cfg.phy_layer = &g_external_dsi_instance;
    g_external_display_cfg.p_extend = &g_external_display_extend_cfg;

    uint32_t const horizontal_total =
        g_external_display_cfg.output.htiming.total_cyc;
    uint32_t const horizontal_active =
        g_external_display_cfg.output.htiming.display_cyc;
    uint32_t const horizontal_back_porch_including_sync =
        g_external_display_cfg.output.htiming.back_porch;
    uint32_t const horizontal_sync =
        g_external_display_cfg.output.htiming.sync_width;
    uint32_t const vertical_total =
        g_external_display_cfg.output.vtiming.total_cyc;
    uint32_t const vertical_active =
        g_external_display_cfg.output.vtiming.display_cyc;
    uint32_t const vertical_back_porch_including_sync =
        g_external_display_cfg.output.vtiming.back_porch;
    uint32_t const vertical_sync =
        g_external_display_cfg.output.vtiming.sync_width;

    if (((uint32_t)g_external_display_extend_cfg.clock_div_ratio !=
         DISPLAY_PANEL_CLOCK_DIVISOR) ||
        (horizontal_total != DISPLAY_HORIZONTAL_TOTAL_CYC) ||
        (horizontal_active != DISPLAY_HORIZONTAL_ACTIVE_CYC) ||
        (horizontal_back_porch_including_sync !=
         DISPLAY_HORIZONTAL_BACK_START_CYC) ||
        (horizontal_sync != DISPLAY_HORIZONTAL_SYNC_CYC) ||
        (vertical_total != DISPLAY_VERTICAL_TOTAL_CYC) ||
        (vertical_active != DISPLAY_VERTICAL_ACTIVE_CYC) ||
        (vertical_back_porch_including_sync !=
         DISPLAY_VERTICAL_BACK_START_CYC) ||
        (vertical_sync != DISPLAY_VERTICAL_SYNC_CYC))
    {
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    uint32_t const horizontal_back_porch =
        horizontal_back_porch_including_sync - horizontal_sync;
    uint32_t const horizontal_front_porch =
        horizontal_total - horizontal_active -
        horizontal_back_porch_including_sync;
    uint32_t const vertical_back_porch =
        vertical_back_porch_including_sync - vertical_sync;
    uint32_t const vertical_front_porch =
        vertical_total - vertical_active - vertical_back_porch_including_sync;

    if ((horizontal_back_porch != DISPLAY_HORIZONTAL_BACK_PORCH_CYC) ||
        (horizontal_front_porch != DISPLAY_HORIZONTAL_FRONT_PORCH_CYC) ||
        (vertical_back_porch != DISPLAY_VERTICAL_BACK_PORCH_CYC) ||
        (vertical_front_porch != DISPLAY_VERTICAL_FRONT_PORCH_CYC))
    {
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    /* The generated DSI porches and delay are one coupled scheduling tuple. */
    if ((g_external_dsi_cfg.num_lanes != DISPLAY_DIAGNOSTIC_DSI_LANES) ||
        (g_external_dsi_cfg.horizontal_active_lines != DISPLAY_HORIZONTAL_ACTIVE_CYC) ||
        (g_external_dsi_cfg.horizontal_sync_lines != DISPLAY_HORIZONTAL_SYNC_CYC) ||
        (g_external_dsi_cfg.horizontal_back_porch != DISPLAY_DSI_HORIZONTAL_BACK_PORCH_CYC) ||
        (g_external_dsi_cfg.horizontal_front_porch != DISPLAY_DSI_HORIZONTAL_FRONT_PORCH_CYC) ||
        (g_external_dsi_cfg.vertical_active_lines != DISPLAY_VERTICAL_ACTIVE_CYC) ||
        (g_external_dsi_cfg.vertical_sync_lines != DISPLAY_VERTICAL_SYNC_CYC) ||
        (g_external_dsi_cfg.vertical_back_porch != DISPLAY_DSI_VERTICAL_BACK_PORCH_CYC) ||
        (g_external_dsi_cfg.vertical_front_porch != DISPLAY_DSI_VERTICAL_FRONT_PORCH_CYC) ||
        (g_external_dsi_cfg.video_mode_delay != DISPLAY_DSI_VIDEO_MODE_DELAY))
    {
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    g_display_diag.external_video_timing_applied = 1U;
    g_display_diag.dsi_timing_verified = 1U;
    g_display_diag.active_dsi_lanes = g_external_dsi_cfg.num_lanes;
    g_display_diag.panel_clock_divisor = (uint32_t) g_external_display_extend_cfg.clock_div_ratio;
    g_display_diag.panel_clock_hz = DISPLAY_LCDCLK_HZ / g_display_diag.panel_clock_divisor;
    g_display_diag.horizontal_total_cyc = horizontal_total;
    g_display_diag.vertical_total_cyc = vertical_total;
    g_display_diag.horizontal_sync_cyc = g_external_dsi_cfg.horizontal_sync_lines;
    g_display_diag.horizontal_back_porch_cyc = g_external_dsi_cfg.horizontal_back_porch;
    g_display_diag.horizontal_front_porch_cyc = g_external_dsi_cfg.horizontal_front_porch;
    g_display_diag.vertical_sync_cyc = g_external_dsi_cfg.vertical_sync_lines;
    g_display_diag.vertical_back_porch_cyc = g_external_dsi_cfg.vertical_back_porch;
    g_display_diag.vertical_front_porch_cyc = g_external_dsi_cfg.vertical_front_porch;
    g_display_diag.dsi_video_mode_delay = g_external_dsi_cfg.video_mode_delay;
    g_display_diag.refresh_millihz = (uint32_t) ((((uint64_t) g_display_diag.panel_clock_hz) * 1000U) /
                                                (((uint64_t) g_display_diag.horizontal_total_cyc) *
                                                 g_display_diag.vertical_total_cyc));
    return FSP_SUCCESS;
}

static fsp_err_t display_panel_hardware_reset(void)
{
    fsp_err_t err = R_IOPORT_PinWrite(g_ioport.p_ctrl,
                                      DISPLAY_PANEL_RESET,
                                      DISPLAY_RESET_ACTIVE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    display_startup_diag_note_reset_asserted();
    g_display_diag.startup_reset_low_hold_ms =
        DISPLAY_RESET_LOW_HOLD_MS;
    R_BSP_SoftwareDelay(DISPLAY_RESET_LOW_HOLD_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);

    err = R_IOPORT_PinWrite(g_ioport.p_ctrl, DISPLAY_PANEL_RESET, DISPLAY_RESET_IDLE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    g_display_diag.startup_reset_released = 1U;
    g_display_diag.startup_reset_release_sequence =
        display_startup_diag_next_sequence();
    g_display_diag.startup_reset_release_wait_ms =
        DISPLAY_RESET_RELEASE_WAIT_MS;
    R_BSP_SoftwareDelay(DISPLAY_RESET_RELEASE_WAIT_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);

    bsp_io_level_t reset_level = BSP_IO_LEVEL_LOW;
    err = R_IOPORT_PinRead(g_ioport.p_ctrl, DISPLAY_PANEL_RESET, &reset_level);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_display_diag.reset_idle_level = (uint32_t) reset_level;
    g_display_diag.reset_sequence_done = 1U;
    return FSP_SUCCESS;
}

void display_bringup_run(void)
{
    const struct
    {
        uint32_t backlight_low_asserted;
        uint32_t reset_asserted;
        uint32_t ioport_error;
        uint32_t backlight_cfg_error;
        uint32_t backlight_write_error;
        uint32_t reset_cfg_error;
        uint32_t reset_write_error;
        uint32_t reset_read_error;
        uint32_t reset_level;
        uint32_t event_sequence;
        uint32_t reset_assert_sequence;
    } warmstart = {
        .backlight_low_asserted =
            g_display_diag.startup_backlight_low_asserted,
        .reset_asserted = g_display_diag.startup_reset_asserted,
        .ioport_error = g_display_diag.startup_warmstart_ioport_error,
        .backlight_cfg_error =
            g_display_diag.startup_warmstart_backlight_cfg_error,
        .backlight_write_error =
            g_display_diag.startup_warmstart_backlight_write_error,
        .reset_cfg_error =
            g_display_diag.startup_warmstart_reset_cfg_error,
        .reset_write_error =
            g_display_diag.startup_warmstart_reset_write_error,
        .reset_read_error =
            g_display_diag.startup_warmstart_reset_read_error,
        .reset_level = g_display_diag.startup_warmstart_reset_level,
        .event_sequence = g_display_diag.startup_event_sequence,
        .reset_assert_sequence =
            g_display_diag.startup_reset_assert_sequence,
    };

    memset((void *) &g_display_diag, 0, sizeof(g_display_diag));
    g_display_diag.magic = DISPLAY_DIAG_MAGIC;
    g_display_diag.reset_pin = (uint32_t) DISPLAY_PANEL_RESET;
    g_display_diag.reset_active_level = (uint32_t) DISPLAY_RESET_ACTIVE;
    g_display_diag.startup_clean_vsync_required =
        DISPLAY_STARTUP_CLEAN_VSYNCS;
    g_display_diag.startup_backlight_low_asserted =
        warmstart.backlight_low_asserted;
    g_display_diag.startup_reset_asserted = warmstart.reset_asserted;
    g_display_diag.startup_warmstart_ioport_error =
        warmstart.ioport_error;
    g_display_diag.startup_warmstart_backlight_cfg_error =
        warmstart.backlight_cfg_error;
    g_display_diag.startup_warmstart_backlight_write_error =
        warmstart.backlight_write_error;
    g_display_diag.startup_warmstart_reset_cfg_error =
        warmstart.reset_cfg_error;
    g_display_diag.startup_warmstart_reset_write_error =
        warmstart.reset_write_error;
    g_display_diag.startup_warmstart_reset_read_error =
        warmstart.reset_read_error;
    g_display_diag.startup_warmstart_reset_level =
        warmstart.reset_level;
    g_display_diag.startup_event_sequence = warmstart.event_sequence;
    g_display_diag.startup_reset_assert_sequence =
        warmstart.reset_assert_sequence;
    memset(&g_startup_health, 0, sizeof(g_startup_health));
    g_startup_health_valid = false;
    g_startup_last_line_event = 0U;

    bsp_io_level_t startup_level = BSP_IO_LEVEL_HIGH;
    if (FSP_SUCCESS == R_IOPORT_PinRead(g_ioport.p_ctrl,
                                       DISPLAY_BACKLIGHT,
                                       &startup_level))
    {
        g_display_diag.startup_pin_levels_valid |= 1U;
        g_display_diag.startup_backlight_initial_level =
            (uint32_t) startup_level;
    }
    startup_level = BSP_IO_LEVEL_HIGH;
    if (FSP_SUCCESS == R_IOPORT_PinRead(g_ioport.p_ctrl,
                                       DISPLAY_PANEL_RESET,
                                       &startup_level))
    {
        g_display_diag.startup_pin_levels_valid |= 2U;
        g_display_diag.startup_reset_initial_level =
            (uint32_t) startup_level;
    }

    if ((FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_ioport_error) ||
        (FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_backlight_cfg_error) ||
        (FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_backlight_write_error) ||
        (FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_reset_cfg_error) ||
        (FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_reset_write_error) ||
        (FSP_SUCCESS !=
         (fsp_err_t)g_display_diag.startup_warmstart_reset_read_error) ||
        (BSP_IO_LEVEL_LOW !=
         (bsp_io_level_t)g_display_diag.startup_warmstart_reset_level) ||
        (0U == g_display_diag.startup_backlight_low_asserted) ||
        (0U == g_display_diag.startup_reset_asserted))
    {
        display_fail(FSP_ERR_INVALID_HW_CONDITION);
        return;
    }

    fsp_err_t err = R_IOPORT_PinCfg(g_ioport.p_ctrl, DISPLAY_BACKLIGHT, DISPLAY_BACKLIGHT_PIN_CFG);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    err = R_IOPORT_PinWrite(g_ioport.p_ctrl, DISPLAY_BACKLIGHT, BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    g_display_diag.startup_backlight_low_asserted = 1U;
    err = R_IOPORT_PinCfg(g_ioport.p_ctrl, DISPLAY_PANEL_RESET, DISPLAY_RESET_PIN_CFG);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    err = R_IOPORT_PinWrite(g_ioport.p_ctrl,
                            DISPLAY_PANEL_RESET,
                            DISPLAY_RESET_ACTIVE);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    display_startup_diag_note_reset_asserted();

    /* The multicore launch can release CPU1 a few milliseconds before the
     * CPU0 warm-start SDRAM initialization has completed.  Retry the harmless
     * alias test instead of permanently disabling the panel on that race. */
    for (uint32_t attempt = 0U; attempt < DISPLAY_SDRAM_TEST_RETRIES; ++attempt)
    {
        err = display_test_framebuffer_sdram();
        if (FSP_SUCCESS == err)
        {
            break;
        }
        R_BSP_SoftwareDelay(DISPLAY_SDRAM_TEST_RETRY_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }

    display_clear_framebuffers();
    g_display_diag.startup_black_framebuffer_ready = 1U;
    g_display_diag.stage = DISPLAY_STAGE_FRAMEBUFFER_READY;

    err = display_prepare_external_video_config();
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    err = R_GLCDC_Open(&g_display_ctrl, &g_external_display_cfg);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    g_display_diag.stage = DISPLAY_STAGE_HOST_OPEN;

    err = display_panel_hardware_reset();
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    g_display_diag.stage = DISPLAY_STAGE_PANEL_RESET;

    err = jd9165_panel_configure();
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    g_display_diag.stage = DISPLAY_STAGE_PANEL_CONFIGURED;
    g_display_diag.startup_panel_configured = 1U;

    err = jd9165_panel_disable_bist();
    g_display_diag.bist_disable_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }

    /* Register reads require a bidirectional LP/BTA turnaround.  They are not
     * needed to start video and can trigger LP contention on the long FPC. */
    g_display_diag.panel_lane_read_error = DISPLAY_PANEL_READ_SKIPPED;
    g_display_diag.panel_read_error = DISPLAY_PANEL_READ_SKIPPED;

    /* Leave the panel in normal-video mode immediately before VRUN.  Low-power
     * DSI commands are not valid once the video stream has started. */
    err = jd9165_panel_disable_bist();
    g_display_diag.bist_disable_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }

    display_fps_counter_start();
    err = R_GLCDC_Start(&g_display_ctrl);
    if (FSP_SUCCESS != err)
    {
        display_fail(err);
        return;
    }
    g_display_diag.stage = DISPLAY_STAGE_VIDEO_STARTED;
    g_display_diag.startup_video_started = 1U;
    g_display_diag.running = 1U;
}

fsp_err_t display_backlight_startup_step(void)
{
    if (DISPLAY_STAGE_BACKLIGHT_ON == g_display_diag.stage)
    {
        return FSP_SUCCESS;
    }

    g_display_diag.startup_gate_steps++;

    const display_startup_health_t health =
        display_startup_health_capture();
    display_startup_health_record(&health);

    if (0U == g_display_diag.running)
    {
        return FSP_ERR_NOT_OPEN;
    }

    uint32_t wait_flags = 0U;
    if ((0U == g_display_diag.startup_black_framebuffer_ready) ||
        (0U == g_display_diag.startup_panel_configured) ||
        (0U == g_display_diag.startup_video_started) ||
        (0U == g_display_diag.animation_buffer_changes))
    {
        wait_flags |= DISPLAY_STARTUP_WAIT_MAIN_FRAME;
    }
    if ((0U == g_display_diag.overlay_enabled) ||
        (0U == R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLMRD))
    {
        wait_flags |= DISPLAY_STARTUP_WAIT_LAYER2_FRAME;
    }
    if ((0U != health.buffer_errors) ||
        (0U != health.overlay_errors) ||
        (0U != health.video_status) ||
        (0U != health.fatal_status) ||
        (0U != health.phy_status))
    {
        wait_flags |= DISPLAY_STARTUP_WAIT_DISPLAY_ERROR;
    }

    if (0U != wait_flags)
    {
        g_startup_health_valid = false;
        g_display_diag.startup_clean_vsync_count = 0U;
        g_startup_last_line_event = g_display_diag.glcdc_line_events;
        g_display_diag.startup_clean_last_line_event =
            g_startup_last_line_event;
        g_display_diag.startup_wait_flags = wait_flags;
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    const uint32_t line_event = g_display_diag.glcdc_line_events;
    if (!g_startup_health_valid)
    {
        g_startup_health = health;
        g_startup_health_valid = true;
        g_startup_last_line_event = line_event;
        g_display_diag.startup_clean_last_line_event = line_event;
        g_display_diag.startup_clean_vsync_count = 0U;
        g_display_diag.startup_wait_flags = 0U;
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    if (line_event == g_startup_last_line_event)
    {
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    g_startup_last_line_event = line_event;
    g_display_diag.startup_clean_last_line_event = line_event;
    if (!display_startup_health_equal(&g_startup_health, &health))
    {
        g_startup_health = health;
        g_display_diag.startup_clean_vsync_count = 0U;
        g_display_diag.startup_clean_vsync_restarts++;
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    g_display_diag.startup_clean_vsync_count++;
    if (g_display_diag.startup_clean_vsync_count <
        DISPLAY_STARTUP_CLEAN_VSYNCS)
    {
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    if ((0U != R_GLCDC->GR[DISPLAY_FRAME_LAYER_1].VEN_b.PVEN) ||
        (0U != R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].VEN_b.PVEN) ||
        (0U != R_GLCDC->BG.EN_b.VEN))
    {
        g_display_diag.startup_wait_flags =
            DISPLAY_STARTUP_WAIT_LAYER_LATCH;
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    bsp_io_level_t backlight_level = BSP_IO_LEVEL_HIGH;
    fsp_err_t err = R_IOPORT_PinRead(g_ioport.p_ctrl,
                                     DISPLAY_BACKLIGHT,
                                     &backlight_level);
    if (FSP_SUCCESS != err)
    {
        g_display_diag.startup_gate_last_error = (uint32_t) err;
        return err;
    }
    g_display_diag.startup_backlight_readback =
        (uint32_t) backlight_level;
    if (BSP_IO_LEVEL_LOW != backlight_level)
    {
        (void) R_IOPORT_PinWrite(g_ioport.p_ctrl,
                                 DISPLAY_BACKLIGHT,
                                 BSP_IO_LEVEL_LOW);
        g_startup_health_valid = false;
        g_display_diag.startup_clean_vsync_count = 0U;
        g_display_diag.startup_clean_vsync_restarts++;
        g_display_diag.startup_gate_waits++;
        return FSP_ERR_IN_USE;
    }

    g_display_diag.startup_before_underflows = health.underflows;
    g_display_diag.startup_before_layer2_underflows =
        health.layer2_underflows;
    g_display_diag.startup_before_buffer_errors = health.buffer_errors;
    g_display_diag.startup_before_overlay_errors = health.overlay_errors;
    g_display_diag.startup_before_video_status = health.video_status;
    g_display_diag.startup_before_fatal_status = health.fatal_status;
    g_display_diag.startup_before_phy_status = health.phy_status;
    if ((0U == g_display_diag.startup_reset_assert_sequence) ||
        (g_display_diag.startup_reset_release_sequence <=
         g_display_diag.startup_reset_assert_sequence) ||
        (g_display_diag.startup_first_dsi_command_sequence <=
         g_display_diag.startup_reset_release_sequence) ||
        (0U == g_display_diag.startup_panel_configured))
    {
        g_display_diag.startup_gate_last_error =
            (uint32_t) FSP_ERR_INVALID_HW_CONDITION;
        return FSP_ERR_INVALID_HW_CONDITION;
    }
    g_display_diag.startup_backlight_enable_attempts++;

    err = R_IOPORT_PinWrite(g_ioport.p_ctrl,
                            DISPLAY_BACKLIGHT,
                            BSP_IO_LEVEL_HIGH);
    if (FSP_SUCCESS != err)
    {
        g_display_diag.startup_gate_last_error = (uint32_t) err;
        return err;
    }
    if (0U == g_display_diag.startup_backlight_enable_sequence)
    {
        g_display_diag.startup_backlight_enable_sequence =
            display_startup_diag_next_sequence();
    }

    backlight_level = BSP_IO_LEVEL_LOW;
    err = R_IOPORT_PinRead(g_ioport.p_ctrl,
                           DISPLAY_BACKLIGHT,
                           &backlight_level);
    g_display_diag.startup_backlight_readback =
        (uint32_t) backlight_level;
    if ((FSP_SUCCESS != err) || (BSP_IO_LEVEL_HIGH != backlight_level))
    {
        g_display_diag.startup_gate_last_error = (uint32_t)
            ((FSP_SUCCESS != err) ? err : FSP_ERR_INVALID_HW_CONDITION);
        return (FSP_SUCCESS != err) ? err : FSP_ERR_INVALID_HW_CONDITION;
    }

    g_display_diag.startup_wait_flags = 0U;
    g_display_diag.startup_backlight_enabled = 1U;
    g_display_diag.startup_backlight_line_event = line_event;
    g_display_diag.startup_backlight_transitions++;
    g_display_diag.startup_sequence_valid =
        ((g_display_diag.startup_reset_assert_sequence <
          g_display_diag.startup_reset_release_sequence) &&
         (g_display_diag.startup_reset_release_sequence <
          g_display_diag.startup_first_dsi_command_sequence) &&
         (g_display_diag.startup_first_dsi_command_sequence <
          g_display_diag.startup_backlight_enable_sequence)) ? 1U : 0U;
    g_display_diag.stage = DISPLAY_STAGE_BACKLIGHT_ON;
    return FSP_SUCCESS;
}

void glcdc_callback(display_callback_args_t * p_args)
{
    if (DISPLAY_EVENT_LINE_DETECTION == p_args->event)
    {
        g_display_diag.glcdc_line_events++;
    }
    else if ((DISPLAY_EVENT_GR1_UNDERFLOW == p_args->event) || (DISPLAY_EVENT_GR2_UNDERFLOW == p_args->event))
    {
        const uint32_t context = g_display_diag.underflow_context;
        g_display_diag.glcdc_underflows++;
        g_display_diag.underflow_last_context = context;
        if (0U == context)
        {
            g_display_diag.underflow_unattributed++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_RESYNC))
        {
            g_display_diag.underflow_deferred_resync++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_CHANNEL_SWITCH))
        {
            g_display_diag.underflow_channel_switch++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_SPECTRUM_PRESENT))
        {
            g_display_diag.underflow_spectrum_present++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT))
        {
            g_display_diag.underflow_waterfall_present++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_LVGL_REFRESH))
        {
            g_display_diag.underflow_lvgl_refresh++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_FLUSH_WAIT))
        {
            g_display_diag.underflow_flush_wait++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_TILE_DRAIN))
        {
            g_display_diag.underflow_tile_drain++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_DRAW))
        {
            g_display_diag.underflow_deferred_draw++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_COMMIT))
        {
            g_display_diag.underflow_deferred_commit++;
        }
        if (0U != (context & DISPLAY_UNDERFLOW_CONTEXT_NORMAL_REFRESH))
        {
            g_display_diag.underflow_normal_refresh++;
        }
    }
}
