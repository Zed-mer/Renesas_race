/*
 * Small RT-Thread shell helpers for validating an external RTL8211F-CG module.
 */

#include <rtthread.h>
#include <netdev_ipaddr.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <netdev.h>

#define ETH_TEST_THREAD_STACK_SIZE 2048U
#define ETH_TEST_THREAD_PRIORITY   20
#define ETH_TEST_THREAD_TICK       10
#define ETH_TEST_WAIT_DEFAULT_MS   10000U
#define ETH_TEST_PING_TIMEOUT_MS   3000U
#define ETH_TEST_PING_SIZE         32U
#define ETH_TEST_PC_IP             "169.254.139.8"

static struct rt_thread s_eth_thread;
static rt_uint8_t s_eth_thread_stack[ETH_TEST_THREAD_STACK_SIZE];
static rt_bool_t s_eth_thread_started = RT_FALSE;

static struct netdev *eth_test_get_netdev(const char *name)
{
    struct netdev *netdev = RT_NULL;

    if ((name != RT_NULL) && (name[0] != '\0'))
    {
        netdev = netdev_get_by_name(name);
    }

    if (netdev == RT_NULL)
    {
        netdev = netdev_default;
    }

    if (netdev == RT_NULL)
    {
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP | NETDEV_FLAG_LINK_UP);
    }

    return netdev;
}

static rt_bool_t eth_test_net_ready(struct netdev *netdev)
{
    return ((netdev != RT_NULL) && netdev_is_up(netdev) && netdev_is_link_up(netdev)) ? RT_TRUE : RT_FALSE;
}

static void eth_test_print_hwaddr(const struct netdev *netdev)
{
    rt_uint8_t i;

    if ((netdev == RT_NULL) || (netdev->hwaddr_len == 0U))
    {
        rt_kprintf("mac: unknown\n");
        return;
    }

    rt_kprintf("mac:");
    for (i = 0U; i < netdev->hwaddr_len; i++)
    {
        rt_kprintf("%s%02x", (i == 0U) ? " " : ":", netdev->hwaddr[i]);
    }
    rt_kprintf("\n");
}

static void eth_test_print_flags(const struct netdev *netdev)
{
    rt_kprintf("flags:");
    rt_kprintf(netdev_is_up(netdev) ? " UP" : " DOWN");
    rt_kprintf(netdev_is_link_up(netdev) ? " LINK_UP" : " LINK_DOWN");
    rt_kprintf(netdev_is_dhcp_enabled(netdev) ? " DHCP" : " STATIC");
    if ((netdev->flags & NETDEV_FLAG_ETHARP) != 0U)
    {
        rt_kprintf(" ETHARP");
    }
    if ((netdev->flags & NETDEV_FLAG_BROADCAST) != 0U)
    {
        rt_kprintf(" BROADCAST");
    }
    rt_kprintf("\n");
}

static void eth_test_print_status(struct netdev *netdev)
{
    if (netdev == RT_NULL)
    {
        rt_kprintf("eth_status: no netdev registered yet\n");
        return;
    }

    rt_kprintf("netdev: %s%s\n", netdev->name, (netdev == netdev_default) ? " (default)" : "");
    eth_test_print_flags(netdev);
    eth_test_print_hwaddr(netdev);
    rt_kprintf("mtu: %u\n", (unsigned int)netdev->mtu);
    rt_kprintf("ip: %s\n", inet_ntoa(netdev->ip_addr));
    rt_kprintf("gw: %s\n", inet_ntoa(netdev->gw));
    rt_kprintf("mask: %s\n", inet_ntoa(netdev->netmask));
}

static void eth_test_thread_entry(void *parameter)
{
    rt_bool_t last_ready = RT_FALSE;
    rt_bool_t first_report = RT_TRUE;
    rt_uint32_t auto_ping_tick = 0U;

    RT_UNUSED(parameter);

    while (1)
    {
        struct netdev *netdev = eth_test_get_netdev(RT_NULL);
        rt_bool_t ready = eth_test_net_ready(netdev);

        if (first_report || (ready != last_ready))
        {
            first_report = RT_FALSE;
            last_ready = ready;
            rt_kprintf("[eth] %s\n", ready ? "link ready" : "waiting for link");
            eth_test_print_status(netdev);
        }

        if (ready && (netdev->ops != RT_NULL) && (netdev->ops->ping != RT_NULL))
        {
            auto_ping_tick++;
            if (auto_ping_tick >= 5U)
            {
                struct netdev_ping_resp resp;

                auto_ping_tick = 0U;
                rt_memset(&resp, 0, sizeof(resp));
                (void)netdev->ops->ping(netdev,
                                        ETH_TEST_PC_IP,
                                        ETH_TEST_PING_SIZE,
                                        ETH_TEST_PING_TIMEOUT_MS,
                                        &resp,
                                        RT_FALSE);
            }
        }

        rt_thread_mdelay(1000);
    }
}

void eth_test_start(void)
{
    if (s_eth_thread_started)
    {
        return;
    }

    if (rt_thread_init(&s_eth_thread,
                       "ethmon",
                       eth_test_thread_entry,
                       RT_NULL,
                       s_eth_thread_stack,
                       sizeof(s_eth_thread_stack),
                       ETH_TEST_THREAD_PRIORITY,
                       ETH_TEST_THREAD_TICK) == RT_EOK)
    {
        s_eth_thread_started = RT_TRUE;
        rt_thread_startup(&s_eth_thread);
    }
    else
    {
        rt_kprintf("[eth] monitor thread start failed\n");
    }
}

static void eth_status_cmd(int argc, char **argv)
{
    const char *name = (argc >= 2) ? argv[1] : RT_NULL;
    eth_test_print_status(eth_test_get_netdev(name));
}
MSH_CMD_EXPORT_ALIAS(eth_status_cmd, eth_status, show Ethernet netdev status);

static void eth_wait_cmd(int argc, char **argv)
{
    rt_uint32_t timeout_ms = ETH_TEST_WAIT_DEFAULT_MS;
    rt_tick_t deadline;

    if (argc >= 2)
    {
        timeout_ms = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    }

    deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);
    while ((rt_int32_t)(rt_tick_get() - deadline) < 0)
    {
        struct netdev *netdev = eth_test_get_netdev(RT_NULL);
        if (eth_test_net_ready(netdev))
        {
            rt_kprintf("eth_wait: link ready\n");
            eth_test_print_status(netdev);
            return;
        }
        rt_thread_mdelay(100);
    }

    rt_kprintf("eth_wait: timeout after %lu ms\n", (unsigned long)timeout_ms);
    eth_test_print_status(eth_test_get_netdev(RT_NULL));
}
MSH_CMD_EXPORT_ALIAS(eth_wait_cmd, eth_wait, wait for Ethernet link up);

static void eth_ping_cmd(int argc, char **argv)
{
    struct netdev *netdev = eth_test_get_netdev(RT_NULL);
    rt_uint32_t count = 4U;
    rt_uint32_t i;

    if (argc < 2)
    {
        rt_kprintf("Usage: eth_ping <peer_ip_or_host> [count]\n");
        return;
    }

    if (argc >= 3)
    {
        count = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0);
        if (count == 0U)
        {
            count = 1U;
        }
    }

    if (!eth_test_net_ready(netdev))
    {
        rt_kprintf("eth_ping: netdev is not ready\n");
        eth_test_print_status(netdev);
        return;
    }

    if ((netdev->ops == RT_NULL) || (netdev->ops->ping == RT_NULL))
    {
        rt_kprintf("eth_ping: ping operation is unavailable\n");
        return;
    }

    for (i = 0U; i < count; i++)
    {
        struct netdev_ping_resp resp;
        int ret;

        rt_memset(&resp, 0, sizeof(resp));
        ret = netdev->ops->ping(netdev,
                                argv[1],
                                ETH_TEST_PING_SIZE,
                                ETH_TEST_PING_TIMEOUT_MS,
                                &resp,
                                RT_FALSE);
        if (ret == RT_EOK)
        {
            rt_kprintf("eth_ping: %lu bytes from %s seq=%lu ttl=%u time=%lu ms\n",
                       (unsigned long)resp.data_len,
                       inet_ntoa(resp.ip_addr),
                       (unsigned long)i,
                       (unsigned int)resp.ttl,
                       (unsigned long)resp.ticks);
        }
        else
        {
            rt_kprintf("eth_ping: timeout seq=%lu ret=%d\n", (unsigned long)i, ret);
        }

        rt_thread_mdelay(1000);
    }
}
MSH_CMD_EXPORT_ALIAS(eth_ping_cmd, eth_ping, ping through default Ethernet netdev);
