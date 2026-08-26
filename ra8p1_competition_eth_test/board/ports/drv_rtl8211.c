/*
 * drv_ethernet.c
 *
 *  Created on: 2025骞?鏈?9鏃?
 *      Author: RTT
 */
#include <rtthread.h>
#include "hal_data.h"
#include <rtdevice.h>
#include <board.h>

#ifdef BSP_USING_ETH

void rmac_phy_target_rtl8211_initialize (rmac_phy_instance_ctrl_t * phydev)
{
#define RTL_8211F_PAGE_SELECT       0x1F
#define RTL_8211F_BMCR              0x00
#define RTL_8211F_EEELCR_ADDR       0x11
#define RTL_8211F_LED_PAGE          0xD04
#define RTL_8211F_LCR_ADDR          0x10
#define RTL_8211F_RGMII_PAGE        0xD08
#define RTL_8211F_TX_DELAY_REG      0x11
#define RTL_8211F_RX_DELAY_REG      0x15
#define RTL_8211F_TX_DELAY_BIT      (1U << 8)
#define RTL_8211F_RX_DELAY_BIT      (1U << 3)
#define RTL_8211F_ENABLE_TX_DELAY   0
#define RTL_8211F_ENABLE_RX_DELAY   1

    uint32_t val1, val2 = 0;

    R_RMAC_PHY_Write(phydev, RTL_8211F_PAGE_SELECT, RTL_8211F_RGMII_PAGE);

    R_RMAC_PHY_Read(phydev, RTL_8211F_TX_DELAY_REG, &val1);
#if RTL_8211F_ENABLE_TX_DELAY
    val1 |= RTL_8211F_TX_DELAY_BIT;
#else
    val1 &= ~RTL_8211F_TX_DELAY_BIT;
#endif
    R_RMAC_PHY_Write(phydev, RTL_8211F_TX_DELAY_REG, val1);

    R_RMAC_PHY_Read(phydev, RTL_8211F_RX_DELAY_REG, &val1);
#if RTL_8211F_ENABLE_RX_DELAY
    val1 |= RTL_8211F_RX_DELAY_BIT;
#else
    val1 &= ~RTL_8211F_RX_DELAY_BIT;
#endif
    R_RMAC_PHY_Write(phydev, RTL_8211F_RX_DELAY_REG, val1);

    R_RMAC_PHY_Write(phydev, RTL_8211F_PAGE_SELECT, 0x0000);
    R_RMAC_PHY_Write(phydev, RTL_8211F_BMCR, 0x1200);
    rt_thread_mdelay(300);

    R_RMAC_PHY_Write(phydev, RTL_8211F_PAGE_SELECT, RTL_8211F_LED_PAGE);

    R_RMAC_PHY_Read(phydev, RTL_8211F_LCR_ADDR, &val1);
    val1 |= (1 << 5);
    val1 |= (1 << 8);
    val1 &= (~(1 << 9));
    val1 |= (1 << 10);
    val1 |= (1 << 11);
    R_RMAC_PHY_Write(phydev, RTL_8211F_LCR_ADDR, val1);

    R_RMAC_PHY_Read(phydev, RTL_8211F_EEELCR_ADDR, &val2);
    val2 &= (~(1 << 2));
    R_RMAC_PHY_Write(phydev, RTL_8211F_EEELCR_ADDR, val2);

    R_RMAC_PHY_Write(phydev, RTL_8211F_PAGE_SELECT, 0x0000);
}

bool rmac_phy_target_rtl8211_is_support_link_partner_ability (rmac_phy_instance_ctrl_t * p_instance_ctrl,
                                                             uint32_t                   line_speed_duplex)
{
    FSP_PARAMETER_NOT_USED(p_instance_ctrl);
    FSP_PARAMETER_NOT_USED(line_speed_duplex);

    /* This PHY-LSI supports half and full duplex mode. */
    return true;
}
#endif









