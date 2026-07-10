/*
 * RA8P1 RTL8211F-CG Ethernet module test entry.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "hal_data.h"

void hal_entry(void)
{
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
}



