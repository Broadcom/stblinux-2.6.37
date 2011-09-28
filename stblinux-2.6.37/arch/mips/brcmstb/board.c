/*
 * Copyright (C) 2009 Broadcom Corporation
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

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/bmoca.h>
#include <linux/mtd/partitions.h>

#include <asm/addrspace.h>
#include <asm/page.h>
#include <asm/r4kcache.h>
#include <asm/brcmstb/brcmstb.h>

/* board features */
int brcm_docsis_platform;
int brcm_enet_no_mdio;
int brcm_enet0_force_ext_mii;
char brcm_cfe_boardname[CFE_STRING_SIZE];

/* MTD partition layout */
unsigned long brcm_mtd_rootfs_start;
unsigned long brcm_mtd_rootfs_len;
unsigned long brcm_mtd_kernel_start;
unsigned long brcm_mtd_kernel_len;
unsigned long brcm_mtd_ocap_start;
unsigned long brcm_mtd_ocap_len;
unsigned long brcm_mtd_flash_size_mb;
char brcm_mtd_flash_type[CFE_STRING_SIZE];

/* MoCA 3450 I2C port */
unsigned long brcm_moca_i2c_base;

/* Default MoCA RF band (can be overridden by CFE_BOARDNAME in prom.c) */
#ifdef CONFIG_BRCM_HAS_MOCA_MIDRF
unsigned long brcm_moca_rf_band = MOCA_BAND_MIDRF;
#else
unsigned long brcm_moca_rf_band = MOCA_BAND_HIGHRF;
#endif

/*
 * Default mode for external PHY: MII, RGMII, or RGMII + inband signaling
 * Some boards override this - see below.
 */
unsigned long brcm_ext_mii_mode = BRCM_PHY_TYPE_EXT_RGMII;

#ifdef CONFIG_BRCM_HAS_PCI23

/***********************************************************************
 * PCI 2.3 slot->interrupt mappings
 ***********************************************************************/

#define PCI_A0		BRCM_IRQ_PCI_A0
#define PCI_A1		BRCM_IRQ_PCI_A1
#define PCI_A2		BRCM_IRQ_PCI_A2

#define NUM_SLOTS	16

/* Note for supporting customized boards: slot_number = idsel_line - 16 */

char irq_tab_brcmstb[NUM_SLOTS][4] __devinitdata = {
	[4]  =	{  PCI_A2, PCI_A0,	0,	0	}, /* mPCI */
	[13] =	{  PCI_A0, PCI_A1,	0,	0	}, /* EXT PCI */
	[14] =	{  PCI_A1,	0,	0,	0	}, /* 1394 */
};

char irq_tab_brcmstb_docsis[NUM_SLOTS][4] __devinitdata = {
	[4]  =	{  PCI_A2, PCI_A0,	0,	0	}, /* mPCI */
	[7]  =	{  PCI_A1,	0,	0,	0	}, /* BCM325x */
	[13] =	{  PCI_A0, PCI_A1,	0,	0	}, /* EXT PCI */
	[14] =	{  PCI_A2,	0,	0,	0	}, /* 1394 */
};

#endif

/***********************************************************************
 * PIN_MUX setup
 *
 * NOTE: This code assumes that the bootloader set up the pinmux for
 * the primary UART (almost always UARTA) and PCI/EBI.
 ***********************************************************************/

#define PINMUX(reg, field, val) do { \
	BDEV_WR(BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_##reg, \
		(BDEV_RD(BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_##reg) & \
		 ~BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_##reg##_##field##_MASK) | \
		((val) << \
		 BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_##reg##_##field##_SHIFT)); \
	} while (0)

#define AON_PINMUX(reg, field, val) do { \
	BDEV_WR(BCHP_AON_PIN_CTRL_PIN_MUX_CTRL_##reg, \
		(BDEV_RD(BCHP_AON_PIN_CTRL_PIN_MUX_CTRL_##reg) & \
		 ~BCHP_AON_PIN_CTRL_PIN_MUX_CTRL_##reg##_##field##_MASK) | \
		((val) << \
		 BCHP_AON_PIN_CTRL_PIN_MUX_CTRL_##reg##_##field##_SHIFT)); \
	} while (0)

#define PADCTRL(reg, field, val) do { \
	BDEV_WR(BCHP_SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_##reg, \
		(BDEV_RD(BCHP_SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_##reg) & \
		 ~BCHP_SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_##reg##_##field##_MASK) | \
		((val) << \
		 BCHP_SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_##reg##_##field##_SHIFT)); \
	} while (0)

#define AON_PADCTRL(reg, field, val) do { \
	BDEV_WR(BCHP_AON_PIN_CTRL_PIN_MUX_PAD_CTRL_##reg, \
		(BDEV_RD(BCHP_AON_PIN_CTRL_PIN_MUX_PAD_CTRL_##reg) & \
		 ~BCHP_AON_PIN_CTRL_PIN_MUX_PAD_CTRL_##reg##_##field##_MASK) | \
		((val) << \
		 BCHP_AON_PIN_CTRL_PIN_MUX_PAD_CTRL_##reg##_##field##_SHIFT)); \
	} while (0)

void board_pinmux_setup(void)
{
#if !defined(CONFIG_BRCM_IKOS)
#if   defined(CONFIG_BCM35230)

#if defined(CONFIG_BCMGENET_0_GPHY)
	PINMUX(6, i2ssosck_outd, 2);
	PINMUX(6, i2ssd_outd, 2);
	PINMUX(6, i2sws_outd, 2);
	PINMUX(6, i2ssck_outd, 2);
	PINMUX(6, i2ssosck_outc, 2);
	PINMUX(6, i2ssd_outc, 2);
	PINMUX(6, i2sws_outc, 2);
	PINMUX(6, i2ssck_outc, 2);
	PINMUX(7, i2ssd_in, 5);
	PINMUX(7, i2sws_in, 5);
	PINMUX(7, i2ssck_in, 5);
	PINMUX(8, gpio_4, 4);
	PINMUX(9, gpio_74, 3);
	PINMUX(10, gpio_79, 3);
	PINMUX(10, gpio_78, 3);
	PINMUX(10, gpio_77, 3);
	PINMUX(10, gpio_76, 3);
	PINMUX(10, gpio_75, 3);
	PINMUX(11, dint, 2);

	brcm_ext_mii_mode = BRCM_PHY_TYPE_EXT_MII;
#endif

#elif defined(CONFIG_BCM7125)

	PINMUX(8, uart_1_rxd, 0);	/* UARTB RX */
	PINMUX(9, uart_1_txd, 0);	/* UARTB TX */
	PINMUX(9, gpio_16, 1);		/* UARTC RX */
	PINMUX(9, gpio_17, 1);		/* UARTC TX */

	PINMUX(10, sgpio_02, 1);	/* MoCA I2C on BSCB */
	PINMUX(10, sgpio_03, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCB_REG_START);

	brcm_ext_mii_mode = BRCM_PHY_TYPE_EXT_MII;

#elif defined(CONFIG_BCM7231)

	PINMUX(11, gpio_94, 1);		/* UARTB TX */
	PINMUX(11, gpio_95, 1);		/* UARTB RX */

	if (BRCM_PROD_ID() == 0x7230) {
		/* 7230 is not the same ballout as 7231 */
		AON_PINMUX(0, aon_gpio_04, 6);	/* SDIO */
		AON_PINMUX(0, aon_gpio_05, 6);
		AON_PINMUX(1, aon_gpio_12, 5);
		AON_PINMUX(1, aon_gpio_13, 5);
		AON_PINMUX(2, aon_gpio_14, 5);
		AON_PINMUX(2, aon_gpio_15, 6);
		AON_PINMUX(2, aon_gpio_16, 6);
		AON_PINMUX(2, aon_gpio_17, 6);
		AON_PINMUX(2, aon_gpio_18, 6);
		AON_PINMUX(2, aon_gpio_19, 6);

		/* disable GPIO pulldowns */
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_0,
			aon_gpio_04_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_0,
			aon_gpio_05_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_12_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_13_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_14_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_15_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_16_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_17_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_18_pad_ctrl, 0);
		BDEV_WR_F_RB(AON_PIN_CTRL_PIN_MUX_PAD_CTRL_1,
			aon_gpio_19_pad_ctrl, 0);

		/* limit speed to 25MHz due to AON pad timing restrictions */
		BDEV_UNSET(BCHP_SDIO_0_CFG_CAP_REG0, 1 << 19);	/* Highspd=0 */
		BDEV_SET(BCHP_SDIO_0_CFG_CAP_REG1, 1 << 31);	/* Override=1 */
	} else {
		/* set RGMII lines to 2.5V */
		BDEV_WR_F(SUN_TOP_CTRL_GENERAL_CTRL_NO_SCAN_0,
			rgmii_0_pad_mode, 1);

		PINMUX(15, gpio_132, 1);	/* RGMII RX */
		PINMUX(15, gpio_133, 1);
		PINMUX(15, gpio_134, 1);
		PINMUX(16, gpio_139, 1);
		PINMUX(16, gpio_140, 1);
		PINMUX(16, gpio_141, 1);
		PINMUX(16, gpio_142, 1);

		PINMUX(16, gpio_138, 1);	/* RGMII TX */
		PINMUX(17, gpio_143, 1);
		PINMUX(17, gpio_144, 1);
		PINMUX(17, gpio_145, 1);
		PINMUX(17, gpio_146, 1);
		PINMUX(17, gpio_147, 1);

		PINMUX(17, gpio_149, 1);	/* RGMII MDIO */
		PINMUX(16, gpio_137, 1);

		/* no pulldown on RGMII lines */
		PADCTRL(8, gpio_132_pad_ctrl, 0);
		PADCTRL(8, gpio_133_pad_ctrl, 0);
		PADCTRL(8, gpio_134_pad_ctrl, 0);
		PADCTRL(8, gpio_137_pad_ctrl, 0);
		PADCTRL(8, gpio_138_pad_ctrl, 0);
		PADCTRL(9, gpio_139_pad_ctrl, 0);
		PADCTRL(9, gpio_140_pad_ctrl, 0);
		PADCTRL(9, gpio_141_pad_ctrl, 0);
		PADCTRL(9, gpio_142_pad_ctrl, 0);
		PADCTRL(9, gpio_143_pad_ctrl, 0);
		PADCTRL(9, gpio_144_pad_ctrl, 0);
		PADCTRL(9, gpio_145_pad_ctrl, 0);
		PADCTRL(9, gpio_146_pad_ctrl, 0);
		PADCTRL(9, gpio_147_pad_ctrl, 0);
		PADCTRL(9, gpio_149_pad_ctrl, 0);

		PINMUX(14, gpio_122, 1);	/* SDIO */
		PINMUX(14, gpio_123, 1);
		PINMUX(14, gpio_124, 1);
		PINMUX(14, gpio_125, 1);
		PINMUX(14, gpio_126, 1);
		PINMUX(15, gpio_127, 1);
		PINMUX(15, gpio_128, 1);
		PINMUX(15, gpio_129, 1);
		PINMUX(15, gpio_130, 1);
		PINMUX(15, gpio_131, 1);

		/* no pulldown on SDIO lines */
		PADCTRL(7, gpio_122_pad_ctrl, 0);
		PADCTRL(7, gpio_123_pad_ctrl, 0);
		PADCTRL(8, gpio_124_pad_ctrl, 0);
		PADCTRL(8, gpio_125_pad_ctrl, 0);
		PADCTRL(8, gpio_126_pad_ctrl, 0);
		PADCTRL(8, gpio_127_pad_ctrl, 0);
		PADCTRL(8, gpio_128_pad_ctrl, 0);
		PADCTRL(8, gpio_129_pad_ctrl, 0);
		PADCTRL(8, gpio_130_pad_ctrl, 0);
		PADCTRL(8, gpio_131_pad_ctrl, 0);
	}

#elif defined(CONFIG_BCM7340)

	PINMUX(18, uart_rxdb, 0);	/* UARTB RX */
	PINMUX(18, uart_txdb, 0);	/* UARTB TX */
	PINMUX(4, gpio_00, 3);		/* UARTC RX */
	PINMUX(4, gpio_01, 3);		/* UARTC TX */

	PINMUX(14, sgpio_00, 1);	/* MoCA I2C on BSCA */
	PINMUX(14, sgpio_01, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCA_REG_START);

#elif defined(CONFIG_BCM7342)

	PINMUX(10, gpio_023, 1);	/* ENET LEDs */
	PINMUX(11, gpio_024, 1);

	PINMUX(23, vo_656_7, 1);	/* MoCA LEDs */
	PINMUX(23, vo_656_clk, 1);

	PINMUX(12, gpio_034, 1);	/* UARTB TX */
	PINMUX(12, gpio_035, 1);	/* UARTB RX */
	PINMUX(12, gpio_038, 1);	/* UARTC TX */
	PINMUX(12, gpio_039, 1);	/* UARTC RX */

	PINMUX(21, sgpio_02, 1);	/* MoCA I2C on BSCB */
	PINMUX(21, sgpio_03, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCB_REG_START);

#elif defined(CONFIG_BCM7344)

	PINMUX(15, gpio_79, 1);		/* MoCA link */
	PINMUX(15, gpio_80, 1);		/* MoCA activity */

	PINMUX(17, uart_rxdb, 0);	/* UARTB RX */

#ifdef CONFIG_BCM7344A0
	PINMUX(18, uart_txdb, 0);	/* UARTB TX */
#else
	PINMUX(17, uart_txdb, 0);	/* UARTB TX */
#endif

	AON_PINMUX(0, aon_gpio_00, 3);	/* UARTC RX (NC) */
	AON_PINMUX(0, aon_gpio_01, 3);	/* UARTC TX (NC) */

	/* NOTE: this is buggy in A0 */
	AON_PINMUX(2, aon_sgpio_00, 1);	/* MoCA I2C */
	AON_PINMUX(2, aon_sgpio_01, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCC_REG_START);

#if defined(CONFIG_BCMGENET_0_GPHY)
	/* select MAC0 for RGMII */
	BDEV_WR_F(SUN_TOP_CTRL_GENERAL_CTRL_0, mii_genet_mac_select, 0);

	PINMUX(9, gpio_31, 1);		/* RGMII RX */
	PINMUX(9, gpio_28, 1);
	PINMUX(9, gpio_27, 1);
	PINMUX(9, gpio_26, 1);
	PINMUX(8, gpio_25, 1);
	PINMUX(8, gpio_24, 1);
	PINMUX(8, gpio_23, 1);

	PINMUX(9, gpio_34, 1);		/* RGMII TX */
	PINMUX(9, gpio_35, 1);
	PINMUX(10, gpio_36, 1);
	PINMUX(10, gpio_40, 1);
	PINMUX(10, gpio_39, 1);
	PINMUX(10, gpio_38, 1);
	PINMUX(10, gpio_37, 1);

	PINMUX(10, gpio_32, 1);		/* ENET MDIO */
	PINMUX(10, gpio_33, 1);
	PINMUX(9, gpio_29, 1);
	PINMUX(9, gpio_30, 1);
#endif

#elif defined(CONFIG_BCM7346)

	PINMUX(15, gpio_068, 2);	/* MoCA link */
	PINMUX(16, gpio_069, 1);	/* MoCA activity */

	PINMUX(9, gpio_017, 1);		/* UARTB TX */
	PINMUX(9, gpio_018, 1);		/* UARTB RX */
	PINMUX(10, gpio_021, 1);	/* UARTC TX */
	PINMUX(10, gpio_022, 1);	/* UARTC RX */

	PINMUX(16, sgpio_02, 1);	/* MoCA I2C on BSCB */
	PINMUX(16, sgpio_03, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCB_REG_START);

#elif defined(CONFIG_BCM7358) || defined(CONFIG_BCM7552)

	PINMUX(11, gpio_89, 1);		/* UARTB TX */
	PINMUX(11, gpio_90, 1);		/* UARTB RX */
	PINMUX(11, gpio_91, 1);		/* UARTC TX */
	PINMUX(11, gpio_92, 1);		/* UARTC RX */

	AON_PINMUX(1, aon_gpio_08, 6);	/* SDIO */
	AON_PINMUX(1, aon_gpio_12, 5);
	AON_PINMUX(1, aon_gpio_13, 5);
	AON_PINMUX(2, aon_gpio_14, 4);
	AON_PINMUX(2, aon_gpio_15, 5);
	AON_PINMUX(2, aon_gpio_16, 5);
	AON_PINMUX(2, aon_gpio_17, 5);
	AON_PINMUX(2, aon_gpio_18, 5);
	AON_PINMUX(2, aon_gpio_19, 5);
	AON_PINMUX(2, aon_gpio_20, 5);

	/* disable pulldowns */
	AON_PADCTRL(0, aon_gpio_08_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_12_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_13_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_14_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_15_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_16_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_17_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_18_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_19_pad_ctrl, 0);
	AON_PADCTRL(1, aon_gpio_20_pad_ctrl, 0);

#if defined(CONFIG_BCMGENET_0_GPHY)
	/* set RGMII lines to 2.5V */
	BDEV_WR_F(SUN_TOP_CTRL_GENERAL_CTRL_NO_SCAN_0, rgmii_0_pad_mode, 1);

	PINMUX(14, gpio_111, 1);	/* RGMII RX */
	PINMUX(14, gpio_112, 1);
	PINMUX(14, gpio_114, 1);
	PINMUX(14, gpio_115, 1);
	PINMUX(14, gpio_116, 1);
	PINMUX(14, gpio_117, 1);

	PINMUX(14, gpio_113, 1);	/* RGMII TX */
	PINMUX(14, gpio_118, 1);
	PINMUX(15, gpio_119, 1);
	PINMUX(15, gpio_120, 1);
	PINMUX(15, gpio_121, 1);
	PINMUX(15, gpio_122, 1);

	PINMUX(15, gpio_123, 1);	/* RGMII MDIO */
	PINMUX(15, gpio_124, 1);

	/* disable GPIO pulldowns */
	PADCTRL(7, gpio_111_pad_ctrl, 0);
	PADCTRL(7, gpio_112_pad_ctrl, 0);
	PADCTRL(7, gpio_113_pad_ctrl, 0);
	PADCTRL(7, gpio_114_pad_ctrl, 0);
	PADCTRL(7, gpio_115_pad_ctrl, 0);
	PADCTRL(7, gpio_116_pad_ctrl, 0);
	PADCTRL(8, gpio_117_pad_ctrl, 0);
	PADCTRL(8, gpio_118_pad_ctrl, 0);
	PADCTRL(8, gpio_119_pad_ctrl, 0);
	PADCTRL(8, gpio_120_pad_ctrl, 0);
	PADCTRL(8, gpio_121_pad_ctrl, 0);
	PADCTRL(8, gpio_122_pad_ctrl, 0);
	PADCTRL(8, gpio_123_pad_ctrl, 0);
	PADCTRL(8, gpio_124_pad_ctrl, 0);

#endif /* defined(CONFIG_BCMGENET_0_GPHY) */

#elif defined(CONFIG_BCM7405B0)

#if !defined(CONFIG_KGDB)
	/*
	 * Default: use MII, no UARTB/UARTC.
	 * BCM97405 SW2801-7 should be OFF
	 */
	PINMUX(2, gpio_002, 1);		/* MII */
	PINMUX(2, gpio_003, 1);
	PINMUX(3, gpio_004, 1);
	PINMUX(3, gpio_005, 1);
	PINMUX(3, gpio_006, 1);
	PINMUX(3, gpio_007, 1);
	PINMUX(3, gpio_008, 1);
	PINMUX(3, gpio_009, 1);
	PINMUX(3, gpio_010, 1);
	PINMUX(3, gpio_011, 1);
	PINMUX(3, gpio_012, 1);
	PINMUX(3, gpio_013, 1);
	PINMUX(4, gpio_014, 1);
	PINMUX(4, gpio_015, 1);
	PINMUX(4, gpio_016, 1);
	PINMUX(4, gpio_017, 1);
	PINMUX(4, gpio_018, 1);
	PINMUX(4, gpio_019, 1);
#else
	/*
	 * Alternate: use UARTB, UARTC.  Required for kgdb.
	 * BCM97405 SW2801-7 should be ON
	 */
	PINMUX(3, gpio_008, 2);		/* UARTB TX */
	PINMUX(3, gpio_007, 2);		/* UARTB RX */
	PINMUX(3, gpio_012, 2);		/* UARTC TX */
	PINMUX(3, gpio_011, 2);		/* UARTC RX */

	printk(KERN_WARNING "%s: disabling MII to enable extra UARTs\n",
		__func__);
#endif

#elif defined(CONFIG_BCM7408)

	PINMUX(2, gpio_01, 1);		/* MoCA LEDs */
	PINMUX(2, gpio_02, 1);

	PINMUX(3, gpio_06, 1);		/* UARTB RX */
	PINMUX(3, gpio_05, 1);		/* UARTB TX */
	PINMUX(3, gpio_12, 1);		/* UARTC RX */
	PINMUX(3, gpio_11, 1);		/* UARTC TX */

	PINMUX(7, sgpio_02, 1);		/* MoCA I2C on BSCB */
	PINMUX(7, sgpio_03, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCB_REG_START);

#elif defined(CONFIG_BCM7420)

	PINMUX(7, gpio_000, 1);		/* ENET LEDs */
	PINMUX(7, gpio_001, 1);

	PINMUX(9, gpio_017, 1);		/* MoCA LEDs */
	PINMUX(9, gpio_019, 1);

	PINMUX(17, gpio_081, 4);	/* UARTB RX */
	PINMUX(17, gpio_082, 4);	/* UARTB TX */
	PINMUX(9, gpio_022, 3);		/* UARTC RX */
	PINMUX(9, gpio_023, 3);		/* UARTC TX */

	PINMUX(21, sgpio_02, 1);	/* MoCA I2C on BSCB */
	PINMUX(21, sgpio_03, 1);
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCB_REG_START);

#if defined(CONFIG_BCMGENET_0_GPHY)
	/* set RGMII lines to 2.5V */
	BDEV_WR_F(SUN_TOP_CTRL_GENERAL_CTRL_NO_SCAN_1, rgmii_pad_mode, 1);

	PINMUX(7, gpio_002, 1);		/* RGMII RX */
	PINMUX(7, gpio_003, 1);
	PINMUX(7, gpio_004, 1);
	PINMUX(7, gpio_005, 1);
	PINMUX(7, gpio_006, 1);
	PINMUX(7, gpio_007, 1);

	PINMUX(8, gpio_009, 1);		/* RGMII TX */
	PINMUX(8, gpio_010, 1);
	PINMUX(8, gpio_011, 1);
	PINMUX(8, gpio_012, 1);
	PINMUX(8, gpio_013, 1);
	PINMUX(8, gpio_014, 1);

	PINMUX(20, gpio_108, 3);	/* ENET MDIO */
	PINMUX(20, gpio_109, 4);
#endif

#elif defined(CONFIG_BCM7425)

	/* Bootloader indicates the availability of SDIO_0 in SCRATCH reg */
	if ((BDEV_RD(BCHP_SDIO_0_CFG_SCRATCH) & 0x01) == 0) {
		PINMUX(14, gpio_072, 2);
		PINMUX(14, gpio_073, 2);
		PINMUX(14, gpio_074, 2);
		PINMUX(14, gpio_075, 2);
		PINMUX(15, gpio_076, 2);
		PINMUX(15, gpio_077, 2);
		PINMUX(15, gpio_078, 2);
		PINMUX(15, gpio_079, 2);
		PINMUX(15, gpio_080, 2);
		PINMUX(15, gpio_081, 2);

		/* enable internal pullups */
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_9,
			gpio_072_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_073_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_074_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_075_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_076_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_077_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_078_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_079_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_080_pad_ctrl, 2);
		BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_10,
			gpio_081_pad_ctrl, 2);

		/* always use 3.3V (SDIO0_LOW_V_SEL_N=1) */
		BDEV_UNSET(BCHP_GIO_AON_IODIR_LO, 1 << 4);
		BDEV_SET(BCHP_GIO_AON_DATA_LO, 1 << 4);
	}

	PINMUX(18, sgpio_00, 1);	/* MoCA I2C */
	PINMUX(19, sgpio_01, 1);
#if defined(CONFIG_BCM7425B0)
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCC_REG_START);
#else
	brcm_moca_i2c_base = BPHYSADDR(BCHP_BSCA_REG_START);
#endif

#elif defined(CONFIG_BCM7468)

	/* NOTE: R1022 and R1023 must be installed to use UARTB */
	PINMUX(4, gpio_15, 3);		/* UARTB TX */
	PINMUX(4, gpio_14, 3);		/* UARTB RX */

	PINMUX(3, gpio_01, 1);		/* SDIO */
	PINMUX(3, gpio_02, 1);
	PINMUX(3, gpio_03, 1);
	PINMUX(3, gpio_04, 1);
	PINMUX(3, gpio_05, 1);
	PINMUX(3, gpio_06, 1);
	PINMUX(3, gpio_07, 1);
	PINMUX(3, gpio_08, 1);
	PINMUX(4, gpio_09, 1);
	PINMUX(4, gpio_10, 1);

	/* disable GPIO pulldowns, in order to get 3.3v on SDIO pins */
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_01_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_02_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_03_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_04_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_05_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_06_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_07_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_08_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_09_pad_ctrl, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_PIN_MUX_PAD_CTRL_2, gpio_10_pad_ctrl, 0);

	brcm_ext_mii_mode = BRCM_PHY_TYPE_EXT_MII;

#elif defined(CONFIG_BCM7550)

	PINMUX(13, gpio_84, 1);		/* UARTB TX */
	PINMUX(13, gpio_85, 1);		/* UARTB RX */
	PINMUX(13, gpio_86, 1);		/* UARTC TX */
	PINMUX(13, gpio_87, 1);		/* UARTC RX */

#elif defined(CONFIG_BCM7640A0)
	PINMUX(8, gpio_03, 1);		/* UARTB TX */
	PINMUX(7, gpio_02, 1);		/* UARTB RX */

	PINMUX(12, gpio_36, 1);		/* SDIO */
	PINMUX(12, gpio_37, 1);
	PINMUX(12, gpio_38, 1);
	PINMUX(12, gpio_39, 1);
	PINMUX(12, gpio_40, 1);
	PINMUX(12, gpio_41, 1);
	PINMUX(12, gpio_42, 1);
	PINMUX(13, gpio_43, 1);
	PINMUX(13, gpio_44, 1);

#elif defined(CONFIG_BCM7640B0)
	PINMUX(6, gpio_03, 1);		/* UARTB TX */
	PINMUX(6, gpio_02, 1);		/* UARTB RX */

	PINMUX(10, gpio_36, 1);		/* SDIO */
	PINMUX(10, gpio_37, 1);
	PINMUX(11, gpio_38, 1);
	PINMUX(11, gpio_39, 1);
	PINMUX(11, gpio_40, 1);
	PINMUX(11, gpio_41, 1);
	PINMUX(11, gpio_42, 1);
	PINMUX(11, gpio_43, 1);
	PINMUX(11, gpio_44, 1);

#endif /* chip type */
#endif /* !defined(CONFIG_BRCM_IKOS) */
}

/***********************************************************************
 * RAM and FLASH configuration
 ***********************************************************************/

#define MAGIC0		0xdeadbeef
#define MAGIC1		0xfeedcafe

static inline unsigned int __init probe_ram_size(void)
{
	unsigned long addr = KSEG1, taddr;
	uint32_t olddata;
	unsigned long flags;
	unsigned int i, memsize = 256;

	printk(KERN_INFO "Probing system memory size... ");

	local_irq_save(flags);
	cache_op(Hit_Writeback_Inv_D, KSEG0);
	olddata = DEV_RD(addr);

	/*
	 * Try to figure out where memory wraps around.  If it does not
	 * wrap, assume 256MB
	*/
	for (i = 4; i <= 128; i <<= 1) {
		taddr = KSEG1 + i * 1048576;
		DEV_WR(addr, MAGIC0);
		if (DEV_RD(taddr) == MAGIC0) {
			DEV_WR(addr, MAGIC1);
			if (DEV_RD(taddr) == MAGIC1) {
				memsize = i;
				break;
			}
		}
	}

	DEV_WR(addr, olddata);
	cache_op(Hit_Writeback_Inv_D, KSEG0);
	local_irq_restore(flags);

	printk(KERN_CONT "found %u MB\n", memsize);

	return memsize;
}

void __init board_get_ram_size(unsigned long *dram0_mb, unsigned long *dram1_mb)
{
#if defined(CONFIG_BRCM_OVERRIDE_RAM_SIZE)
	*dram0_mb = CONFIG_BRCM_FORCED_DRAM0_SIZE;
#if defined(CONFIG_BRCM_FORCED_DRAM1_SIZE)
	*dram1_mb = CONFIG_BRCM_FORCED_DRAM1_SIZE;
#endif
	printk(KERN_INFO "Using %lu MB + %lu MB RAM "
		"(from kernel configuration)\n", *dram0_mb, *dram1_mb);
#else
	/* DRAM0_SIZE variable from CFE */
	if (*dram0_mb) {
		printk(KERN_INFO "Using %lu MB + %lu MB RAM (from CFE)\n",
			*dram0_mb, *dram1_mb);
		return;
	}

	*dram0_mb = bchip_strap_ram_size();
	if (*dram0_mb)
		printk(KERN_INFO "Using %lu MB RAM (from straps)\n", *dram0_mb);
	else
		*dram0_mb = probe_ram_size();
#endif
}

#if defined(CONFIG_BRCM_FIXED_MTD_PARTITIONS)

static struct mtd_partition fixed_partition_map[] = {
	{
		.name = "entire_device",
		.size = MTDPART_SIZ_FULL,
		.offset = 0x00000000
	},
};

/*
 * Use the partition map defined at compile time
 */
int __init board_get_partition_map(struct mtd_partition **p)
{
	*p = fixed_partition_map;
	return ARRAY_SIZE(fixed_partition_map);
}

#else /* defined(CONFIG_BRCM_FIXED_MTD_PARTITIONS) */

/*
 * Construct the partition map for the primary flash device, based on
 * CFE environment variables that were read from prom.c
 *
 * Note that this does NOT have all of the partitions that CFE recognizes
 * (e.g. macadr, nvram).  It only has the rootfs, entire_device, and
 * optionally the kernel image partition.
 */
int __init board_get_partition_map(struct mtd_partition **p)
{
	struct mtd_partition *ret;
	int nr_parts;

	if (brcm_mtd_rootfs_len == 0)
		return -ENODEV;

	nr_parts = 2;
	if (brcm_mtd_kernel_len != 0)
		nr_parts++;

	ret = kzalloc(nr_parts * sizeof(struct mtd_partition), GFP_KERNEL);
	if (!ret)
		return -ENOMEM;

	ret[0].offset = brcm_mtd_rootfs_start;
	ret[0].size = brcm_mtd_rootfs_len;
	ret[0].name = "rootfs";

	ret[1].offset = 0;
	ret[1].size = MTDPART_SIZ_FULL;
	ret[1].name = "entire_device";

	if (brcm_mtd_kernel_len != 0) {
		ret[2].offset = brcm_mtd_kernel_start;
		ret[2].size = brcm_mtd_kernel_len;
		ret[2].name = "kernel";
	}

	*p = ret;
	return nr_parts;
}
#endif /* defined(CONFIG_BRCM_FIXED_MTD_PARTITIONS) */

void brcm_get_ocap_info(struct brcm_ocap_info *info)
{
	info->ocap_part_start = brcm_mtd_ocap_start;
	info->ocap_part_len = brcm_mtd_ocap_len;
	info->docsis_platform = brcm_docsis_platform;
}
EXPORT_SYMBOL(brcm_get_ocap_info);
