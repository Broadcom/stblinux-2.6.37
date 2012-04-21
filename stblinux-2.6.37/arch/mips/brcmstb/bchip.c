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
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/compiler.h>
#include <linux/mmc/sdhci-pltfm.h>

#include <asm/mipsregs.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/r4kcache.h>
#include <asm/asm-offsets.h>
#include <asm/inst.h>
#include <asm/fpu.h>
#include <asm/hazards.h>
#include <asm/cpu-features.h>
#include <asm/brcmstb/brcmstb.h>
#include <dma-coherence.h>

#include "../drivers/mmc/host/sdhci.h"

/* chip features */
int brcm_sata_enabled;
int brcm_enet_enabled;
int brcm_pci_enabled;
int brcm_pcie_enabled;
int brcm_smp_enabled;
int brcm_enet1_enabled;
int brcm_moca_enabled;
int brcm_usb_enabled;
int brcm_pm_enabled;

/* synchronize writes to shared registers */
DEFINE_SPINLOCK(brcm_magnum_spinlock);
EXPORT_SYMBOL(brcm_magnum_spinlock);

/***********************************************************************
 * Per-chip operations
 ***********************************************************************/

int __init bchip_strap_ram_size(void)
{
	u32 reg;
#if defined(CONFIG_BCM7405)
	const unsigned int mc[] = { 32, 64, 128, 256 };
	const unsigned int mode[] = { 4, 2, 1, 0, 2, 1, 0, 0 };
	unsigned int tmp;
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr0_device_config);
	tmp = mc[reg & 3];
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr_configuration);
	return tmp * mode[reg];
#else
	reg = 0;
	return reg;
#endif
}

#define ALT_CHIP_ID(chip, rev) do { \
	u32 arg_id = 0x ## chip; \
	const u8 rev_name[] = #rev; \
	u32 arg_rev = ((rev_name[0] - 'a') << 4) | (rev_name[1] - '0'); \
	if (!kernel_chip_id && arg_id == chip_id) { \
		kernel_chip_id = arg_id; \
		kernel_chip_rev = arg_rev; \
	} \
	} while (0)

#define MAIN_CHIP_ID(chip, rev) do { \
	u32 arg_id = 0x ## chip; \
	const u8 rev_name[] = #rev; \
	u32 arg_rev = ((rev_name[0] - 'a') << 4) | (rev_name[1] - '0'); \
	if (!kernel_chip_id) { \
		kernel_chip_id = arg_id; \
		kernel_chip_rev = arg_rev; \
	} \
	} while (0)

/*
 * NOTE: This is a quick sanity test to catch known incompatibilities and
 * obvious chip ID mismatches.  It is not comprehensive.  Higher revs may
 * or may not maintain software compatibility.
 *
 * MAIN_CHIP_ID() must always be the final entry.
 */

void __init bchip_check_compat(void)
{
	u32 chip_id = BRCM_CHIP_ID(), chip_rev = BRCM_CHIP_REV();
	u32 kernel_chip_id = 0, kernel_chip_rev = 0;

#if defined(CONFIG_BCM7125)
	ALT_CHIP_ID(7019, c0);
	ALT_CHIP_ID(7025, c0);
	ALT_CHIP_ID(7116, c0);
	ALT_CHIP_ID(7117, c0);
	ALT_CHIP_ID(7119, c0);
	ALT_CHIP_ID(7120, c0);
	MAIN_CHIP_ID(7125, c0);
#elif defined(CONFIG_BCM7231)
	MAIN_CHIP_ID(7231, a0);
#elif defined(CONFIG_BCM7340)
	ALT_CHIP_ID(7350, b0);
	MAIN_CHIP_ID(7340, b0);
#elif defined(CONFIG_BCM7344)
	MAIN_CHIP_ID(7344, a0);
#elif defined(CONFIG_BCM7346)
	MAIN_CHIP_ID(7346, a0);
#elif defined(CONFIG_BCM7358)
	/* 7358 kernel can boot on 7552, but not vice-versa */
	ALT_CHIP_ID(7552, a0);
	MAIN_CHIP_ID(7358, a0);
#elif defined(CONFIG_BCM7552)
	MAIN_CHIP_ID(7552, a0);
#elif defined(CONFIG_BCM7405)
	ALT_CHIP_ID(7413, a0);
	MAIN_CHIP_ID(7405, b0);
#elif defined(CONFIG_BCM7408)
	MAIN_CHIP_ID(7408, b0);
#elif defined(CONFIG_BCM7420)
	ALT_CHIP_ID(3320, c1);
	ALT_CHIP_ID(7220, c1);
	ALT_CHIP_ID(7409, c1);
	ALT_CHIP_ID(7410, c1);
	MAIN_CHIP_ID(7420, c1);
#elif defined(CONFIG_BCM7425)
	MAIN_CHIP_ID(7425, a0);
#elif defined(CONFIG_BCM7468)
	MAIN_CHIP_ID(7468, b0);
#elif defined(CONFIG_BCM7550)
	MAIN_CHIP_ID(7550, a0);
#endif
	if (!kernel_chip_id)
		return;

	if (chip_id != kernel_chip_id)
		cfe_die("PANIC: BCM%04x kernel cannot boot on "
			"BCM%04x chip.\n", kernel_chip_id, chip_id);

	if (chip_rev < kernel_chip_rev)
		cfe_die("PANIC: This kernel requires BCM%04x rev >= %02X "
			"(P%02x)\n", kernel_chip_id,
			kernel_chip_rev + 0xa0, kernel_chip_rev + 0x10);
}

/***********************************************************************
 * MIPS features, caches, and bus interface
 ***********************************************************************/

void bchip_mips_setup(void)
{
#if   defined(CONFIG_BMIPS3300)

	unsigned long cbr = BMIPS_GET_CBR();

	/* Set BIU to async mode */
	set_c0_brcm_bus_pll(BIT(22));
	__sync();

#ifdef BCHP_MISB_BRIDGE_WG_MODE_N_TIMEOUT
	/* Enable write gathering */
	BDEV_WR_RB(BCHP_MISB_BRIDGE_WG_MODE_N_TIMEOUT, 0x264);

	/* Enable split mode */
	BDEV_WR_RB(BCHP_MISB_BRIDGE_MISB_SPLIT_MODE, 0x1);
	__sync();
#endif

	/* put the BIU back in sync mode */
	clear_c0_brcm_bus_pll(BIT(22));

	/* clear BHTD to enable branch history table */
	clear_c0_brcm_reset(BIT(16));

	/* Flush and enable RAC */
	DEV_WR_RB(cbr + BMIPS_RAC_CONFIG, 0x100);
	DEV_WR_RB(cbr + BMIPS_RAC_CONFIG, 0xf);
	DEV_WR_RB(cbr + BMIPS_RAC_ADDRESS_RANGE, 0x0fff0000);

#elif defined(CONFIG_BMIPS4380)

	unsigned long cbr = BMIPS_GET_CBR();

	/* CRBMIPS438X-164: CBG workaround */
	switch (read_c0_prid()) {
	case 0x2a040:
	case 0x2a042:
	case 0x2a044:
	case 0x2a060:
		DEV_UNSET(cbr + BMIPS_L2_CONFIG, 0x07000000);
	}

	/* clear BHTD to enable branch history table */
	clear_c0_brcm_config_0(BIT(21));

	/* XI enable */
	if (kernel_uses_smartmips_rixi)
		set_c0_brcm_config_0(BIT(23));

#elif defined(CONFIG_BMIPS5000)

	/* enable RDHWR, BRDHWR */
	set_c0_brcm_config(BIT(17) | BIT(21));

	if (kernel_uses_smartmips_rixi) {
		/* XI enable */
		set_c0_brcm_config(BIT(27));

		/* enable MIPS32R2 ROR instruction for XI TLB handlers */
		__asm__ __volatile__(
		"	li	$8, 0x5a455048\n"
		"	.word	0x4088b00f\n"	/* mtc0 $8, $22, 15 */
		"	nop; nop; nop\n"
		"	.word	0x4008b008\n"	/* mfc0 $8, $22, 8 */
		"	lui	$9, 0x0100\n"
		"	or	$8, $9\n"
		"	.word	0x4088b008\n"	/* mtc0 $8, $22, 8 */
		: : : "$8", "$9");
	}

#endif
}

/***********************************************************************
 * Common operations for all chips
 ***********************************************************************/

#ifdef CONFIG_BRCM_HAS_SATA3

#ifdef CONFIG_CPU_BIG_ENDIAN
#define DATA_ENDIAN             2       /* AHCI->DDR inbound accesses */
#define MMIO_ENDIAN             2       /* MIPS->AHCI outbound accesses */
#else
#define DATA_ENDIAN             0
#define MMIO_ENDIAN             0
#endif /* CONFIG_CPU_BIG_ENDIAN */

/* SATA3 SSC per-port bitfield */
static u32 sata3_enable_ssc;

#define SATA3_MDIO_TXPMD_0_REG_BANK	0x1A0
#define SATA3_MDIO_BRIDGE_BASE		(BCHP_SATA_GRB_REG_START + 0x100)
#define SATA3_MDIO_BASE_REG_ADDR	(SATA3_MDIO_BRIDGE_BASE + 0x8F * 4)

#define SATA_AHCI_GHC_PORTS_IMPLEMENTED	(BCHP_SATA_AHCI_GHC_REG_START + 0xC)

#define SATA3_TXPMD_CONTROL1			0x81
#define SATA3_TXPMD_TX_FREQ_CTRL_CONTROL1	0x82
#define SATA3_TXPMD_TX_FREQ_CTRL_CONTROL2	0x83
#define SATA3_TXPMD_TX_FREQ_CTRL_CONTROL3	0x84

static inline void brcm_sata3_mdio_wr_reg(u32 bank, unsigned int ofs, u32 msk,
		u32 enable)
{
	u32 tmp;
	BDEV_WR(SATA3_MDIO_BASE_REG_ADDR, bank);
	/* Read, mask, enable */
	tmp = BDEV_RD(ofs * 4 + SATA3_MDIO_BRIDGE_BASE);
	tmp = (tmp & msk) | enable;
	/* Write */
	BDEV_WR(ofs * 4 + SATA3_MDIO_BRIDGE_BASE, tmp);
}

static void brcm_sata3_init_freq(int port, int ssc_enable)
{
	u32 bank = SATA3_MDIO_TXPMD_0_REG_BANK + port * 0x10;

	if (ssc_enable)
		pr_info("SATA3: enabling SSC on port %d\n", port);

	/* TXPMD_control1 - enable SSC force */
	brcm_sata3_mdio_wr_reg(bank, SATA3_TXPMD_CONTROL1, 0xFFFFFFFC,
			0x00000003);

	/* TXPMD_tx_freq_ctrl_control2 - set fixed min freq */
	brcm_sata3_mdio_wr_reg(bank, SATA3_TXPMD_TX_FREQ_CTRL_CONTROL2,
			0xFFFFFC00, 0x000003DF);

	/*
	 * TXPMD_tx_freq_ctrl_control3 - set fixed max freq
	 *  If ssc_enable == 0, center frequencies
	 *  Otherwise, spread spectrum frequencies
	 */
	if (ssc_enable)
		brcm_sata3_mdio_wr_reg(bank, SATA3_TXPMD_TX_FREQ_CTRL_CONTROL3,
				0xFFFFFC00, 0x00000083);
	else
		brcm_sata3_mdio_wr_reg(bank, SATA3_TXPMD_TX_FREQ_CTRL_CONTROL3,
				0xFFFFFC00, 0x000003DF);
}

/* Check up to 32 ports, although we typically only have 2 */
#define SATA_MAX_CHECK_PORTS	32

/*
 * Check commandline for 'sata3_ssc' options. They can be specified in 2 ways:
 *  (1) 'sata3_ssc'     -> enable SSC on all ports
 *  (2) 'sata3_ssc=x,y' -> enable SSC on specific port(s), given a comma-
 *                         separated list of port numbers
 */
static int __init sata3_ssc_setup(char *str)
{
	int opts[SATA_MAX_CHECK_PORTS + 1], i;

	if (*str == '\0') {
		/* enable SSC on all ports */
		sata3_enable_ssc = ~0;
		return 0;
	}
	get_options(str + 1, SATA_MAX_CHECK_PORTS, opts);

	for (i = 0; i < opts[0]; i++) {
		int port = opts[i + 1];
		if ((port >= 0) && (port < SATA_MAX_CHECK_PORTS))
			sata3_enable_ssc |= 1 << port;
	}

	return 0;
}

__setup("sata3_ssc", sata3_ssc_setup);

#endif /* CONFIG_BRCM_HAS_SATA3 */

void bchip_sata3_init(void)
{
#ifdef CONFIG_BRCM_HAS_SATA3
	int i, ports = fls(BDEV_RD(SATA_AHCI_GHC_PORTS_IMPLEMENTED));

	BDEV_WR(BCHP_SATA_TOP_CTRL_BUS_CTRL, (DATA_ENDIAN << 4) |
			(DATA_ENDIAN << 2) | (MMIO_ENDIAN << 0));

	for (i = 0; i < ports; i++)
		brcm_sata3_init_freq(i, sata3_enable_ssc & (1 << i));
#endif
}

#ifdef CONFIG_CPU_LITTLE_ENDIAN
#define USB_ENDIAN		0x03 /* !WABO !FNBO FNHW BABO */
#else
#define USB_ENDIAN		0x0e /* WABO FNBO FNHW !BABO */
#endif

#define USB_ENDIAN_MASK		0x0f
#define USB_IOC			BCHP_USB_CTRL_SETUP_IOC_MASK
#define USB_IPP			BCHP_USB_CTRL_SETUP_IPP_MASK

#define USB_REG(x, y)		(x + BCHP_USB_CTRL_##y - \
				 BCHP_USB_CTRL_REG_START)

static void bchip_usb_init_one(int id, uintptr_t base)
{
	/* endianness setup */
	BDEV_UNSET_RB(USB_REG(base, SETUP), USB_ENDIAN_MASK);
	BDEV_SET_RB(USB_REG(base, SETUP), USB_ENDIAN);

	/* power control setup */
#ifdef CONFIG_BRCM_OVERRIDE_USB

#ifdef CONFIG_BRCM_FORCE_USB_OC_LO
	BDEV_SET(USB_REG(base, SETUP), USB_IOC);
#else
	BDEV_UNSET(USB_REG(base, SETUP), USB_IOC);
#endif

#ifdef CONFIG_BRCM_FORCE_USB_PWR_LO
	BDEV_SET(USB_REG(base, SETUP), USB_IPP);
#else
	BDEV_UNSET(USB_REG(base, SETUP), USB_IPP);
#endif

#else /* CONFIG_BRCM_OVERRIDE_USB */
	if ((BDEV_RD(USB_REG(base, SETUP)) & USB_IOC) == 0) {
		printk(KERN_WARNING "USB%d: IOC was not set by the bootloader;"
			" forcing default settings\n", id);
		BDEV_SET(USB_REG(base, SETUP), USB_IOC);
		BDEV_SET(USB_REG(base, SETUP), USB_IPP);
	}
#endif /* CONFIG_BRCM_OVERRIDE_USB */

	printk(KERN_INFO "USB%d: power enable is active %s; overcurrent is "
		"active %s\n", id,
		BDEV_RD(USB_REG(base, SETUP)) & USB_IPP ? "low" : "high",
		BDEV_RD(USB_REG(base, SETUP)) & USB_IOC ? "low" : "high");

	/* PR45703 - for OHCI->SCB bridge lockup */
	BDEV_UNSET(USB_REG(base, OBRIDGE),
		BCHP_USB_CTRL_OBRIDGE_OBR_SEQ_EN_MASK);

	/* Disable EHCI transaction combining */
	BDEV_UNSET(USB_REG(base, EBRIDGE),
		BCHP_USB_CTRL_EBRIDGE_EBR_SEQ_EN_MASK);

	/* SWLINUX-1705: Avoid OUT packet underflows */
	BDEV_UNSET(USB_REG(base, EBRIDGE),
		BCHP_USB_CTRL_EBRIDGE_EBR_SCB_SIZE_MASK);
	BDEV_SET(USB_REG(base, EBRIDGE),
		0x08 << BCHP_USB_CTRL_EBRIDGE_EBR_SCB_SIZE_SHIFT);

#if defined(CONFIG_BRCM_HAS_1GB_MEMC1)
	/* enable access to SCB1 */
	BDEV_SET(USB_REG(base, SETUP), BIT(14));
#endif

#if defined(BCHP_USB_CTRL_GENERIC_CTL_1_PLL_SUSPEND_EN_MASK)
	BDEV_SET(USB_REG(base, GENERIC_CTL_1),
		BCHP_USB_CTRL_GENERIC_CTL_1_PLL_SUSPEND_EN_MASK);
#elif defined(BCHP_USB_CTRL_GENERIC_CTL_PLL_SUSPEND_EN_MASK)
	BDEV_SET(USB_REG(base, GENERIC_CTL),
		BCHP_USB_CTRL_GENERIC_CTL_PLL_SUSPEND_EN_MASK);
#elif defined(BCHP_USB_CTRL_PLL_CTL_1_PLL_SUSPEND_EN_MASK)
	BDEV_SET(USB_REG(base, PLL_CTL_1),
		BCHP_USB_CTRL_PLL_CTL_1_PLL_SUSPEND_EN_MASK);
#elif defined(BCHP_USB_CTRL_PLL_CTL_PLL_SUSPEND_EN_MASK)
	BDEV_SET(USB_REG(base, PLL_CTL),
		BCHP_USB_CTRL_PLL_CTL_PLL_SUSPEND_EN_MASK);
#endif

}

void bchip_usb_init(void)
{
	bchip_usb_init_one(0, BCHP_USB_CTRL_REG_START);
#ifdef BCHP_USB1_CTRL_REG_START
	bchip_usb_init_one(1, BCHP_USB1_CTRL_REG_START);
#endif
}

#if defined(CONFIG_BRCM_HAS_MOCA)
void bchip_moca_init(void)
{
#ifdef BCHP_SUN_TOP_CTRL_SW_RESET
	BDEV_WR_F_RB(SUN_TOP_CTRL_SW_RESET, moca_sw_reset, 0);
#else
	BDEV_WR_F_RB(SUN_TOP_CTRL_SW_INIT_0_CLEAR, moca_sw_init, 1);
#endif

#ifdef BCHP_MOCA_HOSTMISC_SW_RESET_moca_enet_reset_MASK
	BDEV_WR_F_RB(MOCA_HOSTMISC_SW_RESET, moca_enet_reset, 0);
#endif

#if   defined(CONFIG_BCM7125)
	BDEV_WR_F_RB(CLKGEN_MISC_CLOCK_SELECTS, CLOCK_SEL_ENET_CG_MOCA, 0);
	BDEV_WR_F_RB(CLKGEN_MISC_CLOCK_SELECTS, CLOCK_SEL_GMII_CG_MOCA, 0);
#elif defined(CONFIG_BCM7340)
	BDEV_WR_F_RB(CLKGEN_MISC_CLOCK_SELECTS, CLOCK_SEL_ENET_CG_MOCA, 1);
	BDEV_WR_F_RB(CLKGEN_MISC_CLOCK_SELECTS, CLOCK_SEL_GMII_CG_MOCA, 0);
#elif defined(CONFIG_BCM7408) || defined(CONFIG_BCM7420)
	BDEV_WR_F_RB(CLK_MISC, MOCA_ENET_GMII_TX_CLK_SEL, 0);
#endif
}
#endif

#if defined(CONFIG_BRCM_SDIO)

/* Use custom I/O accessors to avoid readl/readw byte swapping in BE mode */

static u32 sdhci_brcm_readl(struct sdhci_host *host, int reg)
{
	return __raw_readl(host->ioaddr + reg);
}

static u16 sdhci_brcm_readw(struct sdhci_host *host, int reg)
{
	return __raw_readw(host->ioaddr + reg);
}

static void sdhci_brcm_writel(struct sdhci_host *host, u32 val, int reg)
{
	__raw_writel(val, host->ioaddr + reg);
}

static void sdhci_brcm_writew(struct sdhci_host *host, u16 val, int reg)
{
	__raw_writew(val, host->ioaddr + reg);
}

static struct sdhci_ops __maybe_unused sdhci_be_ops = {
	.read_l			= sdhci_brcm_readl,
	.read_w			= sdhci_brcm_readw,
	.write_l		= sdhci_brcm_writel,
	.write_w		= sdhci_brcm_writew,
};

struct sdhci_pltfm_data sdhci_brcm_pdata = { };

static int nommc;

static int __init nommc_setup(char *str)
{
	nommc = 1;
	return 0;
}

__setup("nommc", nommc_setup);

int __init bchip_sdio_init(int id, uintptr_t cfg_base)
{
#define SDIO_REG(x, y)		(x + BCHP_SDIO_0_CFG_##y - \
				 BCHP_SDIO_0_CFG_REG_START)

/*
 * The following chips have SDIO issues and will not run correctly
 * at 50MHz so disable them.
 */
#if defined(CONFIG_BCM7425B0) || defined(CONFIG_BCM7429A0) || \
	defined(CONFIG_BCM7435A0)
	printk(KERN_INFO "SDIO_%d: disabled due to chip issues\n", id);
	return -ENODEV;
#endif
	if (nommc) {
		printk(KERN_INFO "SDIO_%d: disabled via command line\n", id);
		return -ENODEV;
	}
	if (BDEV_RD(SDIO_REG(cfg_base, SCRATCH)) & 0x01) {
		printk(KERN_INFO "SDIO_%d: disabled by bootloader\n", id);
		return -ENODEV;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 37)
	printk(KERN_INFO "SDIO_%d: this core requires Linux 2.6.37 or higher; "
		"disabling\n", id);
	return -ENODEV;
#endif
	printk(KERN_INFO "SDIO_%d: enabling controller\n", id);

	BDEV_UNSET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL1), 0xf000);
	BDEV_UNSET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL2), 0x00ff);
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	/* FRAME_NHW | BUFFER_ABO */
	BDEV_SET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL1), 0x3000);
#else
	/* WORD_ABO | FRAME_NBO | FRAME_NHW */
	BDEV_SET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL1), 0xe000);
	/* address swap only */
	BDEV_SET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL2), 0x0050);
	sdhci_brcm_pdata.ops = &sdhci_be_ops;
#endif

#if defined(CONFIG_BCM7231B0) || defined(CONFIG_BCM7346B0)
	BDEV_SET(SDIO_REG(cfg_base, CAP_REG1), BIT(31));	/* Override=1 */
#endif

#if defined(CONFIG_BCM7344B0)
	BDEV_UNSET(SDIO_REG(cfg_base, CAP_REG0), BIT(19));	/* Highspd=0 */
	BDEV_SET(SDIO_REG(cfg_base, CAP_REG1), BIT(31));	/* Override=1 */
#endif

	return 0;
}
#endif /* defined(CONFIG_BRCM_SDIO) */

void __init bchip_set_features(void)
{
#if defined(CONFIG_BRCM_HAS_SATA)
	brcm_sata_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_EMAC_0) || defined(CONFIG_BRCM_HAS_GENET)
	brcm_enet_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_PCI23)
	brcm_pci_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_PCIE)
	brcm_pcie_enabled = 1;
#endif
#if defined(CONFIG_SMP)
	brcm_smp_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_EMAC_1) || defined(CONFIG_BRCM_HAS_GENET_1)
	brcm_enet1_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_MOCA)
	brcm_moca_enabled = 1;
#endif
#if defined(CONFIG_BRCM_PM)
	brcm_pm_enabled = 1;
#endif
	brcm_usb_enabled = 1;

	/* now remove any features disabled in hardware */

#ifdef CONFIG_BCM7405
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS, otp_option_sata_disable))
		brcm_sata_enabled = 0;

	switch (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS,
		otp_option_product_id)) {
	case 0x0:
		/* 7405/7406 */
		break;
	case 0x1:
		/* 7466 */
		brcm_pci_enabled = 0;
		brcm_enet1_enabled = 0;
		break;
	case 0x3:
		/* 7106 */
		brcm_enet1_enabled = 0;
		brcm_smp_enabled = 0;
		break;
	case 0x4:
	case 0x6:
		/* 7205/7213 */
		brcm_enet1_enabled = 0;
		break;
	}
#endif

#ifdef BCHP_SUN_TOP_CTRL_OTP_OPTION_STATUS_0_otp_option_sata_disable_MASK
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
			otp_option_sata_disable) == 1)
		brcm_sata_enabled = 0;
#endif

#ifdef BCHP_SUN_TOP_CTRL_OTP_OPTION_STATUS_0_otp_option_enet_disable_MASK
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
			otp_option_enet_disable) == 1)
		brcm_enet_enabled = 0;
#endif

#ifdef BCHP_SUN_TOP_CTRL_OTP_OPTION_STATUS_0_otp_option_usb_disable_MASK
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
			otp_option_usb_disable) == 1)
		brcm_usb_enabled = 0;
#endif

#ifdef BCHP_SUN_TOP_CTRL_OTP_OPTION_STATUS_0_otp_option_moca_disable_MASK
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
			otp_option_moca_disable) == 1)
		brcm_moca_enabled = 0;
#endif

#if defined(CONFIG_BCM7125) || defined(CONFIG_BCM7340) || \
	defined(CONFIG_BCM7420)

	switch (BRCM_CHIP_ID()) {
	case 0x7340:
		if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
				otp_option_product_id) != 1)
			break;
		/* 7350, 7352 (alt): fall through */
	case 0x7019:
	case 0x7117:
	case 0x7119:
	case 0x7350:
	case 0x7352:
	case 0x7409:
		if (brcm_moca_enabled) {
			/* MOCA_GENET is usable - enable and scan it */
			BDEV_WR_F_RB(SUN_TOP_CTRL_SW_RESET, moca_sw_reset, 0);
			BDEV_WR_F_RB(MOCA_HOSTMISC_SW_RESET, moca_enet_reset,
				0);
		} else {
			/* MOCA_GENET regs are inaccessible - don't touch it */
			brcm_enet1_enabled = 0;
		}

		/* MoCA PHY is always disabled on these bondouts */
		brcm_moca_enabled = 0;
		break;
	}
#endif

#ifdef BCHP_SUN_TOP_CTRL_OTP_OPTION_STATUS_0_otp_option_pcie_disable_MASK
	if (BDEV_RD_F(SUN_TOP_CTRL_OTP_OPTION_STATUS_0,
			otp_option_pcie_disable) == 1)
		brcm_pcie_enabled = 0;
#endif

#ifdef CONFIG_BCM7425
	/* disable PCIe initialization in EP mode */
	if (BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_rc_ep) == 0)
		brcm_pcie_enabled = 0;
#endif
}

void __init bchip_early_setup(void)
{
#if defined(CONFIG_BRCM_HAS_WKTMR)
	struct wktmr_time t;

	BDEV_WR_F_RB(WKTMR_EVENT, wktmr_alarm_event, 1);
	BDEV_WR_F_RB(WKTMR_PRESCALER, wktmr_prescaler, WKTMR_FREQ);
	BDEV_WR_F_RB(WKTMR_COUNTER, wktmr_counter, 0);

	/* wait for first tick so we know the counter is ready to use */
	wktmr_read(&t);
	while (wktmr_elapsed(&t) == 0)
		;
#endif

#ifdef CONFIG_PCI
	if (brcm_pcie_enabled)
		brcm_early_pcie_setup();
#endif
}

/***********************************************************************
 * Simulate privileged instructions (RDHWR, MFC0) and unaligned accesses
 ***********************************************************************/

#define OPCODE 0xfc000000
#define BASE   0x03e00000
#define RT     0x001f0000
#define OFFSET 0x0000ffff
#define LL     0xc0000000
#define SC     0xe0000000
#define SPEC0  0x00000000
#define SPEC3  0x7c000000
#define RD     0x0000f800
#define FUNC   0x0000003f
#define SYNC   0x0000000f
#define RDHWR  0x0000003b

#define BRDHWR 0xec000000
#define OP_MFC0 0x40000000

int brcm_simulate_opcode(struct pt_regs *regs, unsigned int opcode)
{
	struct thread_info *ti = task_thread_info(current);
	int rd = (opcode & RD) >> 11;
	int rt = (opcode & RT) >> 16;

	/* PR34054: use alternate RDHWR instruction encoding */
	if (((opcode & OPCODE) == BRDHWR && (opcode & FUNC) == RDHWR)
	    || ((opcode & OPCODE) == SPEC3 && (opcode & FUNC) == RDHWR)) {

		if (rd == 29) {
			regs->regs[rt] = ti->tp_value;
			atomic_inc(&brcm_rdhwr_count);
			return 0;
		}
	}

	/* emulate MFC0 $15 for optimized memcpy() CPU detection */
	if ((opcode & OPCODE) == OP_MFC0 &&
	    (opcode & OFFSET) == (15 << 11)) {
		regs->regs[rt] = read_c0_prid();
		return 0;
	}

	return -1;	/* unhandled */
}

int brcm_unaligned_fp(void __user *addr, union mips_instruction *insn,
	struct pt_regs *regs)
{
	unsigned int op = insn->i_format.opcode;
	unsigned int rt = insn->i_format.rt;
	unsigned int res;
	int wordlen = 8;

	/* on r4k, only the even slots ($f0, $f2, ...) are used */
	u8 *fprptr = (u8 *)current + THREAD_FPR0 + (rt >> 1) *
		(THREAD_FPR2 - THREAD_FPR0);

	if (op == lwc1_op || op == swc1_op) {
		wordlen = 4;
#ifdef __LITTLE_ENDIAN
		/* LE: LSW ($f0) precedes MSW ($f1) */
		fprptr += (rt & 1) ? 4 : 0;
#else
		/* BE: MSW ($f1) precedes LSW ($f0) */
		fprptr += (rt & 1) ? 0 : 4;
#endif
	}

	preempt_disable();
	if (is_fpu_owner())
		save_fp(current);
	else
		own_fpu(1);

	if (op == lwc1_op || op == ldc1_op) {
		if (!access_ok(VERIFY_READ, addr, wordlen))
			goto sigbus;
		/*
		 * FPR load: copy from user struct to kernel saved
		 * register struct, then restore all FPRs
		 */
		__asm__ __volatile__ (
		"1:	lb	%0, 0(%3)\n"
		"	sb	%0, 0(%2)\n"
		"	addiu	%2, 1\n"
		"	addiu	%3, 1\n"
		"	addiu	%1, -1\n"
		"	bnez	%1, 1b\n"
		"	li	%0, 0\n"
		"3:\n"
		"	.section .fixup,\"ax\"\n"
		"4:	li	%0, %4\n"
		"	j	3b\n"
		"	.previous\n"
		"	.section __ex_table,\"a\"\n"
		STR(PTR)" 1b,4b\n"
		"	.previous\n"
			: "=&r" (res), "+r" (wordlen),
			  "+r" (fprptr), "+r" (addr)
			: "i" (-EFAULT));
		if (res)
			goto fault;

		restore_fp(current);
	} else {
		if (!access_ok(VERIFY_WRITE, addr, wordlen))
			goto sigbus;
		/*
		 * FPR store: copy from kernel saved register struct
		 * to user struct
		 */
		__asm__ __volatile__ (
		"2:	lb	%0, 0(%2)\n"
		"1:	sb	%0, 0(%3)\n"
		"	addiu	%2, 1\n"
		"	addiu	%3, 1\n"
		"	addiu	%1, -1\n"
		"	bnez	%1, 2b\n"
		"	li	%0, 0\n"
		"3:\n"
		"	.section .fixup,\"ax\"\n"
		"4:	li	%0, %4\n"
		"	j	3b\n"
		"	.previous\n"
		"	.section __ex_table,\"a\"\n"
		STR(PTR)" 1b,4b\n"
		"	.previous\n"
			: "=&r" (res), "+r" (wordlen),
			  "+r" (fprptr), "+r" (addr)
			: "i" (-EFAULT));
		if (res)
			goto fault;
	}
	preempt_enable();

	atomic_inc(&brcm_unaligned_fp_count);
	return 0;

sigbus:
	preempt_enable();
	return -EINVAL;

fault:
	preempt_enable();
	return -EFAULT;
}
