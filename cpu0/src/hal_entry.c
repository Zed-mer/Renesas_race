/*
 * RA8P1 RTL8211F-CG Ethernet module test entry.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdbool.h>
#include <board.h>
#include "hal_data.h"
#include "eth_perf.h"
#include "sdr_iiod_perf.h"
#include "framework/rf_pipeline.h"
#include "framework/activity_mailbox.h"
#include "framework/esp_report.h"

#define PANEL_SHUTDOWN_ACK_TIMEOUT_MS (1000U)

void eth_test_start(void);

void hal_entry(void)
{
#if (0 == _RA_CORE) && (1 == BSP_MULTICORE_PROJECT) && !BSP_TZ_NONSECURE_BUILD
    R_BSP_SecondaryCoreStart();
#endif

    rt_kprintf("Competition board P714 is enabled during board warm start.\n");

    rt_kprintf("\nRA8P1 RTL8211F-CG Ethernet module test\n");
    rt_kprintf("==================================================\n");
    rt_kprintf("Default netdev: e0\n");
    rt_kprintf("Static IP: %s, gateway: %s, mask: %s\n",
               RT_LWIP_IPADDR, RT_LWIP_GWADDR, RT_LWIP_MSKADDR);
    rt_kprintf("Commands:\n");
    rt_kprintf("  ifconfig\n");
    rt_kprintf("  ping <peer_ip>\n");
    rt_kprintf("  eth_status [netdev]\n");
    rt_kprintf("  eth_wait [timeout_ms]\n");
    rt_kprintf("  eth_ping <peer_ip> [count]\n");
    rt_kprintf("==================================================\n");

    eth_test_start();
    eth_perf_start();
    rf_pipeline_start();
    esp_report_start();
#if !SDR_IIOD_PERF_BOOT_ENABLE
    /* Keep the zeroed result ABI available to the read-only network sampler
     * even when --gc-sections removes the diagnostic thread. These volatile
     * reads have no network side effect and do not start iiod. */
    (void)g_sdr_iiod_perf_result.magic;
    (void)g_sdr_iiod_perf_report[0];
#endif
#if SDR_IIOD_PERF_BOOT_ENABLE
    /* Explicit diagnostic build only. The production default is zero because
     * this TCP/iio context contends with IQSC/UDP streaming. */
    sdr_iiod_perf_start();
#endif
}

static void panel_reset_cpu0_state_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)&RA8P1_ACTIVITY_CONTROL->cpu0,
                            (int32_t)sizeof(RA8P1_ACTIVITY_CONTROL->cpu0));
#endif
    __DSB();
}

static void panel_reset_cpu1_state_invalidate(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)&RA8P1_ACTIVITY_CONTROL->cpu1,
                                 (int32_t)sizeof(RA8P1_ACTIVITY_CONTROL->cpu1));
#endif
    __DSB();
}

void rt_hw_cpu_reset(void)
{
    volatile ra8p1_activity_cpu0_state_t * const cpu0 =
        &RA8P1_ACTIVITY_CONTROL->cpu0;
    volatile ra8p1_activity_cpu1_state_t * const cpu1 =
        &RA8P1_ACTIVITY_CONTROL->cpu1;

    (void)rt_hw_interrupt_disable();
    panel_reset_cpu1_state_invalidate();
    const bool handshake_ready =
        (cpu0->magic == RA8P1_ACTIVITY_CONTROL_MAGIC) &&
        (cpu0->version == RA8P1_ACTIVITY_CONTROL_VERSION) &&
        (cpu0->size == sizeof(*cpu0)) &&
        (cpu0->boot_epoch != 0U) &&
        ((cpu0->flags & RA8P1_ACTIVITY_CPU0_FLAG_READY) != 0U) &&
        (cpu1->magic == RA8P1_ACTIVITY_CONTROL_MAGIC) &&
        (cpu1->version == RA8P1_ACTIVITY_CONTROL_VERSION) &&
        (cpu1->size == sizeof(*cpu1)) &&
        (cpu1->observed_cpu0_epoch == cpu0->boot_epoch) &&
        ((cpu1->flags & RA8P1_ACTIVITY_CPU1_FLAG_ONLINE) != 0U);

    if (handshake_ready)
    {
        cpu0->flags |= RA8P1_ACTIVITY_CPU0_FLAG_PANEL_SHUTDOWN_REQUEST;
        __DMB();
        panel_reset_cpu0_state_clean();
        for (uint32_t elapsed_ms = 0U;
             elapsed_ms < PANEL_SHUTDOWN_ACK_TIMEOUT_MS;
             ++elapsed_ms)
        {
            panel_reset_cpu1_state_invalidate();
            if ((cpu1->observed_cpu0_epoch == cpu0->boot_epoch) &&
                ((cpu1->flags & RA8P1_ACTIVITY_CPU1_FLAG_PANEL_SHUTDOWN_ACK) != 0U))
            {
                break;
            }
            R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    NVIC_SystemReset();
    while (1)
    {
        __NOP();
    }
}



