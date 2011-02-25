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

#include <linux/module.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/serial_8250.h>
#include <linux/platform_device.h>
#include <linux/bootmem.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/compiler.h>
#include <linux/bmoca.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/spi/spi.h>
#include <linux/spi/flash.h>

#include <asm/serial.h>
#include <asm/reboot.h>
#include <asm/addrspace.h>
#include <asm/irq.h>
#include <asm/cpu-features.h>
#include <asm/war.h>
#include <asm/brcmstb/brcmstb.h>

#ifndef CONFIG_MTD
/* squash MTD warning on IKOS builds */
#define CONFIG_MTD_MAP_BANK_WIDTH_1 1
#endif

#include <linux/mtd/mtd.h>
#include <linux/mtd/physmap.h>
#include <linux/mtd/map.h>

/* Extra SPI flash chip selects to scan at boot time (configurable) */
#define EXTRA_SPI_CS		0x00

/***********************************************************************
 * Platform device setup
 ***********************************************************************/

#ifdef CONFIG_BRCM_HAS_16550
#define BRCM_16550_PLAT_DEVICE(uart_addr, uart_irq) \
	{ \
		.mapbase = BPHYSADDR(uart_addr), \
		.irq = (uart_irq), \
		.uartclk = BASE_BAUD * 16, \
		.regshift = 2, \
		.iotype = UPIO_MEM32, \
		.flags = UPF_BOOT_AUTOCONF | UPF_IOREMAP, \
	},

#ifdef CONFIG_BRCM_HAS_PCU_UARTS
#define BCHP_UARTA_REG_START	BCHP_TVM_UART1_RBR
#define BCHP_UARTB_REG_START	BCHP_PCU_UART2_RBR
#endif

static struct plat_serial8250_port brcm_16550_ports[] = {
#ifdef CONFIG_BRCM_UARTA_IS_16550
BRCM_16550_PLAT_DEVICE(BCHP_UARTA_REG_START, BRCM_IRQ_UARTA)
#endif
#ifdef CONFIG_BRCM_UARTB_IS_16550
BRCM_16550_PLAT_DEVICE(BCHP_UARTB_REG_START, BRCM_IRQ_UARTB)
#endif
#ifdef CONFIG_BRCM_UARTC_IS_16550
BRCM_16550_PLAT_DEVICE(BCHP_UARTC_REG_START, BRCM_IRQ_UARTC)
#endif
	{
		.flags = 0,
	}
};

static struct platform_device brcm_16550_uarts = {
	.name = "serial8250",
	.dev = {
		.platform_data = &brcm_16550_ports,
	},
};
#endif

#ifdef CONFIG_BRCM_HAS_3250

#define BRCM_3250_PLAT_DEVICE(uart, i) \
	static struct resource bcm3250_##uart##_resources[] = { \
		[0] = { \
			.start = BPHYSADDR(BCHP_##uart##_REG_START), \
			.end = BPHYSADDR(BCHP_##uart##_REG_END) + 3, \
			.flags = IORESOURCE_MEM, \
		}, \
		[1] = { \
			.start = BRCM_IRQ_##uart, \
			.end = BRCM_IRQ_##uart, \
			.flags = IORESOURCE_IRQ, \
		}, \
	}; \
	static struct platform_device bcm3250_##uart##_device = { \
		.name = "bcm3250_serial", \
		.num_resources = ARRAY_SIZE(bcm3250_##uart##_resources), \
		.resource = bcm3250_##uart##_resources, \
		.id = i, \
	};

#ifdef CONFIG_BRCM_UARTA_IS_3250
BRCM_3250_PLAT_DEVICE(UARTA, 0)
#endif
#ifdef CONFIG_BRCM_UARTB_IS_3250
BRCM_3250_PLAT_DEVICE(UARTB, 1)
#endif
#ifdef CONFIG_BRCM_UARTC_IS_3250
BRCM_3250_PLAT_DEVICE(UARTC, 2)
#endif

#endif

static inline void brcm_bogus_release(struct device *dev)
{
}

#if defined(CONFIG_BRCM_HAS_EMAC_0)

#ifdef BCHP_ENET_TOP_REG_START
#define BCHP_EMAC_0_REG_START	BCHP_ENET_TOP_REG_START
#define BCHP_EMAC_0_REG_END	BCHP_ENET_TOP_REG_END
#endif

static struct resource bcmemac_0_resources[] = {
	[0] = {
		.start		= BPHYSADDR(BCHP_EMAC_0_REG_START),
		.end		= BPHYSADDR(BCHP_EMAC_0_REG_END) + 3,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= BRCM_IRQ_EMAC_0,
		.end		= BRCM_IRQ_EMAC_0,
		.flags		= IORESOURCE_IRQ,
	},
};

struct bcmemac_platform_data bcmemac_0_plat_data = {
#if !defined(CONFIG_BCMEMAC_EXTMII)
	.phy_type		= BRCM_PHY_TYPE_INT,
#else
	.phy_type		= BRCM_PHY_TYPE_EXT_MII,
#endif
	.phy_id			= BRCM_PHY_ID_AUTO,
};

static struct platform_device bcmemac_0_plat_dev = {
	.name			= "bcmemac",
	.id			= 0,
	.num_resources		= ARRAY_SIZE(bcmemac_0_resources),
	.resource		= bcmemac_0_resources,
	.dev = {
		.platform_data	= &bcmemac_0_plat_data,
		.release	= &brcm_bogus_release,
	},
};

#endif /* defined(CONFIG_BRCM_HAS_EMAC_0) */

#if defined(CONFIG_BRCM_HAS_EMAC_1)

static struct resource bcmemac_1_resources[] = {
	[0] = {
		.start		= BPHYSADDR(BCHP_EMAC_1_REG_START),
		.end		= BPHYSADDR(BCHP_EMAC_1_REG_END) + 3,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= BRCM_IRQ_EMAC_1,
		.end		= BRCM_IRQ_EMAC_1,
		.flags		= IORESOURCE_IRQ,
	},
};

struct bcmemac_platform_data bcmemac_1_plat_data = {
	.phy_type		= BRCM_PHY_TYPE_EXT_MII,
	.phy_id			= BRCM_PHY_ID_AUTO,
};

static struct platform_device bcmemac_1_plat_dev = {
	.name			= "bcmemac",
	.id			= 1,
	.num_resources		= ARRAY_SIZE(bcmemac_1_resources),
	.resource		= bcmemac_1_resources,
	.dev = {
		.platform_data	= &bcmemac_1_plat_data,
		.release	= &brcm_bogus_release,
	},
};
#endif /* defined(CONFIG_BRCM_HAS_EMAC_1) */

#if defined(CONFIG_BRCM_HAS_SDIO_V0)

static struct resource sdio_resources[] = {
	[0] = {
		.start		= BPHYSADDR(BCHP_SDIO_REG_START),
		.end		= BPHYSADDR(BCHP_SDIO_REG_END) + 3,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= BRCM_IRQ_HIF,
		.end		= BRCM_IRQ_HIF,
		.flags		= IORESOURCE_IRQ,
	},
};

static struct platform_device sdio_plat_dev = {
	.name			= "sdhci-brcm",
	.id			= 0,
	.num_resources		= ARRAY_SIZE(sdio_resources),
	.resource		= sdio_resources,
};

#endif /* defined(CONFIG_BRCM_HAS_SDIO_V0) */

#if defined(CONFIG_BRCM_HAS_SDIO_V1)

static void __init brcm_add_sdio_host(int id, uintptr_t cfg_base,
	uintptr_t host_base, int irq)
{
	struct resource res[2];
	struct platform_device *pdev;

	/*
	 * CFE will disable EMMC (via CFG SCRATCH bit 0) if something else is
	 * connected to the shared EMMC/EBI pins
	 */
	if (bchip_sdio_init(id, cfg_base) < 0)
		return;

	memset(&res, 0, sizeof(res));
	res[0].start = BPHYSADDR(host_base);
	res[0].end = BPHYSADDR(host_base + 0xff);
	res[0].flags = IORESOURCE_MEM;

	res[1].start = res[1].end = irq;
	res[1].flags = IORESOURCE_IRQ;

	pdev = platform_device_alloc("sdhci", id);
	platform_device_add_resources(pdev, res, 2);
	platform_device_add(pdev);
}

#endif /* defined(CONFIG_BRCM_HAS_SDIO_V1) */

static struct platform_device __init *brcm_new_usb_host(char *name, int id,
	uintptr_t base, int irq)
{
	struct resource res[2];
	struct platform_device *pdev;
	static const u64 usb_dmamask = ~(u32)0;

	memset(&res, 0, sizeof(res));
	res[0].start = BPHYSADDR(base);
	res[0].end = BPHYSADDR(base + 0xff);
	res[0].flags = IORESOURCE_MEM;

	res[1].start = res[1].end = irq;
	res[1].flags = IORESOURCE_IRQ;

	pdev = platform_device_alloc(name, id);
	platform_device_add_resources(pdev, res, 2);

	pdev->dev.dma_mask = (u64 *)&usb_dmamask;
	pdev->dev.coherent_dma_mask = 0xffffffff;

	return pdev;
}

#if defined(CONFIG_BRCM_HAS_GENET)

/* legacy names */
#if !defined(BCHP_GENET_0_SYS_REG_START)
#define BCHP_GENET_0_SYS_REG_START	BCHP_GENET_SYS_REG_START
#endif
#if !defined(BCHP_GENET_1_SYS_REG_START)
#define BCHP_GENET_1_SYS_REG_START	BCHP_MOCA_GENET_SYS_REG_START
#endif

static void __init brcm_register_genet(int id, uintptr_t base,
	int irq0, int irq1, int phy_type)
{
	struct resource res[3];
	struct platform_device *pdev;
	struct bcmemac_platform_data pdata;

	memset(&res, 0, sizeof(res));
	res[0].start = BPHYSADDR(base);
	res[0].end = BPHYSADDR(base + 0x4fff);
	res[0].flags = IORESOURCE_MEM;

	res[1].start = res[1].end = irq0;
	res[1].flags = IORESOURCE_IRQ;

	res[2].start = res[2].end = irq1;
	res[2].flags = IORESOURCE_IRQ;

	pdata.phy_type = phy_type;

	switch (phy_type) {
	case BRCM_PHY_TYPE_INT:
		pdata.phy_id = 1;
		break;
	case BRCM_PHY_TYPE_MOCA:
		pdata.phy_id = BRCM_PHY_ID_NONE;
		break;
	default:
		pdata.phy_id = BRCM_PHY_ID_AUTO;
	}
	brcm_alloc_macaddr(pdata.macaddr);

	pdev = platform_device_alloc("bcmgenet", id);
	platform_device_add_resources(pdev, res, 3);
	platform_device_add_data(pdev, &pdata, sizeof(pdata));
	platform_device_add(pdev);
}
#endif /* defined(CONFIG_BRCM_HAS_GENET) */

#if defined(CONFIG_BRCM_HAS_MOCA)
static void __init brcm_register_moca(int enet_id)
{
	struct resource res[2];
	struct platform_device *pdev;
	struct moca_platform_data pdata;
	u8 macaddr[ETH_ALEN];

	bchip_moca_init();

	memset(&res, 0, sizeof(res));
	res[0].start = BPHYSADDR(BCHP_DATA_MEM_REG_START);
	res[0].end = BPHYSADDR(BCHP_MOCA_HOSTMISC_MMP_REG_END) + 3;
	res[0].flags = IORESOURCE_MEM;

	res[1].start = BRCM_IRQ_MOCA;
	res[1].flags = IORESOURCE_IRQ;

	brcm_alloc_macaddr(macaddr);
	mac_to_u32(&pdata.macaddr_hi, &pdata.macaddr_lo, macaddr);

	strcpy(pdata.enet_name, "bcmgenet");
	pdata.enet_id = enet_id;
	pdata.bcm3450_i2c_addr = 0x70;
	pdata.bcm3450_i2c_base = brcm_moca_i2c_base;
	pdata.hw_rev = (BRCM_CHIP_ID() << 16) | (BRCM_CHIP_REV() + 0xa0);
	pdata.rf_band = brcm_moca_rf_band;

	if (brcm_moca_i2c_base == 0) {
		printk(KERN_WARNING
			"error: bmoca I2C base addr is not set\n");
		return;
	}

	pdev = platform_device_alloc("bmoca", 0);
	platform_device_add_resources(pdev, res, 2);
	platform_device_add_data(pdev, &pdata, sizeof(pdata));
	platform_device_add(pdev);
}
#endif /* defined(CONFIG_BRCM_HAS_MOCA) */

static int __init platform_devices_setup(void)
{
	/* UARTs */

#ifdef CONFIG_BRCM_HAS_16550
	platform_device_register(&brcm_16550_uarts);
#endif
#ifdef CONFIG_BRCM_UARTA_IS_3250
	platform_device_register(&bcm3250_UARTA_device);
#endif
#ifdef CONFIG_BRCM_UARTB_IS_3250
	platform_device_register(&bcm3250_UARTB_device);
#endif
#ifdef CONFIG_BRCM_UARTC_IS_3250
	platform_device_register(&bcm3250_UARTC_device);
#endif

#if defined(CONFIG_BRCM_IKOS)
	/* the remaining devices do not exist in emulation */
	return 0;
#endif

	/* USB controllers */

#define ADD_USB(type, reg, irq) do { \
	if (!(usb_disable_mask & (1 << type##_id))) \
		pdevs[devno++] = brcm_new_usb_host(#type "-brcm", type##_id++, \
			BCHP_##reg##_REG_START, BRCM_IRQ_##irq); \
	} while (0)

	if (brcm_usb_enabled) {
#ifdef CONFIG_BRCM_USB_DISABLE_MASK
		unsigned long usb_disable_mask = CONFIG_BRCM_USB_DISABLE_MASK;
#else
		unsigned long usb_disable_mask = 0x00;
#endif
		struct platform_device *pdevs[16];
		int ehci_id = 0, ohci_id = 0, devno = 0, i;

		bchip_usb_init();

#if defined(BCHP_USB_EHCI_REG_START)
		ADD_USB(ehci, USB_EHCI, EHCI0_0);
#endif
#if defined(BCHP_USB_EHCI1_REG_START)
		ADD_USB(ehci, USB_EHCI1, EHCI0_1);
#endif
#if defined(BCHP_USB_EHCI2_REG_START)
		ADD_USB(ehci, USB_EHCI2, EHCI0_2);
#endif
#if defined(BCHP_USB_OHCI_REG_START)
		ADD_USB(ohci, USB_OHCI, OHCI0_0);
#endif
#if defined(BCHP_USB_OHCI1_REG_START)
		ADD_USB(ohci, USB_OHCI1, OHCI0_1);
#endif
#if defined(BCHP_USB_OHCI2_REG_START)
		ADD_USB(ohci, USB_OHCI2, OHCI0_2);
#endif
#if defined(BCHP_USB1_EHCI_REG_START)
		ADD_USB(ehci, USB1_EHCI, EHCI1_0);
#endif
#if defined(BCHP_USB1_EHCI1_REG_START)
		ADD_USB(ehci, USB1_EHCI1, EHCI1_1);
#endif
#if defined(BCHP_USB1_OHCI_REG_START)
		ADD_USB(ohci, USB1_OHCI, OHCI1_0);
#endif
#if defined(BCHP_USB1_OHCI1_REG_START)
		ADD_USB(ohci, USB1_OHCI1, OHCI1_1);
#endif

		for (i = 0; i < devno; i++)
			platform_device_add(pdevs[i]);
	}

	/* Network interfaces */

#if defined(CONFIG_BRCM_HAS_EMAC_0)
	brcm_alloc_macaddr(bcmemac_0_plat_data.macaddr);
	platform_device_register(&bcmemac_0_plat_dev);
#endif

#if defined(CONFIG_BRCM_HAS_EMAC_1)
	if (brcm_emac_1_enabled) {
		brcm_alloc_macaddr(bcmemac_1_plat_data.macaddr);
		if (brcm_enet_no_mdio)
			bcmemac_1_plat_data.phy_id = BRCM_PHY_ID_NONE;
		platform_device_register(&bcmemac_1_plat_dev);
	}
#endif

#if defined(CONFIG_BRCM_HAS_GENET)
	/*
	 * Supported GENET configurations:
	 *
	 * GENET_0 INT (7468)
	 * GENET_0 EXT (7468 alt)
	 * GENET_0 INT, GENET_1 MOCA (7420)
	 * GENET_0 EXT, GENET_1 MOCA (7420 alt)
	 * GENET_1 MOCA (7125) (deprecated)
	 * GENET_1 EXT (7019) (deprecated)
	 *
	 * 65nm chips use GENET (0) / MOCA_GENET (1) (deprecated)
	 * 40nm chips use GENET_0 / GENET_1
	 */
	if (brcm_enet_enabled) {
		int phy_type = BRCM_PHY_TYPE_INT;
		int id = 0;

#if defined(CONFIG_BRCM_HAS_GENET_0)

#if defined(CONFIG_BRCM_MOCA_ON_GENET_0)
		if (!brcm_moca_enabled)
			phy_type = brcm_ext_mii_mode;
		else {
			phy_type = BRCM_PHY_TYPE_MOCA;
			brcm_register_moca(id);
		}
#endif
#if defined(CONFIG_BCMGENET_0_GPHY)
		phy_type = brcm_ext_mii_mode;
#endif
		brcm_register_genet(id++, BCHP_GENET_0_SYS_REG_START,
			BRCM_IRQ_GENET_0_A, BRCM_IRQ_GENET_0_B, phy_type);
#endif /* defined(CONFIG_BRCM_HAS_GENET_0) */

#if defined(CONFIG_BRCM_HAS_GENET_1)

#if defined(CONFIG_BRCM_MOCA_ON_GENET_1)
		if (!brcm_moca_enabled)
			phy_type = brcm_ext_mii_mode;
		else {
			phy_type = BRCM_PHY_TYPE_MOCA;
			brcm_register_moca(id);
		}
#endif
#if defined(CONFIG_BCMGENET_1_GPHY)
		phy_type = brcm_ext_mii_mode;
#endif
		if (brcm_emac_1_enabled)
			brcm_register_genet(id++, BCHP_GENET_1_SYS_REG_START,
				BRCM_IRQ_GENET_1_A, BRCM_IRQ_GENET_1_B,
				phy_type);
#endif /* defined(CONFIG_BRCM_HAS_GENET_1) */
	}
#endif /* defined(CONFIG_BRCM_HAS_GENET) */

#if defined(CONFIG_BRCM_HAS_SDIO_V0)
	bchip_sdio_init(0, 0);
	platform_device_register(&sdio_plat_dev);
#endif

#if defined(CONFIG_BRCM_HAS_SDIO_V1)
	brcm_add_sdio_host(0, BCHP_SDIO_0_CFG_REG_START,
		BCHP_SDIO_0_HOST_REG_START, BRCM_IRQ_SDIO0);

#if defined(BCHP_SDIO_1_CFG_REG_START)
	brcm_add_sdio_host(1, BCHP_SDIO_1_CFG_REG_START,
		BCHP_SDIO_1_HOST_REG_START, BRCM_IRQ_SDIO1);
#endif /* defined(BCHP_SDIO_1_CFG_REG_START) */

#endif /* defined(CONFIG_BRCM_HAS_SDIO_V1) */

	return 0;
}

arch_initcall(platform_devices_setup);

#if defined(CONFIG_BRCM_FLASH)

/***********************************************************************
 * Flash device setup
 ***********************************************************************/

#ifdef BCHP_EBI_CS_BASE_5
#define NUM_CS			6
#else
#define NUM_CS			2
#endif

#define TYPE_NONE		0
#define TYPE_NOR		1
#define TYPE_NAND		2
#define TYPE_SPI		3
#define TYPE_MAX		TYPE_SPI

static const char type_names[][8] = { "NONE", "NOR", "NAND", "SPI" };

struct ebi_cs_info {
	int			type;
	unsigned long		start;
	unsigned long		len;
	int			width;
};

static struct ebi_cs_info cs_info[NUM_CS];

#ifdef CONFIG_BRCM_HAS_SPI
static int __init brcm_setup_spi_flash(int cs, int bus_num, int nr_parts,
	struct mtd_partition *parts)
{
	struct spi_board_info board_info;
	struct flash_platform_data *pdata;
	struct spi_master *master;

	master = spi_busnum_to_master(bus_num);
	if (!master) {
		printk(KERN_WARNING "%s: can't locate SPI master\n",
			__func__);
		return -ENODEV;
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->nr_parts = nr_parts;
	pdata->parts = parts;

	memset(&board_info, 0, sizeof(board_info));

	strcpy(board_info.modalias, "m25p80");
	board_info.bus_num = bus_num;
	board_info.chip_select = cs;
	board_info.mode = SPI_MODE_3;
	board_info.platform_data = pdata;

	if (spi_new_device(master, &board_info) == NULL) {
		printk(KERN_WARNING "%s: can't add SPI device\n",
			__func__);
		kfree(pdata);
		return -ENODEV;
	}

	return 0;
}

static int __init brcm_setup_spi_master(int cs, int bus_id)
{
	struct resource res[4];
	struct brcmspi_platform_data pdata;
	struct platform_device *pdev;
	memset(&pdata, 0, sizeof(pdata));

	pdata.flash_cs = cs;

	memset(&res, 0, sizeof(res));
	res[0].start = BPHYSADDR(BCHP_HIF_MSPI_REG_START);
	res[0].end = BPHYSADDR(BCHP_HIF_MSPI_REG_END) + 3;
	res[0].flags = IORESOURCE_MEM;

	res[1].start = BRCM_IRQ_HIF_SPI;
	res[1].end = BRCM_IRQ_HIF_SPI;
	res[1].flags = IORESOURCE_IRQ;

	res[2].start = BPHYSADDR(BCHP_BSPI_REG_START);
	res[2].end = BPHYSADDR(BCHP_BSPI_REG_END) + 3;
	res[2].flags = IORESOURCE_MEM;

	res[3].start = BPHYSADDR(BCHP_BSPI_RAF_REG_START);
	res[3].end = BPHYSADDR(BCHP_BSPI_RAF_REG_END) + 3;
	res[3].flags = IORESOURCE_MEM;

	pdev = platform_device_alloc("spi_brcmstb", bus_id);
	if (!pdev ||
	    platform_device_add_resources(pdev, res, 4) ||
	    platform_device_add(pdev)) {
		platform_device_put(pdev);
		return -ENODEV;
	}
	return 0;
}
#endif

static void __init brcm_setup_cs(int cs, int nr_parts,
	struct mtd_partition *parts)
{
	struct platform_device *pdev;

	switch (cs_info[cs].type) {
	case TYPE_NOR: {
#ifdef CONFIG_BRCM_HAS_NOR
		struct physmap_flash_data pdata;
		struct resource res;
		static int nor_id;

		memset(&res, 0, sizeof(res));
		memset(&pdata, 0, sizeof(pdata));

		pdata.width = cs_info[cs].width;
		pdata.nr_parts = nr_parts;
		pdata.parts = parts;

		res.start = cs_info[cs].start;
		res.end = cs_info[cs].start + cs_info[cs].len - 1;
		res.flags = IORESOURCE_MEM;

		pdev = platform_device_alloc("physmap-flash", nor_id++);
		if (!pdev ||
		    platform_device_add_resources(pdev, &res, 1) ||
		    platform_device_add_data(pdev, &pdata, sizeof(pdata)) ||
		    platform_device_add(pdev))
			platform_device_put(pdev);
#endif
		break;
	}
	case TYPE_NAND: {
		struct brcmnand_platform_data pdata;
		static int nand_id;

		pdata.chip_select = cs;
		pdata.nr_parts = nr_parts;
		pdata.parts = parts;

		pdev = platform_device_alloc("brcmnand", nand_id++);
		if (!pdev ||
		    platform_device_add_data(pdev, &pdata, sizeof(pdata)) ||
		    platform_device_add(pdev))
			platform_device_put(pdev);
		break;
	}
	case TYPE_SPI: {
#ifdef CONFIG_BRCM_HAS_SPI
		const int bus_num = 0;
		static int spi_master_registered;
		int ret;

		if (!spi_master_registered) {
			ret = brcm_setup_spi_master(cs, bus_num);
			if (ret) {
				printk(KERN_WARNING
					"%s: can't register SPI master "
					"(error %d)\n", __func__, ret);
				break;
			}
			spi_master_registered = 1;
		}

		ret = brcm_setup_spi_flash(cs, bus_num, nr_parts, parts);
		if (ret < 0) {
			printk(KERN_WARNING
				"%s: can't register SPI flash (error %d)\n",
				__func__, ret);
			break;
		}
#endif
		break;
	}
	default:
		BUG();
	}
}

static int __initdata noflash;
static int __initdata nandcs[NUM_CS];

static struct map_info brcm_dummy_map = {
	.name			= "DUMMY",
};

static int __init brcmstb_mtd_setup(void)
{
	struct mtd_partition *parts;
	int nr_parts;
	int i, first = -1, primary = -1, primary_type = TYPE_NAND;

	if (noflash)
		return 0;

	nr_parts = board_get_partition_map(&parts);
	if (nr_parts <= 0) {
		struct mtd_info *mtd;

		/*
		 * mtd0 is normally the rootfs partition.  If it is missing,
		 * create a dummy partition so that any person or script
		 * trying to mount or erase mtd0 does not corrupt the
		 * whole flash device.
		 */
		mtd = do_map_probe("map_absent", &brcm_dummy_map);
		if (mtd)
			add_mtd_device(mtd);

		printk(KERN_WARNING
			"warning: unable to build a flash partition map, "
			"using entire device\n");

		nr_parts = 0;
		parts = NULL;
	}

	/* parse CFE FLASH_TYPE variable */
	for (i = TYPE_NOR; i <= TYPE_MAX; i++)
		if (strcmp(brcm_mtd_flash_type, type_names[i]) == 0)
			primary_type = i;

	/* scan each chip select to see what (if anything) lives there */
	for (i = 0; i < NUM_CS; i++) {
		u32 base, size, config __maybe_unused;

		cs_info[i].type = TYPE_NONE;

		base = BDEV_RD(BCHP_EBI_CS_BASE_0 + (i * 8));
		size = base & 0x0f;

		cs_info[i].start = (base >> (13 + size)) << (13 + size);
		cs_info[i].len = 8192UL << (base & 0xf);

#ifdef BCHP_EBI_CS_CONFIG_0
		config = BDEV_RD(BCHP_EBI_CS_CONFIG_0 + (i * 8));
		if (config & BCHP_EBI_CS_CONFIG_0_enable_MASK)
			cs_info[i].type = TYPE_NOR;

		if (config & BCHP_EBI_CS_CONFIG_0_dest_size_MASK)
			cs_info[i].width = 2;
		else
			cs_info[i].width = 1;
#endif
#ifdef BCHP_EBI_CS_SPI_SELECT
		/*
		 * bits 7:0 - owned by HW (at most one bit active)
		 * bits 23:16 - owned by SW (any number of '1' bits OK)
		 * A '1' in either position means there is a SPI chip there.
		 *
		 * 65nm chips don't have SW bits 15:8 so EXTRA_SPI_CS
		 * should be set at compile time for multiple SPI flashes.
		 */
		if ((BDEV_RD(BCHP_EBI_CS_SPI_SELECT) & (0x101 << i)) ||
				(EXTRA_SPI_CS & (0x01 << i)))
			cs_info[i].type = TYPE_SPI;
#endif
#ifdef BCHP_NAND_CS_NAND_SELECT
		if (BDEV_RD(BCHP_NAND_CS_NAND_SELECT) & (0x100 << i))
			cs_info[i].type = TYPE_NAND;
#endif

		if (cs_info[i].type != TYPE_NAND && nandcs[i] != 0) {
			cs_info[i].type = TYPE_NAND;
		} else {
			/*
			 * nandcs = ineligible to be primary.  The partition
			 * information reported by CFE is not for the NAND
			 * flash if CFE doesn't know the system has a
			 * NAND flash.
			 */
			if (primary == -1 && primary_type == cs_info[i].type)
				primary = i;
		}
		if (first == -1 && cs_info[i].type != TYPE_NONE)
			first = i;
	}

	if (primary == -1) {
		if (first == -1) {
			printk(KERN_INFO "EBI: No flash devices detected\n");
			return -ENODEV;
		}
		primary = first;
		primary_type = cs_info[primary].type;
	}

	/* set up primary first, so that it owns mtd0/mtd1/(mtd2) */
	printk(KERN_INFO "EBI CS%d: setting up %s flash (primary)\n", primary,
		type_names[primary_type]);
	brcm_setup_cs(primary, nr_parts, parts);

	for (i = 0; i < NUM_CS; i++) {
		if (i != primary && cs_info[i].type != TYPE_NONE) {
			printk(KERN_INFO "EBI CS%d: setting up %s flash\n", i,
				type_names[cs_info[i].type]);
			brcm_setup_cs(i, 0, NULL);
		}
	}

	return 0;
}

/*
 * late_initcall means the flash drivers are already loaded, so we control
 * the order in which the /dev/mtd* devices get created.
 */
late_initcall(brcmstb_mtd_setup);

static int __init noflash_setup(char *str)
{
	noflash = 1;
	return 0;
}

__setup("noflash", noflash_setup);

static int __init nandcs_setup(char *str)
{
	int opts[NUM_CS + 1], i;

	if (*str == 0)
		return 0;
	get_options(str + 1, NUM_CS, opts);

	for (i = 0; i < opts[0]; i++) {
		int cs = opts[i + 1];
		if ((cs >= 0) && (cs < NUM_CS))
			nandcs[cs] = 1;
	}
	return 0;
}

__setup("nandcs", nandcs_setup);

#endif /* defined(CONFIG_BRCM_FLASH) */

/***********************************************************************
 * Miscellaneous platform-specific functions
 ***********************************************************************/

void __init bus_error_init(void)
{
}

static void brcm_machine_restart(char *command)
{
/* PR21527 - Fix SMP reboot problem */
#ifdef CONFIG_SMP
	smp_send_stop();
	udelay(10);
#endif

#ifdef BCHP_SUN_TOP_CTRL_SW_RESET
	BDEV_WR_F_RB(SUN_TOP_CTRL_RESET_CTRL, master_reset_en, 1);
	BDEV_WR_F_RB(SUN_TOP_CTRL_SW_RESET, chip_master_reset, 1);
#else
	BDEV_WR_F_RB(SUN_TOP_CTRL_RESET_SOURCE_ENABLE,
		sw_master_reset_enable, 1);
	BDEV_WR_F_RB(SUN_TOP_CTRL_SW_MASTER_RESET, chip_master_reset, 1);
#endif

	while (1)
		;
}

static void brcm_machine_halt(void)
{
#ifdef CONFIG_BRCM_IRW_HALT
	/* ultra low power standby - on wakeup, system will restart */
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, irw_top_sw_pwroff, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, irw_top_sw_pwroff, 1);
#endif
	while (1)
		;
}

char *__devinit brcmstb_pcibios_setup(char *str)
{
	/* implement "pci=off" command line option */
	if (!strcmp(str, "off")) {
		brcm_pci_enabled = 0;
		brcm_sata_enabled = 0;
		brcm_pcie_enabled = 0;
		return NULL;
	}
	return str;
}

void __init plat_mem_setup(void)
{
	_machine_restart = brcm_machine_restart;
	_machine_halt = brcm_machine_halt;
	pm_power_off = brcm_machine_halt;

	panic_timeout = 180;

#ifdef CONFIG_PCI
	pcibios_plat_setup = brcmstb_pcibios_setup;
#endif

#if defined(CONFIG_BRCM_HAS_16550) || defined(CONFIG_SERIAL_BCM3250_TTYS)
	add_preferred_console("ttyS", CONFIG_BRCM_CONSOLE_DEVICE, "115200");
#else
	add_preferred_console("ttyBCM", CONFIG_BRCM_CONSOLE_DEVICE, "115200");
#endif
}
