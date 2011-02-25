/*
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
 */

#ifndef _BCMEMAC_H
#define _BCMEMAC_H

#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/types.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>

#include <asm/brcmstb/brcmstb.h>

#ifndef __ASSEMBLY__

/*
** DMA Channel Configuration
*/
typedef struct DmaChannel {
	unsigned long cfg;	/* (00) assorted configuration */
#define         DMA_BURST_HALT  0x00000040	/* idle after finish current memory burst */
#define         DMA_PKT_HALT    0x00000020	/* idle after an EOP flag is detected */
#define         DMA_ENABLE      0x00000001	/* set to enable channel */
	unsigned long intStat;	/* (04) interrupts control and status */
	unsigned long intMask;	/* (08) interrupts mask */
#define         DMA_BUFF_DONE   0x00000001	/* buffer done */
#define         DMA_DONE        0x00000002	/* packet xfer complete */
#define         DMA_NO_DESC     0x00000004	/* no valid descriptors */
	unsigned long maxBurst;	/* (0C) max burst length permitted */
	unsigned long descPtr;	/* (10) iudma base descriptor pointer */

	/* Unused words */
	unsigned long resv[27];
} DmaChannel;

/*
** DMA Registers
*/
typedef struct DmaRegs {
#define IUDMA_ENABLE           0x00000001
#define DMA_FLOWC_CH1_EN        0x00000002
/*
THT: Not defined on 7038 and 7318
#define DMA_FLOWC_CH3_EN        0x00000004
#define DMA_NUM_CHS_MASK        0x0f000000
#define DMA_NUM_CHS_SHIFT       24
#define DMA_FLOWCTL_MASK        0x30000000
#define DMA_FLOWCTL_CH1         0x10000000
#define DMA_FLOWCTL_CH3         0x20000000
#define DMA_FLOWCTL_SHIFT       28
*/
	unsigned long controller_cfg;	/* (00) controller configuration */

	/* Flow control */
	unsigned long flowctl_ch1_thresh_lo;	/* (04) EMAC1 RX DMA channel */
	unsigned long flowctl_ch1_thresh_hi;	/* (08) EMAC1 RX DMA channel */
	unsigned long flowctl_ch1_alloc;	/* (0C) EMAC1 RX DMA channel */
#define IUDMA_CH1_FLOW_ALLOC_FORCE 0x80000000	/* Bit 31 */

	unsigned long enet_iudma_rev;	/* (10) Enet rev             */
	unsigned long enet_iudma_tstctl;	/* (14) Enet test control    */
	unsigned long enet_iudma_pci_irq_sts;	/* (18) Enet pci intr status */
	unsigned long enet_iudma_pci_irq_msk;	/* (1C) Enet pci intr mask   */
#define R5K_SCB_IRQ		0x01
#define R5K_IUDMA_IRQ		0x02
#define R5K_EMAC_IRQ		0x04
#define R5K_EPHY_IRQ		0x08
#define R5K_GISB_IRQ		0x10
	unsigned long enet_iudma_r5k_irq_sts;	/* (20) Enet r5k intr status */
	unsigned long enet_iudma_r5k_irq_msk;	/* (24) Enet r5k intr mask   */
	unsigned long enet_iudma_diag_ctl;	/* (28) Enet diag control   */
	unsigned long enet_iudma_diag_rdbk;	/* (2C) Enet diag readback   */

	/* new registers added in 7405b0 */
	unsigned long enet_iudma_mii_select;	/* (30) Enet PHY select */
	unsigned long resv0[3];
	unsigned long enet_iudma_desc_alloc;	/* (40) Enet RX desc allocation */
	unsigned long enet_iudma_desc_thres;	/* (44) Enet RX desc threshold */
	unsigned long enet_iudma_desc_timeout;	/* (48) Enet RX desc timeout */
	unsigned long enet_iudma_desc_irq_sts;	/* (4c) Enet RX desc irq status */
	unsigned long enet_iudma_desc_irq_msk;	/* (50) Enet RX desc irq mask */

	/* Unused words */
	unsigned long resv1[43];

	/* Per channel registers/state ram */
	DmaChannel chcfg[2];	/* (100) Channel configuration */
} DmaRegs;

/*
** DMA Buffer 
*/
typedef struct DmaDesc {
	unsigned long length_status;	/* in bytes of data in buffer */
#define          DMA_DESC_USEFPM    0x80000000
#define          DMA_DESC_MULTICAST 0x40000000
#define          DMA_DESC_BUFLENGTH 0x0fff0000
//  unsigned short status;                 /* buffer status */
#define          DMA_OWN        0x8000	/* cleared by DMA, set by SW */
#define          DMA_EOP        0x4000	/* last buffer in packet */
#define          DMA_SOP        0x2000	/* first buffer in packet */
#define          DMA_WRAP       0x1000	/* */
#define          DMA_APPEND_CRC 0x0100

/* EMAC Descriptor Status definitions */
#define          EMAC_MISS      0x0080	/* framed address recognition failed (promiscuous) */
#define          EMAC_BRDCAST   0x0040	/* DA is Broadcast */
#define          EMAC_MULT      0x0020	/* DA is multicast */
#define          EMAC_LG        0x0010	/* frame length > RX_LENGTH register value */
#define          EMAC_NO        0x0008	/* Non-Octet aligned */
#define          EMAC_RXER      0x0004	/* RX_ERR on MII while RX_DV assereted */
#define          EMAC_CRC_ERROR 0x0002	/* CRC error */
#define          EMAC_OV        0x0001	/* Overflow */

/* HDLC Descriptor Status definitions */
#define          DMA_HDLC_TX_ABORT      0x0100
#define          DMA_HDLC_RX_OVERRUN    0x4000
#define          DMA_HDLC_RX_TOO_LONG   0x2000
#define          DMA_HDLC_RX_CRC_OK     0x1000
#define          DMA_HDLC_RX_ABORT      0x0100

	unsigned long address;	/* address of data */
} DmaDesc;

/*
** EMAC transmit MIB counters
*/
typedef struct EmacTxMib {
	unsigned long tx_good_octets;	/* (200) good byte count */
	unsigned long tx_good_pkts;	/* (204) good pkt count */
	unsigned long tx_octets;	/* (208) good and bad byte count */
	unsigned long tx_pkts;	/* (20c) good and bad pkt count */
	unsigned long tx_broadcasts_pkts;	/* (210) good broadcast packets */
	unsigned long tx_multicasts_pkts;	/* (214) good mulitcast packets */
	unsigned long tx_len_64;	/* (218) RMON tx pkt size buckets */
	unsigned long tx_len_65_to_127;	/* (21c) */
	unsigned long tx_len_128_to_255;	/* (220) */
	unsigned long tx_len_256_to_511;	/* (224) */
	unsigned long tx_len_512_to_1023;	/* (228) */
	unsigned long tx_len_1024_to_max;	/* (22c) */
	unsigned long tx_jabber_pkts;	/* (230) > 1518 with bad crc */
	unsigned long tx_oversize_pkts;	/* (234) > 1518 with good crc */
	unsigned long tx_fragment_pkts;	/* (238) < 63   with bad crc */
	unsigned long tx_underruns;	/* (23c) fifo underrun */
	unsigned long tx_total_cols;	/* (240) total collisions in all tx pkts */
	unsigned long tx_single_cols;	/* (244) tx pkts with single collisions */
	unsigned long tx_multiple_cols;	/* (248) tx pkts with multiple collisions */
	unsigned long tx_excessive_cols;	/* (24c) tx pkts with excessive cols */
	unsigned long tx_late_cols;	/* (250) tx pkts with late cols */
	unsigned long tx_defered;	/* (254) tx pkts deferred */
	unsigned long tx_carrier_lost;	/* (258) tx pkts with CRS lost */
	unsigned long tx_pause_pkts;	/* (25c) tx pause pkts sent */
#define NumEmacTxMibVars        24
} EmacTxMib;

/*
** EMAC receive MIB counters
*/
typedef struct EmacRxMib {
	unsigned long rx_good_octets;	/* (280) good byte count */
	unsigned long rx_good_pkts;	/* (284) good pkt count */
	unsigned long rx_octets;	/* (288) good and bad byte count */
	unsigned long rx_pkts;	/* (28c) good and bad pkt count */
	unsigned long rx_broadcasts_pkts;	/* (290) good broadcast packets */
	unsigned long rx_multicasts_pkts;	/* (294) good mulitcast packets */
	unsigned long rx_len_64;	/* (298) RMON rx pkt size buckets */
	unsigned long rx_len_65_to_127;	/* (29c) */
	unsigned long rx_len_128_to_255;	/* (2a0) */
	unsigned long rx_len_256_to_511;	/* (2a4) */
	unsigned long rx_len_512_to_1023;	/* (2a8) */
	unsigned long rx_len_1024_to_max;	/* (2ac) */
	unsigned long rx_jabber_pkts;	/* (2b0) > 1518 with bad crc */
	unsigned long rx_oversize_pkts;	/* (2b4) > 1518 with good crc */
	unsigned long rx_fragment_pkts;	/* (2b8) < 63   with bad crc */
	unsigned long rx_missed_pkts;	/* (2bc) missed packets */
	unsigned long rx_crc_align_errs;	/* (2c0) both or either */
	unsigned long rx_undersize;	/* (2c4) < 63   with good crc */
	unsigned long rx_crc_errs;	/* (2c8) crc errors (only) */
	unsigned long rx_align_errs;	/* (2cc) alignment errors (only) */
	unsigned long rx_symbol_errs;	/* (2d0) pkts with RXERR assertions (symbol errs) */
	unsigned long rx_pause_pkts;	/* (2d4) MAC control, PAUSE */
	unsigned long rx_nonpause_pkts;	/* (2d8) MAC control, not PAUSE */
#define NumEmacRxMibVars        23
} EmacRxMib;

typedef struct EmacRegisters {
	unsigned long rxControl;	/* (00) receive control */
#define          EMAC_PM_REJ    0x80	/*      - reject DA match in PMx regs */
#define          EMAC_UNIFLOW   0x40	/*      - accept cam match fc */
#define          EMAC_FC_EN     0x20	/*      - enable flow control */
#define          EMAC_LOOPBACK  0x10	/*      - loopback */
#define          EMAC_PROM      0x08	/*      - promiscuous */
#define          EMAC_RDT       0x04	/*      - ignore transmissions */
#define          EMAC_ALL_MCAST 0x02	/*      - ignore transmissions */
#define          EMAC_NO_BCAST  0x01	/*      - ignore transmissions */

	unsigned long rxMaxLength;	/* (04) receive max length */
	unsigned long txMaxLength;	/* (08) transmit max length */
	unsigned long unused1[1];
	unsigned long mdioFreq;	/* (10) mdio frequency */
#define          EMAC_MII_PRE_EN 0x00000080	/* prepend preamble sequence */
#define          EMAC_MDIO_PRE   0x00000080	/*      - enable MDIO preamble */
#define          EMAC_MDC_FREQ   0x0000007f	/*      - mdio frequency */

	unsigned long mdioData;	/* (14) mdio data */
#define          MDIO_WR        0x50020000	/*   - write framing */
#define          MDIO_RD        0x60020000	/*   - read framing */
#define          MDIO_PMD_SHIFT  23
#define          MDIO_REG_SHIFT  18

	unsigned long intMask;	/* (18) int mask */
	unsigned long intStatus;	/* (1c) int status */
#define          EMAC_FLOW_INT  0x04	/*      - flow control event */
#define          EMAC_MIB_INT   0x02	/*      - mib event */
#define          EMAC_MDIO_INT  0x01	/*      - mdio event */

	unsigned long unused2[3];
	unsigned long config;	/* (2c) config */
#define          EMAC_ENABLE    0x001	/*      - enable emac */
#define          EMAC_DISABLE   0x002	/*      - disable emac */
#define          EMAC_SOFT_RST  0x004	/*      - soft reset */
#define          EMAC_SOFT_RESET 0x004	/*      - emac soft reset */
#define          EMAC_EXT_PHY   0x008	/*      - external PHY select */

	unsigned long txControl;	/* (30) transmit control */
#define          EMAC_FD        0x001	/*      - full duplex */
#define          EMAC_FLOWMODE  0x002	/*      - flow mode */
#define          EMAC_NOBKOFF   0x004	/*      - no backoff in  */
#define          EMAC_SMALLSLT  0x008	/*      - small slot time */

	unsigned long txThreshold;	/* (34) transmit threshold */
	unsigned long mibControl;	/* (38) mib control */
#define          EMAC_NO_CLEAR  0x001	/* don't clear on read */

	unsigned long unused3[7];

	unsigned long pm0DataLo;	/* (58) perfect match 0 data lo */
	unsigned long pm0DataHi;	/* (5C) perfect match 0 data hi (15:0) */
	unsigned long pm1DataLo;	/* (60) perfect match 1 data lo */
	unsigned long pm1DataHi;	/* (64) perfect match 1 data hi (15:0) */
	unsigned long pm2DataLo;	/* (68) perfect match 2 data lo */
	unsigned long pm2DataHi;	/* (6C) perfect match 2 data hi (15:0) */
	unsigned long pm3DataLo;	/* (70) perfect match 3 data lo */
	unsigned long pm3DataHi;	/* (74) perfect match 3 data hi (15:0) */
	unsigned long pm4DataLo;	/* (78) perfect match 0 data lo */
	unsigned long pm4DataHi;	/* (7C) perfect match 0 data hi (15:0) */
	unsigned long pm5DataLo;	/* (80) perfect match 1 data lo */
	unsigned long pm5DataHi;	/* (84) perfect match 1 data hi (15:0) */
	unsigned long pm6DataLo;	/* (88) perfect match 2 data lo */
	unsigned long pm6DataHi;	/* (8C) perfect match 2 data hi (15:0) */
	unsigned long pm7DataLo;	/* (90) perfect match 3 data lo */
	unsigned long pm7DataHi;	/* (94) perfect match 3 data hi (15:0) */
#define          EMAC_CAM_V   0x10000	/*      - cam index */
#define          EMAC_CAM_VALID 0x00010000

	unsigned long unused4[90];	/* (98-1fc) */

	EmacTxMib tx_mib;	/* (200) emac tx mib */
	unsigned long unused5[8];	/* (260-27c) */

	EmacRxMib rx_mib;	/* (280) rx mib */

} EmacRegisters;

/* register offsets for subrouting access */
#define EMAC_RX_CONTROL         0x00
#define EMAC_RX_MAX_LENGTH      0x04
#define EMAC_TX_MAC_LENGTH      0x08
#define EMAC_MDIO_FREQ          0x10
#define EMAC_MDIO_DATA          0x14
#define EMAC_INT_MASK           0x18
#define EMAC_INT_STATUS         0x1C
#ifndef INCLUDE_DOCSIS_APP
/* Does not exist in the internal EMAC core int he bcm7110  */
#define EMAC_CAM_DATA_LO        0x20
#define EMAC_CAM_DATA_HI        0x24
#define EMAC_CAM_CONTROL        0x28
#endif
#define EMAC_CONTROL            0x2C
#define EMAC_TX_CONTROL         0x30
#define EMAC_TX_THRESHOLD       0x34
#define EMAC_MIB_CONTROL        0x38

#endif /* __ASSEMBLY__ */

#define EMAC_RX_CHAN               0
#define EMAC_TX_CHAN               1

#if !defined(CONFIG_BRCM_EMAC_128_DESC)

/* most chips have 256 descriptors (RDB: ENET_TOP_Descriptor[0..511]) */

#define TOTAL_DESC		256
#define RX_RATIO		1/2
#define EXTRA_TX_DESC		24
#define EMAC_RX_DESC_OFFSET	0x2800

#else

/* some only have 128 descriptors (RDB: ENET_TOP_Descriptor[0..255]) */

#define TOTAL_DESC		128
#define RX_RATIO		3/4
#define EXTRA_TX_DESC		24
#define EMAC_RX_DESC_OFFSET	0x2000

#endif

#define DESC_MASK		(TOTAL_DESC - 1)
#define NR_RX_BDS               ((TOTAL_DESC*RX_RATIO) - EXTRA_TX_DESC)
#define NR_TX_BDS               (TOTAL_DESC - NR_RX_BDS)

#define ENET_MAX_MTU_SIZE       1536	/* Body(1500) + EH_SIZE(14) + VLANTAG(4) + BRCMTAG(6) + FCS(4) = 1528.  1536 is multiple of 256 bytes */
#define DMA_MAX_BURST_LENGTH    0x40	/* in 32 bit words = 256 bytes  THT per David F, to allow 256B burst */

/* misc. configuration */
#define DMA_FC_THRESH_LO        5
#define DMA_FC_THRESH_HI        (NR_RX_BDS / 2)

#define EMAC_TX_DESC_OFFSET   	(EMAC_RX_DESC_OFFSET+(8*NR_RX_BDS))	/* MAC DMA Tx Descriptor word */

#define EMAC_DMA_OFFSET   		0x2400

#define ERROR(x)        printk x
#define ASSERT(x)       BUG_ON(!(x))

#define IUDMA_INIT_WORKAROUND		1

#if defined(DUMP_TRACE)
#define TRACE(x)        printk x
#else
#define TRACE(x)
#endif

typedef struct Enet_Tx_CB {
	struct sk_buff *skb;
	u32 nrBds;		/* # of bds req'd (incl 1 for this header) */
	volatile DmaDesc *lastBdAddr;
	dma_addr_t dma_addr;
	int dma_len;
	struct Enet_Tx_CB *next;	/* ptr to next header in free list */
} Enet_Tx_CB;

typedef struct PM_Addr {
	bool valid;		/* 1 indicates the corresponding address is valid */
	unsigned int ref;	/* reference count */
	unsigned char dAddr[ETH_ALEN];	/* perfect match register's destination address */
	char unused[2];		/* pad */
} PM_Addr;

#define MAX_PMADDR			8	/* # of perfect match address */

/*
 * device context
 */
typedef struct BcmEnet_devctrl {
	struct net_device *dev;	/* ptr to net_device */
	struct net_device *next_dev;
	struct napi_struct napi;

	struct mii_if_info mii;
	struct completion mdio_complete;
	struct mutex mdio_mutex;
	struct work_struct link_status_work;
	struct timer_list timer;

	spinlock_t lock;	/* Serializing lock */
	atomic_t rxDmaRefill;	/* rxDmaRefill == 1 needs refill rxDma */
	atomic_t rxbuf_assign_idle;

	volatile EmacRegisters *emac;	/* EMAC register base address */

	/* transmit variables */
	volatile DmaRegs *dmaRegs;
	volatile DmaChannel *txDma;	/* location of transmit DMA register set */

	Enet_Tx_CB *txCbPtrHead;	/* points to EnetTxCB struct head */
	Enet_Tx_CB *txCbPtrTail;	/* points to EnetTxCB struct tail */

	Enet_Tx_CB *txCbQHead;	/* points to EnetTxCB struct queue head */
	Enet_Tx_CB *txCbQTail;	/* points to EnetTxCB struct queue tail */
	Enet_Tx_CB *txCbs;	/* memory locaation of tx control block pool */

	volatile DmaDesc *txBds;	/* Memory location of tx Dma BD ring */
	volatile DmaDesc *txLastBdPtr;	/* ptr to last allocated Tx BD */
	volatile DmaDesc *txFirstBdPtr;	/* ptr to first allocated Tx BD */
	volatile DmaDesc *txNextBdPtr;	/* ptr to next Tx BD to transmit with */

	int nrTxBds;		/* number of transmit bds */
	int txFreeBds;		/* # of free transmit bds */

	/* receive variables */
	volatile DmaChannel *rxDma;	/* location of receive DMA register set */
	volatile DmaDesc *rxBds;	/* Memory location of rx bd ring */
	volatile DmaDesc *rxBdAssignPtr;	/* ptr to next rx bd to become full */
	volatile DmaDesc *rxBdReadPtr;	/* ptr to next rx bd to be processed */
	volatile DmaDesc *rxLastBdPtr;	/* ptr to last allocated rx bd */
	volatile DmaDesc *rxFirstBdPtr;	/* ptr to first allocated rx bd */
	Enet_Tx_CB *rxCbs;

	int nrRxBds;		/* number of receive bds */
	int rxBufLen;		/* size of rx buffers for DMA */

	int rxIrq;		/* rx dma irq */
	PM_Addr pmAddr[MAX_PMADDR];	/* perfect match address */
	bool bIPHdrOptimize;
	int phy_type;
	unsigned long mem_start;
	unsigned long mem_size;
} BcmEnet_devctrl;

#define EMAC_SWAP16(x) (x)
#define EMAC_SWAP32(x) (x)

#endif /* _BCMEMAC_H */
