/*
 *
 * Copyright (c) 2002-2005 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *
 *File Name  : bcmmii.c
 *
 *Description: Broadcom PHY/GPHY/Ethernet Switch Configuration
 *Revision:	09/25/2008, L.Sun created.
*/

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/platform_device.h>
#include <linux/compiler.h>
#include <asm/brcmstb/brcmstb.h>

#include "bcmgenet_map.h"
#include "bcmgenet.h"
#include "bcmmii.h"

/* read a value from the MII */
int bcmgenet_mii_read(struct net_device *dev, int phy_id, int location)
{
	int ret;
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	volatile struct uniMacRegs *umac = pDevCtrl->umac;

	if (phy_id == BRCM_PHY_ID_NONE) {
		switch (location) {
		case MII_BMCR:
			return pDevCtrl->phyType == BRCM_PHY_TYPE_EXT_MII ?
				BMCR_FULLDPLX | BMCR_SPEED100 :
				BMCR_FULLDPLX | BMCR_SPEED1000;
		case MII_BMSR:
			return BMSR_LSTATUS;
		default:
			return 0;
		}
	}

	mutex_lock(&pDevCtrl->mdio_mutex);

	umac->mdio_cmd = (MDIO_RD | (phy_id << MDIO_PMD_SHIFT) |
			(location << MDIO_REG_SHIFT));
	/* Start MDIO transaction*/
	umac->mdio_cmd |= MDIO_START_BUSY;
	wait_event_timeout(pDevCtrl->wq, !(umac->mdio_cmd & MDIO_START_BUSY),
			HZ/100);
	ret = umac->mdio_cmd;
	mutex_unlock(&pDevCtrl->mdio_mutex);
	if (ret & MDIO_READ_FAIL) {
		TRACE(("MDIO read failure\n"));
		ret = 0;
	}
	return ret & 0xffff;
}

/* write a value to the MII */
void bcmgenet_mii_write(struct net_device *dev, int phy_id,
			int location, int val)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	volatile struct uniMacRegs *umac = pDevCtrl->umac;

	if (phy_id == BRCM_PHY_ID_NONE)
		return;
	mutex_lock(&pDevCtrl->mdio_mutex);
	umac->mdio_cmd = (MDIO_WR | (phy_id << MDIO_PMD_SHIFT) |
			(location << MDIO_REG_SHIFT) | (0xffff & val));
	umac->mdio_cmd |= MDIO_START_BUSY;
	wait_event_timeout(pDevCtrl->wq, !(umac->mdio_cmd & MDIO_START_BUSY),
			HZ/100);
	mutex_unlock(&pDevCtrl->mdio_mutex);
}

/* mii register read/modify/write helper function */
static int __maybe_unused bcmgenet_mii_set_clr_bits(struct net_device *dev,
	int location, int set_mask, int clr_mask)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	int phy_id = pDevCtrl->phyAddr;
	int v;

	v = bcmgenet_mii_read(dev, phy_id, location);
	v &= ~clr_mask;
	v |= set_mask;
	bcmgenet_mii_write(dev, phy_id, location, v);
	return v;
}

/* probe for an external PHY via MDIO; return PHY address */
int bcmgenet_mii_probe(struct net_device *dev, void *p)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	int i;
	struct bcmemac_platform_data *cfg = p;

	if (cfg->phy_type != BRCM_PHY_TYPE_EXT_MII) {
		/*
		 * Enable RGMII to interface external PHY, disable
		 * internal 10/100 MII.
		 */
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= RGMII_MODE_EN;
	}
	/* Power down EPHY */
	if (pDevCtrl->ext)
		pDevCtrl->ext->ext_pwr_mgmt |= (EXT_PWR_DOWN_PHY |
			EXT_PWR_DOWN_DLL | EXT_PWR_DOWN_BIAS);

	for (i = 31; i >= 0; i--) {
		if (bcmgenet_mii_read(dev, i, MII_BMSR) != 0) {
			pDevCtrl->phyAddr = i;
			if (i == 1)
				continue;
			return 0;
		}
		TRACE(("I=%d\n", i));
	}
	return -ENODEV;
}

/*
 * setup netdev link state when PHY link status change and
 * update UMAC and RGMII block when link up
 */
void bcmgenet_mii_setup(struct net_device *dev)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	struct ethtool_cmd ecmd ;
	volatile struct uniMacRegs *umac = pDevCtrl->umac;
	int cur_link, prev_link;
	unsigned int val, cmd_bits = 0;

	if (pDevCtrl->phyType == BRCM_PHY_TYPE_MOCA)
		return;

	cur_link = mii_link_ok(&pDevCtrl->mii);
	prev_link = netif_carrier_ok(pDevCtrl->dev);
	if (cur_link && !prev_link) {
		mii_ethtool_gset(&pDevCtrl->mii, &ecmd);
		/*
		 * program UMAC and RGMII block based on established link
		 * speed, pause, and duplex.
		 * the speed set in umac->cmd tell RGMII block which clock
		 * 25MHz(100Mbps)/125MHz(1Gbps) to use for transmit.
		 * receive clock is provided by PHY.
		 */
		if (pDevCtrl->phyType != BRCM_PHY_TYPE_EXT_RGMII_IBS) {
			GENET_RGMII_OOB_CTRL(pDevCtrl) &= ~OOB_DISABLE;
			GENET_RGMII_OOB_CTRL(pDevCtrl) |= RGMII_LINK;

			/* speed */
			if (ecmd.speed == SPEED_1000)
				cmd_bits = UMAC_SPEED_1000;
			else if (ecmd.speed == SPEED_100)
				cmd_bits = UMAC_SPEED_100;
			else
				cmd_bits = UMAC_SPEED_10;
			cmd_bits <<= CMD_SPEED_SHIFT;

			/* duplex */
			if (ecmd.duplex != DUPLEX_FULL)
				cmd_bits |= CMD_HD_EN;

		}
		/* pause capability */
		if (pDevCtrl->phyType == BRCM_PHY_TYPE_INT ||
		    pDevCtrl->phyType == BRCM_PHY_TYPE_EXT_MII) {
			val = bcmgenet_mii_read(
				dev, pDevCtrl->phyAddr, MII_LPA);
			if (!(val & LPA_PAUSE_CAP)) {
				cmd_bits |= CMD_RX_PAUSE_IGNORE;
				cmd_bits |= CMD_TX_PAUSE_IGNORE;
			}
		} else if (pDevCtrl->phyType == BRCM_PHY_TYPE_EXT_RGMII ||
			   pDevCtrl->phyType == BRCM_PHY_TYPE_EXT_RGMII_IBS) {
			val = bcmgenet_mii_read(dev,
				pDevCtrl->phyAddr, MII_BRCM_AUX_STAT_SUM);
			if (!(val & MII_BRCM_AUX_GPHY_RX_PAUSE))
				cmd_bits |= CMD_RX_PAUSE_IGNORE;
			if (!(val & MII_BRCM_AUX_GPHY_TX_PAUSE))
				cmd_bits |= CMD_TX_PAUSE_IGNORE;
		}

		umac->cmd &= ~((CMD_SPEED_MASK << CMD_SPEED_SHIFT) |
			       CMD_HD_EN |
			       CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE);
		umac->cmd |= cmd_bits;

		netif_carrier_on(pDevCtrl->dev);
		netdev_info(dev, "link up, %d Mbps, %s duplex\n", ecmd.speed,
			ecmd.duplex == DUPLEX_FULL ? "full" : "half");
	} else if (!cur_link && prev_link) {
		netif_carrier_off(pDevCtrl->dev);
		netdev_info(dev, "link down\n");
	}
}

void bcmgenet_ephy_workaround(struct net_device *dev)
{
#ifdef CONFIG_BRCM_EPHY_CLOCK_INV
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	int phy_id = pDevCtrl->phyAddr;

	/* set shadow mode 2 */
	bcmgenet_mii_set_clr_bits(dev, 0x1f, 0x0004, 0x0004);

	/*
	 * Workaround for SWLINUX-2281: explicitly reset IDDQ_CLKBIAS
	 * in the Shadow 2 regset, due to power sequencing issues.
	 */
	/* set iddq_clkbias */
	bcmgenet_mii_write(dev, phy_id, 0x14, 0x0F00);
	udelay(10);
	/* reset iddq_clkbias */
	bcmgenet_mii_write(dev, phy_id, 0x14, 0x0C00);

	/*
	 * Workaround for SWLINUX-2056: fix timing issue between the ephy
	 * digital and the ephy analog blocks.  This clock inversion will
	 * inherently fix any setup and hold issue.
	 */
	bcmgenet_mii_write(dev, phy_id, 0x13, 0x7555);

	/* reset shadow mode 2 */
	bcmgenet_mii_set_clr_bits(dev, 0x1f, 0x0004, 0);
#endif
}

void bcmgenet_mii_reset(struct net_device *dev)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);

	bcmgenet_mii_write(dev, pDevCtrl->phyAddr, MII_BMCR, BMCR_RESET);
	udelay(1);
	/* enable 64 clock MDIO */
	bcmgenet_mii_write(dev, pDevCtrl->phyAddr, 0x1d, 0x1000);
	bcmgenet_mii_read(dev, pDevCtrl->phyAddr, 0x1d);
	bcmgenet_ephy_workaround(dev);
}

int bcmgenet_mii_init(struct net_device *dev)
{
	struct BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	volatile struct uniMacRegs *umac;
	char *phy_name;

	umac = pDevCtrl->umac;
	pDevCtrl->mii.phy_id = pDevCtrl->phyAddr;
	pDevCtrl->mii.phy_id_mask = 0x1f;
	pDevCtrl->mii.reg_num_mask = 0x1f;
	pDevCtrl->mii.dev = dev;
	pDevCtrl->mii.mdio_read = bcmgenet_mii_read;
	pDevCtrl->mii.mdio_write = bcmgenet_mii_write;
	pDevCtrl->mii.supports_gmii = 0;

	switch (pDevCtrl->phyType) {

	case BRCM_PHY_TYPE_INT:
		phy_name = "internal PHY";
		pDevCtrl->sys->sys_port_ctrl = PORT_MODE_INT_EPHY;
		/* enable APD */
		pDevCtrl->ext->ext_pwr_mgmt |= EXT_PWR_DN_EN_LD;
		bcmgenet_mii_reset(dev);
		break;
	case BRCM_PHY_TYPE_EXT_MII:
		phy_name = "external MII";
		pDevCtrl->sys->sys_port_ctrl = PORT_MODE_EXT_EPHY;
		break;
	case BRCM_PHY_TYPE_EXT_RGMII:
		phy_name = "external RGMII";
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= RGMII_MODE_EN;
		/* Don't shift tx clock by 90 degree, does it still needed? */
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= (1 << 16);
		pDevCtrl->mii.supports_gmii = 1;
		pDevCtrl->sys->sys_port_ctrl = PORT_MODE_EXT_GPHY;
		break;
	case BRCM_PHY_TYPE_EXT_RGMII_IBS:
		phy_name = "external RGMII IBS";
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= RGMII_MODE_EN;
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= (1 << 16);
		/* Use in-band signaling for auto config.*/
		GENET_RGMII_OOB_CTRL(pDevCtrl) |= OOB_DISABLE;
		umac->cmd |= CMD_AUTO_CONFIG;
		pDevCtrl->mii.supports_gmii = 1;
		pDevCtrl->sys->sys_port_ctrl = PORT_MODE_EXT_GPHY;
		break;
	case BRCM_PHY_TYPE_MOCA:
		phy_name = "MoCA";
		/* setup speed in umac->cmd for RGMII txclk set to 125MHz */
		umac->cmd = umac->cmd  | (UMAC_SPEED_1000 << CMD_SPEED_SHIFT);
		pDevCtrl->mii.force_media = 1;
		pDevCtrl->sys->sys_port_ctrl = PORT_MODE_INT_GPHY |
			LED_ACT_SOURCE_MAC;
		break;
	default:
		pr_err("unknown phy_type: %d\n", pDevCtrl->phyType);
		return -1;
	}

	pr_info("%s: configuring instance #%d for %s\n",
			CARDNAME, pDevCtrl->pdev->id, phy_name);

	return 0;
}
