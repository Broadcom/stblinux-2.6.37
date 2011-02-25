/*
 * Copyright (C) 2002-2009 Broadcom Corporation 
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
 * Description: Kernel driver for EMAC/ENET 10/100 Ethernet MAC/PHY
 */

#define DRV_NAME		"bcmemac"
#define DRV_VERSION		"3.0"

// Turn this on to allow Hardware Multicast Filter
#define MULTICAST_HW_FILTER

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/platform_device.h>
#include <linux/stddef.h>

#include <asm/io.h>
#include <asm/cacheflush.h>
#include <asm/brcmstb/brcmstb.h>
// #include <asm/brcmstb/brcm-pm.h>

#include "bcmemac.h"

#define NAPI_WEIGHT		16
#define EMAC_MDC		0x1f

#define EMAC_TX_WATERMARK   0x180

#define MAKE4(x)   ((x[3] & 0xFF) | ((x[2] & 0xFF) << 8) | ((x[1] & 0xFF) << 16) | ((x[0] & 0xFF) << 24))
#define MAKE2(x)   ((x[1] & 0xFF) | ((x[0] & 0xFF) << 8))

/* Ignore the link status if the MSB of the PHY ID is set */
#define IGNORE_LINK_STATUS(phy_id) ((phy_id) & 0x10)

/*
 * IP Header Optimization, on 7401B0 and on-wards
 * We present an aligned buffer to the DMA engine, but ask it
 * to transfer the data with a 2-byte offset from the top.
 * The idea is to have the IP payload aligned on a 4-byte boundary
 * because the IP payload follows a 14-byte Ethernet header
 */
#define RX_CONFIG_PKT_OFFSET_SHIFT		9
#define RX_CONFIG_PKT_OFFSET_MASK		0x0000_0600

#define ENET_POLL_DONE      			0x80000000

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
//      External, indirect entry points. 
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
//      Called for "ifconfig ethX up" & "down"
// --------------------------------------------------------------------------
static int bcmemac_net_open(struct net_device *dev);
static int bcmemac_net_close(struct net_device *dev);
// --------------------------------------------------------------------------
//      Watchdog timeout
// --------------------------------------------------------------------------
static void bcmemac_net_timeout(struct net_device *dev);
// --------------------------------------------------------------------------
//      Packet transmission. 
// --------------------------------------------------------------------------
static int bcmemac_net_xmit(struct sk_buff *skb, struct net_device *dev);
// --------------------------------------------------------------------------
//      Set address filtering mode
// --------------------------------------------------------------------------
static void bcm_set_multicast_list(struct net_device *dev);
// --------------------------------------------------------------------------
//      Set the hardware MAC address.
// --------------------------------------------------------------------------
static int bcm_set_mac_addr(struct net_device *dev, void *p);

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
//      Interrupt routines
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
static irqreturn_t bcmemac_net_isr(int irq, void *);
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
//      dev->poll() method
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
static int bcmemac_enet_poll(struct napi_struct *napi, int budget);
// --------------------------------------------------------------------------
//  Bottom half service routine. Process all received packets.                  
// --------------------------------------------------------------------------
static int bcmemac_rx(BcmEnet_devctrl * pDevCtrl, int budget);

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
//      Internal routines
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
/* Add an address to the ARL register */
static void write_mac_address(struct net_device *dev);
/* Allocate and initialize tx/rx buffer descriptor pools */
static int bcmemac_init_dev(BcmEnet_devctrl * pDevCtrl);
static int bcmemac_uninit_dev(BcmEnet_devctrl * pDevCtrl);
/* Assign the Rx descriptor ring */
static int assign_rx_buffers(BcmEnet_devctrl * pDevCtrl);
/* Initialize driver's pools of receive buffers and tranmit headers */
static int init_buffers(BcmEnet_devctrl * pDevCtrl);
/* Initialise the Ethernet Switch control registers */
static int init_emac(BcmEnet_devctrl * pDevCtrl);
/* Initialize the Ethernet Switch MIB registers */
static void clear_mib(volatile EmacRegisters * emac);

/* update an address to the EMAC perfect match registers */
static void perfectmatch_update(BcmEnet_devctrl * pDevCtrl, const u8 * pAddr,
				bool bValid);

#ifdef MULTICAST_HW_FILTER
/* clear perfect match registers except given pAddr (PR10861) */
static void perfectmatch_clean(BcmEnet_devctrl * pDevCtrl, const u8 * pAddr);
#endif

/* write an address to the EMAC perfect match registers */
static void perfectmatch_write(volatile EmacRegisters * emac, unsigned int reg,
			       const u8 * pAddr, bool bValid);
/* Initialize DMA control register */
static void init_IUdma(BcmEnet_devctrl * pDevCtrl);
/* Add a Tx control block to the pool */
static void txCb_enq(BcmEnet_devctrl * pDevCtrl, Enet_Tx_CB * txCbPtr);
/* Remove a Tx control block from the pool*/
static Enet_Tx_CB *txCb_deq(BcmEnet_devctrl * pDevCtrl);

struct net_device * eth_root_dev = NULL;
/* --------------------------------------------------------------------------
 Purpose: Get current available Tx desc count, for backword compatability
-------------------------------------------------------------------------- */
int bcmemac_get_free_txdesc(struct net_device * dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	return pDevCtrl->txFreeBds;
}
/* --------------------------------------------------------------------------
 Purpose: Get first emac device, for backword compatability
-------------------------------------------------------------------------- */
struct net_device * bcmemac_get_device(void) {
	return eth_root_dev;
}
/* --------------------------------------------------------------------------
 Purpose: MDIO / link state functions
-------------------------------------------------------------------------- */

static int bcmemac_mdio_read(struct net_device *dev, int addr, int reg)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	int ret;

	if (addr == BRCM_PHY_ID_NONE)
		return 0xffff;

	mutex_lock(&pDevCtrl->mdio_mutex);
	pDevCtrl->emac->mdioData = MDIO_RD | (addr << MDIO_PMD_SHIFT) |
	    (reg << MDIO_REG_SHIFT);
	wait_for_completion(&pDevCtrl->mdio_complete);
	ret = pDevCtrl->emac->mdioData & 0xffff;
	mutex_unlock(&pDevCtrl->mdio_mutex);

	if (addr == MII_BMSR && IGNORE_LINK_STATUS(addr))
		ret |= BMSR_LSTATUS;

	return ret;
}

static void bcmemac_mdio_write(struct net_device *dev, int addr, int reg,
			       int data)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);

	if (addr == BRCM_PHY_ID_NONE)
		return;

	mutex_lock(&pDevCtrl->mdio_mutex);
	data = MDIO_WR | (addr << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT) |
	    data;
	pDevCtrl->emac->mdioData = data;
	wait_for_completion(&pDevCtrl->mdio_complete);
	mutex_unlock(&pDevCtrl->mdio_mutex);
}

static int bcmemac_phy_init(BcmEnet_devctrl * pDevCtrl, int phy_id)
{
	unsigned int i;

	pDevCtrl->emac->mdioFreq = EMAC_MII_PRE_EN | EMAC_MDC;

	/* autodetect phy_id */
	if (phy_id == BRCM_PHY_ID_AUTO) {
		i = 1;
		while (1) {
			u16 data = bcmemac_mdio_read(pDevCtrl->dev,
						     i, MII_BMSR);

			if (data != 0x0000 && data != 0xffff)
				break;

			i = (i + 1) & 0x1f;
			if (i == 1)
				return -ENODEV;
		}
		phy_id = i;
	}

	pDevCtrl->mii.phy_id = phy_id;

	bcmemac_mdio_write(pDevCtrl->dev, phy_id, MII_BMCR, BMCR_RESET);
	mdelay(1);
	bcmemac_mdio_write(pDevCtrl->dev, phy_id, MII_BMCR,
			   BMCR_ANENABLE | BMCR_ANRESTART | BMCR_SPEED100);

	return 0;
}

void bcmemac_link_status(struct work_struct *work)
{
	BcmEnet_devctrl *pDevCtrl = container_of(work, BcmEnet_devctrl,
						 link_status_work);
	struct ethtool_cmd cmd;
	int link;

	if (pDevCtrl->mii.phy_id == BRCM_PHY_ID_NONE) {
		cmd.speed = SPEED_100;
		cmd.duplex = DUPLEX_FULL;
		link = 1;
	} else {
		mii_ethtool_gset(&pDevCtrl->mii, &cmd);
		link = mii_link_ok(&pDevCtrl->mii);
	}

	if (link && !netif_carrier_ok(pDevCtrl->dev)) {
		printk(KERN_INFO DRV_NAME
		       ": %s NIC Link is Up %s Mbps %s Duplex\n",
		       pDevCtrl->dev->name,
		       cmd.speed == SPEED_100 ? "100" : "10",
		       cmd.duplex == DUPLEX_FULL ? "Full" : "Half");
		netif_carrier_on(pDevCtrl->dev);
	} else if (!link && netif_carrier_ok(pDevCtrl->dev)) {
		printk(KERN_INFO DRV_NAME ": %s NIC Link is Down\n",
		       pDevCtrl->dev->name);
		netif_carrier_off(pDevCtrl->dev);
	}

	if (cmd.duplex == DUPLEX_FULL)
		pDevCtrl->emac->txControl |= EMAC_FD;
	else
		pDevCtrl->emac->txControl &= ~EMAC_FD;
}

static void bcmemac_link_timer(unsigned long arg)
{
	BcmEnet_devctrl *pDevCtrl = (BcmEnet_devctrl *) arg;

	schedule_work(&pDevCtrl->link_status_work);
	mod_timer(&pDevCtrl->timer, jiffies + HZ);
}

/* --------------------------------------------------------------------------
    Name: bcmemac_net_open
 Purpose: Open and Initialize the EMAC on the chip
-------------------------------------------------------------------------- */
static int bcmemac_net_open(struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	ASSERT(pDevCtrl != NULL);

	TRACE(("%s: bcmemac_net_open, EMACConf=%x, &RxDMA=%x, rxDma.cfg=%x\n",
	       dev->name, pDevCtrl->emac->config, &pDevCtrl->rxDma,
	       pDevCtrl->rxDma->cfg));

	/* disable ethernet MAC while updating its registers */
	pDevCtrl->emac->config |= EMAC_DISABLE;
	while (pDevCtrl->emac->config & EMAC_DISABLE) ;

	pDevCtrl->emac->config |= EMAC_ENABLE;
	pDevCtrl->dmaRegs->controller_cfg |= IUDMA_ENABLE;

	// Tell Tx DMA engine to start from the top
#ifdef IUDMA_INIT_WORKAROUND
	{
		unsigned long diag_rdbk;

		pDevCtrl->dmaRegs->enet_iudma_diag_ctl = 0x100;	/* enable to read diags. */
		diag_rdbk = pDevCtrl->dmaRegs->enet_iudma_diag_rdbk;
		pDevCtrl->txDma->descPtr =
		    (u32) CPHYSADDR(pDevCtrl->txFirstBdPtr);
		pDevCtrl->txNextBdPtr =
		    pDevCtrl->txFirstBdPtr +
		    ((diag_rdbk >> (16 + 1)) & DESC_MASK);
	}
#endif

	/* Initialize emac registers */
	if (pDevCtrl->bIPHdrOptimize) {
		pDevCtrl->emac->rxControl =
		    EMAC_FC_EN | EMAC_UNIFLOW | (2 <<
						 RX_CONFIG_PKT_OFFSET_SHIFT);
	} else {
		pDevCtrl->emac->rxControl = EMAC_FC_EN | EMAC_UNIFLOW /* |EMAC_PROM */ ;	// Don't allow Promiscuous
	}
	pDevCtrl->rxDma->cfg |= DMA_ENABLE;

	pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk |= R5K_IUDMA_IRQ;

	mod_timer(&pDevCtrl->timer, jiffies);

	// Start the network engine
	netif_start_queue(dev);
	napi_enable(&pDevCtrl->napi);

	return 0;
}

/* --------------------------------------------------------------------------
    Name: bcmemac_net_close
 Purpose: Stop communicating with the outside world
    Note: Caused by 'ifconfig ethX down'
-------------------------------------------------------------------------- */
static int bcmemac_net_close(struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	Enet_Tx_CB *txCBPtr;

	ASSERT(pDevCtrl != NULL);

	TRACE(("%s: bcmemac_net_close\n", dev->name));

	napi_disable(&pDevCtrl->napi);
	netif_stop_queue(dev);

	pDevCtrl->rxDma->cfg &= ~DMA_ENABLE;
	pDevCtrl->emac->config |= EMAC_DISABLE;

	pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk &= ~R5K_IUDMA_IRQ;

	del_timer_sync(&pDevCtrl->timer);
	cancel_work_sync(&pDevCtrl->link_status_work);

	/* free the skb in the txCbPtrHead */
	while (pDevCtrl->txCbPtrHead) {
		pDevCtrl->txFreeBds += pDevCtrl->txCbPtrHead->nrBds;

		if (pDevCtrl->txCbPtrHead->skb)
			dev_kfree_skb(pDevCtrl->txCbPtrHead->skb);

		txCBPtr = pDevCtrl->txCbPtrHead;

		/* Advance the current reclaim pointer */
		pDevCtrl->txCbPtrHead = pDevCtrl->txCbPtrHead->next;

		/* Finally, return the transmit header to the free list */
		txCb_enq(pDevCtrl, txCBPtr);
	}
	/* Adjust the tail if the list is empty */
	if (pDevCtrl->txCbPtrHead == NULL)
		pDevCtrl->txCbPtrTail = NULL;

	pDevCtrl->txNextBdPtr = pDevCtrl->txFirstBdPtr = pDevCtrl->txBds;
	return 0;
}

/* --------------------------------------------------------------------------
    Name: bcmemac_net_timeout
 Purpose: 
-------------------------------------------------------------------------- */
static void bcmemac_net_timeout(struct net_device *dev)
{
	ASSERT(dev != NULL);

	TRACE(("%s: bcmemac_net_timeout\n", dev->name));

	dev->trans_start = jiffies;

	netif_wake_queue(dev);
}

/* --------------------------------------------------------------------------
    Name: bcm_set_multicast_list
 Purpose: Set the multicast mode, ie. promiscuous or multicast
-------------------------------------------------------------------------- */
static void bcm_set_multicast_list(struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
	struct netdev_hw_addr *ha;
	int mc_count = netdev_mc_count(dev);
#else
	struct dev_mc_list *dmi;
	int mc_count = dev->mc_count;
#endif

	TRACE(("%s: bcm_set_multicast_list: %08X\n", dev->name, dev->flags));

	/* Promiscous mode */
	if (dev->flags & IFF_PROMISC) {
		pDevCtrl->emac->rxControl |= EMAC_PROM;
	} else {
		pDevCtrl->emac->rxControl &= ~EMAC_PROM;
	}

#ifndef MULTICAST_HW_FILTER
	/* All Multicast packets (PR10861 Check for any multicast request) */
	if (dev->flags & IFF_ALLMULTI || mc_count) {
		pDevCtrl->emac->rxControl |= EMAC_ALL_MCAST;
	} else {
		pDevCtrl->emac->rxControl &= ~EMAC_ALL_MCAST;
	}
#else
	{
		/* PR10861 - Filter specific group Multicast packets (R&C 2nd Ed., p463) */
		if (dev->flags & IFF_ALLMULTI
		    || mc_count > (MAX_PMADDR - 1)) {
			perfectmatch_clean(pDevCtrl, dev->dev_addr);
			pDevCtrl->emac->rxControl |= EMAC_ALL_MCAST;
			return;
		} else {
			pDevCtrl->emac->rxControl &= ~EMAC_ALL_MCAST;
		}

		/* No multicast? Just get our own stuff */
		if (mc_count == 0)
			return;

		/* Store multicast addresses in the prefect match registers */
		perfectmatch_clean(pDevCtrl, dev->dev_addr);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
		netdev_for_each_mc_addr(ha, dev)
			perfectmatch_update(pDevCtrl, ha->addr, 1);
#else
		for (dmi = dev->mc_list; dmi; dmi = dmi->next)
			perfectmatch_update(pDevCtrl, dmi->dmi_addr, 1);
#endif
	}
#endif
}

/*
 * txCb_enq: add a Tx control block to the pool
 */
static void txCb_enq(BcmEnet_devctrl * pDevCtrl, Enet_Tx_CB * txCbPtr)
{
	txCbPtr->next = NULL;

	if (pDevCtrl->txCbQTail) {
		pDevCtrl->txCbQTail->next = txCbPtr;
		pDevCtrl->txCbQTail = txCbPtr;
	} else {
		pDevCtrl->txCbQHead = pDevCtrl->txCbQTail = txCbPtr;
	}
}

/*
 * txCb_deq: remove a Tx control block from the pool
 */
static Enet_Tx_CB *txCb_deq(BcmEnet_devctrl * pDevCtrl)
{
	Enet_Tx_CB *txCbPtr;

	if ((txCbPtr = pDevCtrl->txCbQHead)) {
		pDevCtrl->txCbQHead = txCbPtr->next;
		txCbPtr->next = NULL;

		if (pDevCtrl->txCbQHead == NULL)
			pDevCtrl->txCbQTail = NULL;
	} else {
		txCbPtr = NULL;
	}
	return txCbPtr;
}

/* --------------------------------------------------------------------------
    Name: bcmemac_xmit_check
 Purpose: Reclaims TX descriptors
-------------------------------------------------------------------------- */
int bcmemac_xmit_check(struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	Enet_Tx_CB *txCBPtr;
	Enet_Tx_CB *txedCBPtr;
	unsigned long flags, ret = 0, old_free = pDevCtrl->txFreeBds;

	ASSERT(pDevCtrl != NULL);

	/*
	 * Obtain exclusive access to transmitter.  This is necessary because
	 * we might have more than one stack transmitting at once.
	 */
	spin_lock_irqsave(&pDevCtrl->lock, flags);

	txCBPtr = NULL;

	/* Reclaim transmitted buffers */
	while (pDevCtrl->txCbPtrHead) {
		if (EMAC_SWAP32
		    (pDevCtrl->txCbPtrHead->lastBdAddr->
		     length_status) & (DMA_OWN)) {
			break;
		}
		pDevCtrl->txFreeBds += pDevCtrl->txCbPtrHead->nrBds;

		if (pDevCtrl->txCbPtrHead->skb) {
			dev_kfree_skb_any(pDevCtrl->txCbPtrHead->skb);
		}

		txedCBPtr = pDevCtrl->txCbPtrHead;

		/* Advance the current reclaim pointer */
		pDevCtrl->txCbPtrHead = pDevCtrl->txCbPtrHead->next;

		/* 
		 * Finally, return the transmit header to the free list.
		 * But keep one around, so we don't have to allocate again
		 */
		txCb_enq(pDevCtrl, txedCBPtr);
		ret++;
	}

	/* Adjust the tail if the list is empty */
	if (pDevCtrl->txCbPtrHead == NULL)
		pDevCtrl->txCbPtrTail = NULL;

	if (pDevCtrl->txFreeBds && !old_free)
		netif_wake_queue(dev);

	spin_unlock_irqrestore(&pDevCtrl->lock, flags);
	return ret;
}

/* --------------------------------------------------------------------------
    Name: bcmemac_net_xmit
 Purpose: Send ethernet traffic
-------------------------------------------------------------------------- */
static int bcmemac_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	Enet_Tx_CB *txCBPtr;
	volatile DmaDesc *firstBdPtr;
	unsigned long flags;

	ASSERT(pDevCtrl != NULL);

	spin_lock_irqsave(&pDevCtrl->lock, flags);

	TRACE(("bcmemac_net_xmit, txCfg=%08x, txIntStat=%08x\n",
	       pDevCtrl->txDma->cfg, pDevCtrl->txDma->intStat));

	txCBPtr = txCb_deq(pDevCtrl);

	/*
	 * Obtain a transmit header from the free list.  If there are none
	 * available, we can't send the packet. Discard the packet.
	 */
	if (txCBPtr == NULL) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&pDevCtrl->lock, flags);
		return 1;
	}

	txCBPtr->nrBds = 1;
	txCBPtr->skb = skb;

	/* If we could not queue this packet, free it */
	if (pDevCtrl->txFreeBds < txCBPtr->nrBds) {
		TRACE(("%s: bcmemac_net_xmit low on txFreeBds\n", dev->name));
		txCb_enq(pDevCtrl, txCBPtr);
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&pDevCtrl->lock, flags);
		return 1;
	}

	firstBdPtr = pDevCtrl->txNextBdPtr;
	/* store off the last BD address used to enqueue the packet */
	txCBPtr->lastBdAddr = pDevCtrl->txNextBdPtr;
	txCBPtr->dma_addr = dma_map_single(&pDevCtrl->dev->dev, skb->data,
					   skb->len, DMA_TO_DEVICE);

	/* assign skb data to TX Dma descriptor */
	/*
	 * Add the buffer to the ring.
	 * Set addr and length of DMA BD to be transmitted.
	 */

	pDevCtrl->txNextBdPtr->address = EMAC_SWAP32(txCBPtr->dma_addr);
	pDevCtrl->txNextBdPtr->length_status =
	    EMAC_SWAP32((((unsigned long)((skb->len < ETH_ZLEN) ? ETH_ZLEN :
					  skb->len)) << 16));
	/*
	 * Set status of DMA BD to be transmitted and
	 * advance BD pointer to next in the chain.
	 */
	if (pDevCtrl->txNextBdPtr == pDevCtrl->txLastBdPtr) {
		pDevCtrl->txNextBdPtr->length_status |= EMAC_SWAP32(DMA_WRAP);
		pDevCtrl->txNextBdPtr = pDevCtrl->txFirstBdPtr;
	} else {
		pDevCtrl->txNextBdPtr->length_status |= EMAC_SWAP32(0);
		pDevCtrl->txNextBdPtr++;
	}
#ifdef DUMP_DATA
	printk("bcmemac_net_xmit: len %d", skb->len);
	dumpHexData(skb->data, skb->len);
#endif
	/*
	 * Turn on the "OWN" bit in the first buffer descriptor
	 * This tells the switch that it can transmit this frame.
	 */
	firstBdPtr->length_status |=
	    EMAC_SWAP32(DMA_OWN | DMA_SOP | DMA_EOP | DMA_APPEND_CRC);

	/* Decrement total BD count */
	pDevCtrl->txFreeBds -= txCBPtr->nrBds;

	if ((pDevCtrl->txFreeBds == 0) || (pDevCtrl->txCbQHead == NULL)) {
		TRACE(("%s: bcmemac_net_xmit no transmit queue space -- stopping queues\n", dev->name));
		netif_stop_queue(dev);
	}
	/*
	 * Packet was enqueued correctly.
	 * Advance to the next Enet_Tx_CB needing to be assigned to a BD
	 */
	txCBPtr->next = NULL;
	if (pDevCtrl->txCbPtrHead == NULL) {
		pDevCtrl->txCbPtrHead = txCBPtr;
		pDevCtrl->txCbPtrTail = txCBPtr;
	} else {
		pDevCtrl->txCbPtrTail->next = txCBPtr;
		pDevCtrl->txCbPtrTail = txCBPtr;
	}

	/* Enable DMA for this channel */
	pDevCtrl->txDma->cfg |= DMA_ENABLE;

	/* update stats */
	dev->stats.tx_bytes += ((skb->len < ETH_ZLEN) ? ETH_ZLEN : skb->len);
	dev->stats.tx_bytes += 4;
	dev->stats.tx_packets++;

	dev->trans_start = jiffies;

	spin_unlock_irqrestore(&pDevCtrl->lock, flags);

	return 0;
}

/* --------------------------------------------------------------------------
    Name: bcmemac_xmit_fragment
 Purpose: Send ethernet traffic Buffer DESC and submit to UDMA
-------------------------------------------------------------------------- */
int bcmemac_xmit_fragment(int ch, unsigned char *buf, int buf_len,
			  unsigned long tx_flags, struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	Enet_Tx_CB *txCBPtr;
	volatile DmaDesc *firstBdPtr;

	ASSERT(pDevCtrl != NULL);
	txCBPtr = txCb_deq(pDevCtrl);
	/*
	 * Obtain a transmit header from the free list.  If there are none
	 * available, we can't send the packet. Discard the packet.
	 */
	if (txCBPtr == NULL) {
		return 1;
	}

	txCBPtr->nrBds = 1;
	txCBPtr->skb = NULL;

	/* If we could not queue this packet, free it */
	if (pDevCtrl->txFreeBds < txCBPtr->nrBds) {
		TRACE(("%s: bcmemac_net_xmit low on txFreeBds\n", dev->name));
		txCb_enq(pDevCtrl, txCBPtr);
		return 1;
	}

	/*--------first fragment------*/
	firstBdPtr = pDevCtrl->txNextBdPtr;
	/* store off the last BD address used to enqueue the packet */
	txCBPtr->lastBdAddr = pDevCtrl->txNextBdPtr;
	txCBPtr->dma_addr = dma_map_single(&pDevCtrl->dev->dev, buf, buf_len,
					   DMA_TO_DEVICE);

	/* assign skb data to TX Dma descriptor */
	/*
	 * Add the buffer to the ring.
	 * Set addr and length of DMA BD to be transmitted.
	 */
	pDevCtrl->txNextBdPtr->address = EMAC_SWAP32(txCBPtr->dma_addr);
	pDevCtrl->txNextBdPtr->length_status =
	    EMAC_SWAP32((((unsigned long)buf_len) << 16));
	/*
	 * Set status of DMA BD to be transmitted and
	 * advance BD pointer to next in the chain.
	 */
	if (pDevCtrl->txNextBdPtr == pDevCtrl->txLastBdPtr) {
		pDevCtrl->txNextBdPtr->length_status |= EMAC_SWAP32(DMA_WRAP);
		pDevCtrl->txNextBdPtr = pDevCtrl->txFirstBdPtr;
	} else {
		pDevCtrl->txNextBdPtr++;
	}

	/*
	 * Turn on the "OWN" bit in the first buffer descriptor
	 * This tells the switch that it can transmit this frame.
	 */
	firstBdPtr->length_status &=
	    ~EMAC_SWAP32(DMA_SOP | DMA_EOP | DMA_APPEND_CRC);
	firstBdPtr->length_status |=
	    EMAC_SWAP32(DMA_OWN | tx_flags | DMA_APPEND_CRC);

	/* Decrement total BD count */
	pDevCtrl->txFreeBds -= txCBPtr->nrBds;

	/*
	 * Packet was enqueued correctly.
	 * Advance to the next Enet_Tx_CB needing to be assigned to a BD
	 */
	txCBPtr->next = NULL;
	if (pDevCtrl->txCbPtrHead == NULL) {
		pDevCtrl->txCbPtrHead = txCBPtr;
		pDevCtrl->txCbPtrTail = txCBPtr;
	} else {
		pDevCtrl->txCbPtrTail->next = txCBPtr;
		pDevCtrl->txCbPtrTail = txCBPtr;
	}

	/* Enable DMA for this channel */
	pDevCtrl->txDma->cfg |= DMA_ENABLE;

	/* update stats */
	dev->stats.tx_bytes += buf_len;	//((skb->len < ETH_ZLEN) ? ETH_ZLEN : skb->len);
	dev->stats.tx_bytes += 4;
	dev->stats.tx_packets++;

	dev->trans_start = jiffies;

	return 0;
}

EXPORT_SYMBOL(bcmemac_xmit_fragment);

/* --------------------------------------------------------------------------
    Name: bcmemac_xmit_multibuf
 Purpose: Send ethernet traffic in multi buffers (hdr, buf, tail)
-------------------------------------------------------------------------- */
int bcmemac_xmit_multibuf(int ch, unsigned char *hdr, int hdr_len,
			  unsigned char *buf, int buf_len, unsigned char *tail,
			  int tail_len, struct net_device *dev)
{
	unsigned long flags;
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);

	while (bcmemac_xmit_check(dev)) ;

	/*
	 * Obtain exclusive access to transmitter.  This is necessary because
	 * we might have more than one stack transmitting at once.
	 */
	spin_lock_irqsave(&pDevCtrl->lock, flags);

	/* Header + Optional payload in two parts */
	if ((hdr_len > 0) && (buf_len > 0) && (tail_len > 0) && (hdr) && (buf)
	    && (tail)) {
		/* Send Header */
		while (bcmemac_xmit_fragment(ch, hdr, hdr_len, DMA_SOP, dev))
			bcmemac_xmit_check(dev);
		/* Send First Fragment */
		while (bcmemac_xmit_fragment(ch, buf, buf_len, 0, dev))
			bcmemac_xmit_check(dev);
		/* Send 2nd Fragment */
		while (bcmemac_xmit_fragment(ch, tail, tail_len, DMA_EOP, dev))
			bcmemac_xmit_check(dev);
	}
	/* Header + Optional payload */
	else if ((hdr_len > 0) && (buf_len > 0) && (hdr) && (buf)) {
		/* Send Header */
		while (bcmemac_xmit_fragment(ch, hdr, hdr_len, DMA_SOP, dev))
			bcmemac_xmit_check(dev);
		/* Send First Fragment */
		while (bcmemac_xmit_fragment(ch, buf, buf_len, DMA_EOP, dev))
			bcmemac_xmit_check(dev);
	}
	/* Header Only (includes payload) */
	else if ((hdr_len > 0) && (hdr)) {
		/* Send Header */
		while (bcmemac_xmit_fragment
		       (ch, hdr, hdr_len, DMA_SOP | DMA_EOP, dev))
			bcmemac_xmit_check(dev);
	} else {
		spin_unlock_irqrestore(&pDevCtrl->lock, flags);
		return 0;	/* Drop the packet */
	}
	spin_unlock_irqrestore(&pDevCtrl->lock, flags);
	return 0;
}

// PR5760 - if there are too many packets with the overflow bit set,
// then reset the EMAC.  We arrived at a threshold of 8 packets based
// on empirical analysis and testing (smaller values are too aggressive
// and larger values wait too long).
static void ResetEMAConErrors(BcmEnet_devctrl * pDevCtrl)
{
#if 0 /* disable for now */
	unsigned long flags;

	spin_lock_irqsave(&pDevCtrl->lock, flags);

	// Clear the overflow counter.
	// Set the disable bit, wait for h/w to clear it, then set
	// the enable bit.
	pDevCtrl->emac->config |= EMAC_DISABLE;
	while (pDevCtrl->emac->config & EMAC_DISABLE) ;
	pDevCtrl->emac->config |= EMAC_ENABLE;

	spin_unlock_irqrestore(&pDevCtrl->lock, flags);
#endif
}

static int bcmemac_enet_poll(struct napi_struct *napi, int budget)
{
	BcmEnet_devctrl *pDevCtrl =
	    container_of(napi, struct BcmEnet_devctrl, napi);
	int work_done, reclaimed;

	/* clear pending IRQs for the packets we are about to process */
	pDevCtrl->rxDma->intStat = DMA_DONE;
	pDevCtrl->txDma->intStat = DMA_DONE;

	work_done = bcmemac_rx(pDevCtrl, budget);
	assign_rx_buffers(pDevCtrl);
	reclaimed = bcmemac_xmit_check(pDevCtrl->dev);

	if ((work_done < budget) && !reclaimed) {
		unsigned long flags;

		napi_complete(napi);

		spin_lock_irqsave(&pDevCtrl->lock, flags);
		pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk |= R5K_IUDMA_IRQ;
		spin_unlock_irqrestore(&pDevCtrl->lock, flags);
	}

	return work_done;
}

/*
 * bcmemac_net_isr: Acknowledge interrupts and check if any packets have
 *                  arrived
 */
static irqreturn_t bcmemac_net_isr(int irq, void *dev_id)
{
	BcmEnet_devctrl *pDevCtrl = dev_id;
	unsigned long flags;

	/*
	 * Disable IRQ and use NAPI polling loop.  IRQ will be re-enabled
	 * after all packets are processed.
	 */
	if (likely(pDevCtrl->dmaRegs->enet_iudma_r5k_irq_sts & R5K_IUDMA_IRQ)) {
		spin_lock_irqsave(&pDevCtrl->lock, flags);
		pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk &=
			~R5K_IUDMA_IRQ;
		spin_unlock_irqrestore(&pDevCtrl->lock, flags);

		if (likely(napi_schedule_prep(&pDevCtrl->napi)))
			__napi_schedule(&pDevCtrl->napi);
	}

	if (unlikely(pDevCtrl->emac->intStatus & EMAC_MDIO_INT)) {
		pDevCtrl->emac->intStatus = EMAC_MDIO_INT;
		complete(&pDevCtrl->mdio_complete);
	}

	return IRQ_HANDLED;
}

/*
 *  bcmemac_rx: Process all received packets.
 */
static int bcmemac_rx(BcmEnet_devctrl * pDevCtrl, int budget)
{
	int pkts = 0, overflows = 0;
	volatile DmaDesc *desc;
	struct net_device *dev = pDevCtrl->dev;

	while (1) {
		u32 stat, addr, size;
		Enet_Tx_CB *cb;

		if (pkts >= budget)
			break;

		desc = pDevCtrl->rxBdReadPtr;
		stat = EMAC_SWAP32(desc->length_status);
		addr = EMAC_SWAP32(desc->address);

		if (((stat & DMA_OWN) != 0) || addr == 0)
			break;
		if ((stat & DMA_EOP) == 0)
			printk(KERN_WARNING DRV_NAME ": fragments not "
			       "supported\n");

		pkts++;
		desc->address = 0;

		if (desc == pDevCtrl->rxLastBdPtr) {
			pDevCtrl->rxBdReadPtr = pDevCtrl->rxFirstBdPtr;
		} else {
			pDevCtrl->rxBdReadPtr = desc + 1;
		}

		cb = &pDevCtrl->rxCbs[desc - pDevCtrl->rxBds];
		dma_unmap_single(&dev->dev, cb->dma_addr,
				 cb->dma_len, DMA_FROM_DEVICE);

		size = (stat & DMA_DESC_BUFLENGTH) >> 16;
		BUG_ON(cb->skb == NULL);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += size;

		if (unlikely(stat & (EMAC_CRC_ERROR | EMAC_OV | EMAC_NO |
				     EMAC_LG | EMAC_RXER))) {
			if (stat & EMAC_CRC_ERROR)
				dev->stats.rx_crc_errors++;
			if (stat & EMAC_NO)
				dev->stats.rx_frame_errors++;
			if (stat & EMAC_LG)
				dev->stats.rx_length_errors++;
			if (stat & EMAC_OV) {
				dev->stats.rx_over_errors++;
				overflows++;
			}
			dev->stats.rx_dropped++;
			dev->stats.rx_errors++;

			dev_kfree_skb_any(cb->skb);
			cb->skb = NULL;
			continue;
		}

		skb_put(cb->skb, size);
		if (pDevCtrl->bIPHdrOptimize)
			skb_trim(cb->skb, size - ETH_FCS_LEN - 2);
		cb->skb->protocol = eth_type_trans(cb->skb, dev);
		netif_receive_skb(cb->skb);
		cb->skb = NULL;
	}

	if (overflows)
		ResetEMAConErrors(pDevCtrl);
	return pkts;
}

/*
 * Set the hardware MAC address.
 */
static int bcm_set_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (netif_running(dev))
		return -EBUSY;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);

	write_mac_address(dev);

	return 0;
}

/*
 * write_mac_address: store MAC address into EMAC perfect match registers                   
 */
void write_mac_address(struct net_device *dev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);
	volatile u32 data32bit;

	ASSERT(pDevCtrl != NULL);

	data32bit = pDevCtrl->emac->config;
	if (data32bit & EMAC_ENABLE) {
		/* disable ethernet MAC while updating its registers */
		pDevCtrl->emac->config &= ~EMAC_ENABLE;
		pDevCtrl->emac->config |= EMAC_DISABLE;
		while (pDevCtrl->emac->config & EMAC_DISABLE) ;
	}

	/* add our MAC address to the perfect match register */
	perfectmatch_update(netdev_priv(dev), dev->dev_addr, 1);

	if (data32bit & EMAC_ENABLE) {
		pDevCtrl->emac->config = data32bit;
	}
}

/*
 * assign_rx_buffers: Beginning with the first receive buffer descriptor
 *                  used to receive a packet since this function last
 *                  assigned a buffer, reassign buffers to receive
 *                  buffer descriptors, and mark them as EMPTY; available
 *                  to be used again.
 *
 */

#define BUF_ALIGN		0x100

static int assign_rx_buffers(BcmEnet_devctrl * pDevCtrl)
{
	int filled = 0;

	if (atomic_dec_if_positive(&pDevCtrl->rxbuf_assign_idle))
		return 0;

	while (pDevCtrl->rxBdAssignPtr->address == 0) {
		volatile DmaDesc *p = pDevCtrl->rxBdAssignPtr;
		struct sk_buff *skb;
		Enet_Tx_CB *cb = &pDevCtrl->rxCbs[p - pDevCtrl->rxBds];

		cb->dma_len = pDevCtrl->rxBufLen;
		skb = netdev_alloc_skb(pDevCtrl->dev, cb->dma_len + BUF_ALIGN);

		if (!skb) {
			/* no skb's to be found, try again later */
			atomic_set(&pDevCtrl->rxDmaRefill, 1);
			break;
		}

		/* align DMA to 256B boundary */
		skb_reserve(skb, (-(unsigned long)skb->data) & (BUF_ALIGN - 1));

		cb->skb = skb;
		cb->dma_addr = dma_map_single(&pDevCtrl->dev->dev,
					      skb->data, cb->dma_len,
					      DMA_FROM_DEVICE);

		if (pDevCtrl->bIPHdrOptimize)
			skb_reserve(skb, 2);

		p->address = EMAC_SWAP32(cb->dma_addr);

		if (p == pDevCtrl->rxLastBdPtr) {
			p->length_status = EMAC_SWAP32((cb->dma_len << 16) |
						       DMA_OWN | DMA_WRAP);
			pDevCtrl->rxBdAssignPtr = pDevCtrl->rxFirstBdPtr;
		} else {
			p->length_status = EMAC_SWAP32((cb->dma_len << 16) |
						       DMA_OWN);
			(pDevCtrl->rxBdAssignPtr)++;
		}
		filled++;
	}

	pDevCtrl->rxDma->cfg |= DMA_ENABLE;
	atomic_set(&pDevCtrl->rxDmaRefill, 0);
	atomic_inc(&pDevCtrl->rxbuf_assign_idle);
	return filled;
}

/*
 * perfectmatch_write: write an address to the EMAC perfect match registers
 */
static void perfectmatch_write(volatile EmacRegisters * emac,
	unsigned int reg, const u8 * pAddr, bool bValid)
{
	volatile unsigned long *pmDataLo, *pmDataHi;

	BUG_ON(reg > 7);
	reg <<= 1;
	pmDataLo = &emac->pm0DataLo + reg;
	pmDataHi = &emac->pm0DataHi + reg;

	/* Fill DataHi/Lo */
	if (bValid == 1)
		*pmDataLo = MAKE4((pAddr + 2));
	*pmDataHi = MAKE2(pAddr) | (bValid << 16);
}

/*
 * perfectmatch_update: update an address to the EMAC perfect match registers
 */
static void perfectmatch_update(BcmEnet_devctrl * pDevCtrl, const u8 * pAddr,
				bool bValid)
{
	int i;
	unsigned int aged_ref;
	int aged_entry;

	/* check if this address is in used */
	for (i = 0; i < MAX_PMADDR; i++) {
		if (pDevCtrl->pmAddr[i].valid == 1) {
			if (memcmp(pDevCtrl->pmAddr[i].dAddr, pAddr, ETH_ALEN)
			    == 0) {
				if (bValid == 0) {
					/* clear the valid bit in PM register */
					perfectmatch_write(pDevCtrl->emac, i,
							   pAddr, bValid);
					/* clear the valid bit in pDevCtrl->pmAddr */
					pDevCtrl->pmAddr[i].valid = bValid;
				} else {
					pDevCtrl->pmAddr[i].ref++;
				}
				return;
			}
		}
	}
	if (bValid == 1) {
		for (i = 0; i < MAX_PMADDR; i++) {
			/* find an available entry for write pm address */
			if (pDevCtrl->pmAddr[i].valid == 0) {
				break;
			}
		}
		if (i == MAX_PMADDR) {
			aged_ref = 0xFFFFFFFF;
			aged_entry = 0;
			/* aged out an entry */
			for (i = 0; i < MAX_PMADDR; i++) {
				if (pDevCtrl->pmAddr[i].ref < aged_ref) {
					aged_ref = pDevCtrl->pmAddr[i].ref;
					aged_entry = i;
				}
			}
			i = aged_entry;
		}
		/* find a empty entry and add the address */
		perfectmatch_write(pDevCtrl->emac, i, pAddr, bValid);

		/* update the pDevCtrl->pmAddr */
		pDevCtrl->pmAddr[i].valid = bValid;
		memcpy(pDevCtrl->pmAddr[i].dAddr, pAddr, ETH_ALEN);
		pDevCtrl->pmAddr[i].ref = 1;
	}
}

#ifdef MULTICAST_HW_FILTER
/*
 * perfectmatch_clean: Clear EMAC perfect match registers not matched by pAddr (PR10861)
 */
static void perfectmatch_clean(BcmEnet_devctrl * pDevCtrl, const u8 * pAddr)
{
	int i;

	/* check if this address is in used */
	for (i = 0; i < MAX_PMADDR; i++) {
		if (pDevCtrl->pmAddr[i].valid == 1) {
			if (memcmp(pDevCtrl->pmAddr[i].dAddr, pAddr, ETH_ALEN)
			    != 0) {
				/* clear the valid bit in PM register */
				perfectmatch_write(pDevCtrl->emac, i, pAddr, 0);
				/* clear the valid bit in pDevCtrl->pmAddr */
				pDevCtrl->pmAddr[i].valid = 0;
			}
		}
	}
}
#endif

/*
 * clear_mib: Initialize the Ethernet Switch MIB registers
 */
static void clear_mib(volatile EmacRegisters * emac)
{
	int i;
	volatile u32 *pt;

	pt = (u32 *) & emac->tx_mib;
	for (i = 0; i < NumEmacTxMibVars; i++) {
		*pt++ = 0;
	}

	pt = (u32 *) & emac->rx_mib;;
	for (i = 0; i < NumEmacRxMibVars; i++) {
		*pt++ = 0;
	}
}

/*
 * init_emac: Initializes the Ethernet Switch control registers
 */
static int init_emac(BcmEnet_devctrl * pDevCtrl)
{
	volatile EmacRegisters *emac;
	unsigned long phy_cfg;

	TRACE(("bcmemacenet: init_emac\n"));

	pDevCtrl->emac =
	    (volatile EmacRegisters * const)(pDevCtrl->dev->base_addr);
	emac = pDevCtrl->emac;

	if (pDevCtrl->phy_type == BRCM_PHY_TYPE_INT) {
		/* use internal PHY, disable reclock */
		phy_cfg = 0;
		/* NOTE: this register only exists on EMAC_0 */
		pDevCtrl->dmaRegs->enet_iudma_tstctl &= ~(3 << 12);
	} else {
		/* use external PHY, enable reclock */
		phy_cfg = EMAC_EXT_PHY;
		pDevCtrl->dmaRegs->enet_iudma_tstctl |= (1 << 12);
		pDevCtrl->dmaRegs->enet_iudma_tstctl &= ~(1 << 13);
	}

	/* Initialize the Ethernet Switch MIB registers */
	clear_mib(pDevCtrl->emac);

	/* disable ethernet MAC while updating its registers */
	emac->config = EMAC_DISABLE;
	while (emac->config & EMAC_DISABLE) ;

	/* issue soft reset, wait for it to complete */
	emac->config = EMAC_SOFT_RESET;
	while (emac->config & EMAC_SOFT_RESET) ;

	emac->config = phy_cfg;

	/* Initialize emac registers */
	/* Ethernet header optimization, reserve 2 bytes at head of packet */
	if (pDevCtrl->bIPHdrOptimize) {
		unsigned int packetOffset = (2 << RX_CONFIG_PKT_OFFSET_SHIFT);
		emac->rxControl = EMAC_FC_EN | EMAC_UNIFLOW | packetOffset;
	} else {
		emac->rxControl = EMAC_FC_EN | EMAC_UNIFLOW;	// THT dropped for RR support | EMAC_PROM;   // allow Promiscuous
	}

#ifdef MAC_LOOPBACK
	emac->rxControl |= EMAC_LOOPBACK;
#endif
	emac->rxMaxLength =
	    ENET_MAX_MTU_SIZE + (pDevCtrl->bIPHdrOptimize ? 2 : 0);
	emac->txMaxLength = ENET_MAX_MTU_SIZE;

	/* tx threshold = abs(63-(0.63*EMAC_DMA_MAX_BURST_LENGTH)) */
	emac->txThreshold = EMAC_TX_WATERMARK;	// THT PR12238 as per David Ferguson: Turning off RR, Was 32
	emac->mibControl = EMAC_NO_CLEAR;
	emac->intMask = 0;	/* mask all EMAC interrupts */
	emac->intStatus = 0x7;
	emac->txControl = EMAC_FD;

	TRACE(("done init emac\n"));

	return 0;
}

/*
 * init_dma: Initialize DMA control register
 */
static void init_IUdma(BcmEnet_devctrl * pDevCtrl)
{
	TRACE(("bcmemacenet: init_dma\n"));

	/*
	 * initialize IUDMA controller register
	 */
	//pDevCtrl->dmaRegs->controller_cfg = DMA_FLOWC_CH1_EN;
	pDevCtrl->dmaRegs->controller_cfg = 0;
	pDevCtrl->dmaRegs->flowctl_ch1_thresh_lo = DMA_FC_THRESH_LO;
	pDevCtrl->dmaRegs->flowctl_ch1_thresh_hi = DMA_FC_THRESH_HI;

	// transmit
	pDevCtrl->txDma->cfg = 0;	/* initialize first (will enable later) */
	pDevCtrl->txDma->maxBurst = DMA_MAX_BURST_LENGTH;	/*DMA_MAX_BURST_LENGTH; */

	pDevCtrl->txDma->intMask = 0;	/* mask all ints */
	/* clr any pending interrupts on channel */
	pDevCtrl->txDma->intStat = DMA_DONE | DMA_NO_DESC | DMA_BUFF_DONE;
	/* set to interrupt on packet complete */
	pDevCtrl->txDma->intMask = DMA_DONE;

	pDevCtrl->txDma->descPtr = (u32) CPHYSADDR(pDevCtrl->txFirstBdPtr);

	// receive
	pDevCtrl->rxDma->cfg = 0;	// initialize first (will enable later)
	pDevCtrl->rxDma->maxBurst = DMA_MAX_BURST_LENGTH;	/*DMA_MAX_BURST_LENGTH; */

	pDevCtrl->rxDma->descPtr = (u32) CPHYSADDR(pDevCtrl->rxFirstBdPtr);
	pDevCtrl->rxDma->intMask = 0;	/* mask all ints */
	/* clr any pending interrupts on channel */
	pDevCtrl->rxDma->intStat = DMA_DONE | DMA_NO_DESC | DMA_BUFF_DONE;
	/* set to interrupt on packet complete and no descriptor available */
	pDevCtrl->rxDma->intMask = DMA_DONE;	//|DMA_NO_DESC;
}

/*
 *  init_buffers: initialize driver's pools of receive buffers
 *  and tranmit headers
 */
static int init_buffers(BcmEnet_devctrl * pDevCtrl)
{
/* set initial state of all BD pointers to top of BD ring */
	pDevCtrl->txCbPtrHead = pDevCtrl->txCbPtrTail = NULL;

	atomic_set(&pDevCtrl->rxDmaRefill, 0);
	atomic_set(&pDevCtrl->rxbuf_assign_idle, 1);

	/* assign packet buffers to all available Dma descriptors */
	assign_rx_buffers(pDevCtrl);
	pDevCtrl->dmaRegs->flowctl_ch1_alloc = IUDMA_CH1_FLOW_ALLOC_FORCE |
	    NR_RX_BDS;

	return 0;
}

/*
 * bcmemac_init_dev: initialize Ethernet Switch device,
 * allocate Tx/Rx buffer descriptors pool, Tx control block pool.
 */
static int bcmemac_init_dev(BcmEnet_devctrl * pDevCtrl)
{
	int i;
	int nrCbs;
	void *p;
	Enet_Tx_CB *txCbPtr;

	/* setup buffer/pointer relationships here */
	pDevCtrl->nrTxBds = NR_TX_BDS;
	pDevCtrl->nrRxBds = NR_RX_BDS;
	pDevCtrl->rxBufLen =
	    ENET_MAX_MTU_SIZE + (pDevCtrl->bIPHdrOptimize ? 2 : 0);

	/* init rx/tx dma channels */
	pDevCtrl->dmaRegs =
	    (DmaRegs *) (pDevCtrl->dev->base_addr + EMAC_DMA_OFFSET);
	pDevCtrl->rxDma = &pDevCtrl->dmaRegs->chcfg[EMAC_RX_CHAN];
	pDevCtrl->txDma = &pDevCtrl->dmaRegs->chcfg[EMAC_TX_CHAN];
	pDevCtrl->rxBds =
	    (DmaDesc *) (pDevCtrl->dev->base_addr + EMAC_RX_DESC_OFFSET);
	pDevCtrl->txBds =
	    (DmaDesc *) (pDevCtrl->dev->base_addr + EMAC_TX_DESC_OFFSET);

	/* alloc space for the tx control block pool */
	nrCbs = pDevCtrl->nrTxBds;
	if (!(p = kmalloc(nrCbs * sizeof(Enet_Tx_CB), GFP_KERNEL))) {
		return -ENOMEM;
	}
	memset(p, 0, nrCbs * sizeof(Enet_Tx_CB));
	pDevCtrl->txCbs = (Enet_Tx_CB *) p;	/* tx control block pool */

	if (!(p = kmalloc(pDevCtrl->nrRxBds * sizeof(Enet_Tx_CB), GFP_KERNEL))) {
		kfree(pDevCtrl->txCbs);
		return -ENOMEM;
	}
	memset(p, 0, pDevCtrl->nrRxBds * sizeof(Enet_Tx_CB));
	pDevCtrl->rxCbs = (void *)p;

	/* initialize rx ring pointer variables. */
	pDevCtrl->rxBdAssignPtr = pDevCtrl->rxBdReadPtr =
	    pDevCtrl->rxFirstBdPtr = pDevCtrl->rxBds;
	pDevCtrl->rxLastBdPtr = pDevCtrl->rxFirstBdPtr + pDevCtrl->nrRxBds - 1;

	/* init the receive buffer descriptor ring */
	for (i = 0; i < pDevCtrl->nrRxBds; i++) {
		(pDevCtrl->rxFirstBdPtr + i)->length_status =
		    EMAC_SWAP32((pDevCtrl->rxBufLen << 16));
		(pDevCtrl->rxFirstBdPtr + i)->address = EMAC_SWAP32(0);
	}
	pDevCtrl->rxLastBdPtr->length_status |= EMAC_SWAP32(DMA_WRAP);

	/* init transmit buffer descriptor variables */
	pDevCtrl->txNextBdPtr = pDevCtrl->txFirstBdPtr = pDevCtrl->txBds;
	pDevCtrl->txLastBdPtr = pDevCtrl->txFirstBdPtr + pDevCtrl->nrTxBds - 1;

	/* clear the transmit buffer descriptors */
	for (i = 0; i < pDevCtrl->nrTxBds; i++) {
		(pDevCtrl->txFirstBdPtr + i)->length_status =
		    EMAC_SWAP32((0 << 16));
		(pDevCtrl->txFirstBdPtr + i)->address = EMAC_SWAP32(0);
	}
	pDevCtrl->txLastBdPtr->length_status |= EMAC_SWAP32(DMA_WRAP);
	pDevCtrl->txFreeBds = pDevCtrl->nrTxBds;

	/* initialize the receive buffers and transmit headers */
	if (init_buffers(pDevCtrl)) {
		kfree((void *)pDevCtrl->txCbs);
		kfree((void *)pDevCtrl->rxCbs);
		return -ENOMEM;
	}

	for (i = 0; i < nrCbs; i++) {
		txCbPtr = pDevCtrl->txCbs + i;
		txCb_enq(pDevCtrl, txCbPtr);
	}

	/* init dma registers */
	init_IUdma(pDevCtrl);

	/* init switch control registers */
	if (init_emac(pDevCtrl)) {
		kfree((void *)pDevCtrl->txCbs);
		kfree((void *)pDevCtrl->rxCbs);
		return -EFAULT;
	}
#ifdef IUDMA_INIT_WORKAROUND
	{
		unsigned long diag_rdbk;

		pDevCtrl->dmaRegs->enet_iudma_diag_ctl = 0x100;	/* enable to read diags. */
		diag_rdbk = pDevCtrl->dmaRegs->enet_iudma_diag_rdbk;

		pDevCtrl->rxBdAssignPtr = pDevCtrl->rxBdReadPtr =
		    pDevCtrl->rxFirstBdPtr + ((diag_rdbk >> 1) & DESC_MASK);
		pDevCtrl->txNextBdPtr =
		    pDevCtrl->txFirstBdPtr +
		    ((diag_rdbk >> (16 + 1)) & DESC_MASK);
	}
#endif

	/* if we reach this point, we've init'ed successfully */
	return 0;
}

/* Uninitialize tx/rx buffer descriptor pools */
static int bcmemac_uninit_dev(BcmEnet_devctrl * pDevCtrl)
{
	Enet_Tx_CB *txCBPtr;
	int i;

	if (pDevCtrl) {
		/* disable DMA and interrupts */
		pDevCtrl->txDma->cfg = 0;
		pDevCtrl->rxDma->cfg = 0;
		pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk = 0;

		/* free the skb in the txCbPtrHead */
		while (pDevCtrl->txCbPtrHead) {
			pDevCtrl->txFreeBds += pDevCtrl->txCbPtrHead->nrBds;

			if (pDevCtrl->txCbPtrHead->skb)
				dev_kfree_skb(pDevCtrl->txCbPtrHead->skb);

			txCBPtr = pDevCtrl->txCbPtrHead;

			/* Advance the current reclaim pointer */
			pDevCtrl->txCbPtrHead = pDevCtrl->txCbPtrHead->next;

			/* Finally, return the transmit header to the free list */
			txCb_enq(pDevCtrl, txCBPtr);
		}

		/* release allocated receive buffer memory */
		for (i = 0; i < NR_RX_BDS; i++) {
			if (pDevCtrl->rxCbs[i].skb != NULL) {
				dev_kfree_skb(pDevCtrl->rxCbs[i].skb);
				pDevCtrl->rxCbs[i].skb = NULL;
			}
		}
		/* free the transmit buffer descriptor */
		if (pDevCtrl->txBds) {
			pDevCtrl->txBds = NULL;
		}
		/* free the receive buffer descriptor */
		if (pDevCtrl->rxBds) {
			pDevCtrl->rxBds = NULL;
		}
		/* free the transmit control block pool */
		if (pDevCtrl->txCbs) {
			kfree(pDevCtrl->txCbs);
			pDevCtrl->txCbs = NULL;
		}
		if (pDevCtrl->rxCbs) {
			kfree(pDevCtrl->rxCbs);
			pDevCtrl->rxCbs = NULL;
		}
	}

	return 0;
}

static int bcmemac_enet_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(dev);

	return generic_mii_ioctl(&pDevCtrl->mii, if_mii(rq), cmd, NULL);
}

static int bcmemac_get_settings(struct net_device *netdev,
				struct ethtool_cmd *cmd)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(netdev);
	return mii_ethtool_gset(&pDevCtrl->mii, cmd);
}

static void bcmemac_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *info)
{
	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	strcpy(info->fw_version, "N/A");
	strcpy(info->bus_info, "platform");
}

static int bcmemac_nway_reset(struct net_device *netdev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(netdev);
	return mii_nway_restart(&pDevCtrl->mii);
}

static u32 bcmemac_get_link(struct net_device *netdev)
{
	BcmEnet_devctrl *pDevCtrl = netdev_priv(netdev);
	return mii_link_ok(&pDevCtrl->mii);
}

static const struct net_device_ops bcmemac_netdev_ops = {
	.ndo_open = bcmemac_net_open,
	.ndo_stop = bcmemac_net_close,
	.ndo_start_xmit = bcmemac_net_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_multicast_list = bcm_set_multicast_list,
	.ndo_set_mac_address = bcm_set_mac_addr,
	.ndo_do_ioctl = bcmemac_enet_ioctl,
	.ndo_tx_timeout = bcmemac_net_timeout,
};

static const struct ethtool_ops bcmemac_ethtool_ops = {
	.get_settings = bcmemac_get_settings,
	.get_drvinfo = bcmemac_get_drvinfo,
	.nway_reset = bcmemac_nway_reset,
	.get_link = bcmemac_get_link,
};

static int __devinit bcmemac_drv_probe(struct platform_device *pdev)
{
	struct bcmemac_platform_data *cfg = pdev->dev.platform_data;
	struct resource *res;
	struct net_device *dev;
	BcmEnet_devctrl *pDevCtrl;
	unsigned long res_size;
	int ret = -ENODEV;

	if (!cfg) {
		printk(KERN_WARNING DRV_NAME ": missing platform_data\n");
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		printk(KERN_WARNING DRV_NAME ": bad resource configuration\n");
		return -ENODEV;
	}
	res_size = 1 + res->end - res->start;

	if (!request_mem_region(res->start, res_size, DRV_NAME)) {
		printk(KERN_WARNING DRV_NAME ": can't request memory\n");
		return -ENODEV;
	}

	dev = alloc_etherdev(sizeof(*pDevCtrl));
	if (!dev) {
		printk(KERN_WARNING DRV_NAME ": can't alloc etherdev\n");
		goto out;
	}
	pDevCtrl = netdev_priv(dev);
	SET_NETDEV_DEV(dev, &pdev->dev);
	dev_set_drvdata(&pdev->dev, pDevCtrl);

	pDevCtrl->rxIrq = dev->irq = platform_get_irq(pdev, 0);
	pDevCtrl->dev = dev;
	pDevCtrl->phy_type = cfg->phy_type;

	pDevCtrl->mii.phy_id = cfg->phy_id;
	pDevCtrl->mii.phy_id_mask = 0x1f;
	pDevCtrl->mii.reg_num_mask = 0x1f;
	pDevCtrl->mii.dev = dev;
	pDevCtrl->mii.mdio_read = bcmemac_mdio_read;
	pDevCtrl->mii.mdio_write = bcmemac_mdio_write;

#if ! defined(CONFIG_BCM7038)
	pDevCtrl->bIPHdrOptimize = 1;
#endif

	pDevCtrl->mem_start = res->start;
	pDevCtrl->mem_size = res_size;

	dev->base_addr = (unsigned long)ioremap(res->start, res_size);
	memcpy(dev->dev_addr, cfg->macaddr, ETH_ALEN);

	if (bcmemac_init_dev(pDevCtrl) < 0)
		goto out2;

	pDevCtrl->dmaRegs->enet_iudma_r5k_irq_msk = R5K_EMAC_IRQ;
	pDevCtrl->emac->intStatus = EMAC_MDIO_INT;
	pDevCtrl->emac->intMask = EMAC_MDIO_INT;

	if (request_irq(pDevCtrl->rxIrq, bcmemac_net_isr, IRQF_DISABLED,
			dev->name, pDevCtrl) < 0) {
		printk(KERN_ERR DRV_NAME ": can't request IRQ\n");
		goto out3;
	}

	spin_lock_init(&pDevCtrl->lock);

	mutex_init(&pDevCtrl->mdio_mutex);
	init_completion(&pDevCtrl->mdio_complete);
	INIT_WORK(&pDevCtrl->link_status_work, bcmemac_link_status);

	init_timer(&pDevCtrl->timer);
	pDevCtrl->timer.data = (unsigned long)pDevCtrl;
	pDevCtrl->timer.function = bcmemac_link_timer;

	if (bcmemac_phy_init(pDevCtrl, cfg->phy_id) < 0) {
		printk(KERN_INFO DRV_NAME ": not registering interface #%d "
			"at 0x%08lx (no PHY detected)\n", pdev->id,
		       (unsigned long)res->start);
		goto out4;
	}

	dev->netdev_ops = &bcmemac_netdev_ops;
	SET_ETHTOOL_OPS(dev, &bcmemac_ethtool_ops);
	dev->watchdog_timeo = 2 * HZ;

	netif_napi_add(dev, &pDevCtrl->napi, bcmemac_enet_poll, NAPI_WEIGHT);

	write_mac_address(dev);

	// Let boot setup info overwrite default settings.
	netdev_boot_setup_check(dev);

	ret = register_netdev(dev);
	if (ret < 0) {
		printk(KERN_ERR DRV_NAME ": can't register netdev\n");
		goto out4;
	}

	printk(KERN_INFO DRV_NAME ": registered interface #%d at 0x%08lx "
		"as '%s' (%pM)\n", pdev->id, (unsigned long)res->start,
		dev->name, dev->dev_addr);

	pDevCtrl->next_dev = eth_root_dev;
	eth_root_dev = dev;

	return 0;

out4:
	free_irq(pDevCtrl->rxIrq, pDevCtrl);
out3:
	bcmemac_uninit_dev(pDevCtrl);
out2:
	iounmap((void *)dev->base_addr);
	free_netdev(dev);
out:
	release_mem_region(res->start, res_size);

	return ret;
}

static int __devexit bcmemac_drv_remove(struct platform_device *pdev)
{
	BcmEnet_devctrl *pDevCtrl = dev_get_drvdata(&pdev->dev);
	struct net_device *dev = pDevCtrl->dev;

	unregister_netdev(dev);
	free_irq(pDevCtrl->rxIrq, pDevCtrl);
	bcmemac_uninit_dev(pDevCtrl);
	iounmap((void *)dev->base_addr);
	release_mem_region(pDevCtrl->mem_start, pDevCtrl->mem_size);
	free_netdev(dev);

	return 0;
}

static struct platform_driver bcmemac_platform_driver = {
	.probe = bcmemac_drv_probe,
	.remove = __devexit_p(bcmemac_drv_remove),
	.driver = {
		   .name = DRV_NAME,
	},
};

static int __init bcmemac_module_init(void)
{
	printk(KERN_INFO DRV_NAME ": Broadcom STB 10/100 EMAC driver v"
	       DRV_VERSION "\n");
	return platform_driver_register(&bcmemac_platform_driver);
}

static void __exit bcmemac_module_cleanup(void)
{
	platform_driver_unregister(&bcmemac_platform_driver);
}

module_init(bcmemac_module_init);
module_exit(bcmemac_module_cleanup);

EXPORT_SYMBOL(bcmemac_xmit_multibuf);
EXPORT_SYMBOL(bcmemac_xmit_check);
EXPORT_SYMBOL(bcmemac_get_free_txdesc);
EXPORT_SYMBOL(bcmemac_get_device);

MODULE_AUTHOR("Broadcom Corporation");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
