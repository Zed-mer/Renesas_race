/*
 * RA8P1 RTL8211F-CG Ethernet module test entry.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "hal_data.h"
#include "eth_perf.h"
#include "sdr_iiod_perf.h"
#include "framework/rf_pipeline.h"

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



