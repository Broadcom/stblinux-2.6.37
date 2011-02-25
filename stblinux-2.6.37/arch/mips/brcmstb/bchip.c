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
#include <linux/module.h>
#include <linux/version.h>

#include <asm/mipsregs.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/r4kcache.h>
#include <asm/asm-offsets.h>
#include <asm/inst.h>
#include <asm/fpu.h>
#include <asm/brcmstb/brcmstb.h>
#include <dma-coherence.h>

/* chip features */
int brcm_sata_enabled;
int brcm_enet_enabled;
int brcm_pci_enabled;
int brcm_pcie_enabled;
int brcm_smp_enabled;
int brcm_emac_1_enabled;
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
#if   defined(CONFIG_BCM3563)
	const unsigned int mc[] = { 0, 16, 32, 64 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr_mem0_config);
	return mc[reg & 3] * ((reg >> 2) ? 2 : 4);
#elif defined(CONFIG_BCM7038)
	const unsigned int mc[] = { 0, 16, 32, 64 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE, strap_ddr_configuration);
	return mc[reg & 3] * ((reg >> 2) ? 2 : 4);
#elif defined(CONFIG_BCM7118)
	const unsigned int mc[] = { 256, 32, 64, 128 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE, strap_ddr_configuration);
	return mc[reg & 3];
#elif defined(CONFIG_BCM7400)
	const unsigned int mc[] = { 64, 128, 256, 512 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr_configuration);
	return mc[reg & 3];
#elif defined(CONFIG_BCM7401) || defined(CONFIG_BCM7403)
	const unsigned int mc[] = { 128, 16, 32, 64 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE, strap_ddr_configuration);
	return mc[reg & 3] * ((reg >> 2) ? 2 : 4);
#elif defined(CONFIG_BCM7405) || defined(CONFIG_BCM7335)
	const unsigned int mc[] = { 32, 64, 128, 256 };
	const unsigned int mode[] = { 4, 2, 1, 0, 2, 1, 0, 0 };
	unsigned int tmp;
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr0_device_config);
	tmp = mc[reg & 3];
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr_configuration);
	return tmp * mode[reg];
#elif defined(CONFIG_BCM7325)
	const unsigned int mc[] = { 32, 64, 128, 256 };
	reg = BDEV_RD_F(SUN_TOP_CTRL_STRAP_VALUE_0, strap_ddr0_device_config);
	return mc[reg] * 2;
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

#if   defined(CONFIG_BCM3548)
	ALT_CHIP_ID(3556, b0);
	MAIN_CHIP_ID(3548, b0);
#elif defined(CONFIG_BCM3563)
	MAIN_CHIP_ID(3563, c0);
#elif defined(CONFIG_BCM35230)
	MAIN_CHIP_ID(35230, a0);
#elif defined(CONFIG_BCM7118)
	ALT_CHIP_ID(7018, c0);
	MAIN_CHIP_ID(7118, c0);
#elif defined(CONFIG_BCM7125)
	ALT_CHIP_ID(7019, c0);
	ALT_CHIP_ID(7025, c0);
	ALT_CHIP_ID(7116, c0);
	ALT_CHIP_ID(7117, c0);
	ALT_CHIP_ID(7119, c0);
	ALT_CHIP_ID(7120, c0);
	MAIN_CHIP_ID(7125, c0);
#elif defined(CONFIG_BCM7231)
	MAIN_CHIP_ID(7231, a0);
#elif defined(CONFIG_BCM7325)
	ALT_CHIP_ID(7324, b0);
	MAIN_CHIP_ID(7325, b0);
#elif defined(CONFIG_BCM7335)
	ALT_CHIP_ID(7336, a0);
	MAIN_CHIP_ID(7335, b0);
#elif defined(CONFIG_BCM7340)
	ALT_CHIP_ID(7350, b0);
	MAIN_CHIP_ID(7340, b0);
#elif defined(CONFIG_BCM7342)
	ALT_CHIP_ID(7352, b0);
	MAIN_CHIP_ID(7342, b0);
#elif defined(CONFIG_BCM7344)
	MAIN_CHIP_ID(7344, a0);
#elif defined(CONFIG_BCM7346)
	MAIN_CHIP_ID(7346, a0);
#elif defined(CONFIG_BCM7400)
	MAIN_CHIP_ID(7400, d0);
#elif defined(CONFIG_BCM7401)
	MAIN_CHIP_ID(7401, c1);			/* for EBI bugfix */
#elif defined(CONFIG_BCM7403)
	MAIN_CHIP_ID(7403, a1);			/* for EBI bugfix */
#elif defined(CONFIG_BCM7405)
	ALT_CHIP_ID(7413, a0);
	MAIN_CHIP_ID(7405, b0);
#elif defined(CONFIG_BCM7408)
	MAIN_CHIP_ID(7408, b0);
#elif defined(CONFIG_BCM7420)
	ALT_CHIP_ID(3320, c0);
	ALT_CHIP_ID(7220, c0);
	ALT_CHIP_ID(7409, c0);
	ALT_CHIP_ID(7410, c0);
	MAIN_CHIP_ID(7420, c0);
#elif defined(CONFIG_BCM7422) || defined(CONFIG_BCM7425)
	ALT_CHIP_ID(7422, a0);
	MAIN_CHIP_ID(7425, a0);
#elif defined(CONFIG_BCM7468)
	MAIN_CHIP_ID(7468, b0);
#elif defined(CONFIG_BCM7550)
	MAIN_CHIP_ID(7550, a0);
#elif defined(CONFIG_BCM7601)
	MAIN_CHIP_ID(7443, b0);
#elif defined(CONFIG_BCM7630)
	MAIN_CHIP_ID(7630, b0);
#elif defined(CONFIG_BCM7631)
	MAIN_CHIP_ID(7631, b0);
#elif defined(CONFIG_BCM7635)
	MAIN_CHIP_ID(7635, a0);
#elif defined(CONFIG_BCM7640)
	MAIN_CHIP_ID(7640, a0);
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

void __init bchip_mips_setup(void)
{
#if   defined(CONFIG_BMIPS3300)

	unsigned long cbr = BMIPS_GET_CBR();

	/* Set BIU to async mode */
	set_c0_brcm_bus_pll(1 << 22);
	__sync();

#ifdef BCHP_MISB_BRIDGE_WG_MODE_N_TIMEOUT
	/* Enable write gathering */
	BDEV_WR_RB(BCHP_MISB_BRIDGE_WG_MODE_N_TIMEOUT, 0x264);

	/* Enable split mode */
	BDEV_WR_RB(BCHP_MISB_BRIDGE_MISB_SPLIT_MODE, 0x1);
	__sync();
#endif

	/* put the BIU back in sync mode */
	clear_c0_brcm_bus_pll(1 << 22);

	/* clear BHTD to enable branch history table */
	clear_c0_brcm_reset(1 << 16);

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
	clear_c0_brcm_config_0(1 << 21);

#elif defined(CONFIG_BMIPS5000)

	/* enable RDHWR, BRDHWR */
	set_c0_brcm_config((1 << 17) | (1 << 21));

#elif defined(CONFIG_MTI_R5K)

	/* Flush and enable RAC */
	BDEV_WR(BCHP_RAC_COMMAND, 0x01);
	while (BDEV_RD(BCHP_RAC_VALID_FWD_STATUS) & 0xffff)
		;
	BDEV_WR_RB(BCHP_RAC_COMMAND, 0x00);

	BDEV_WR_RB(BCHP_RAC_CACHEABLE_SPACE, 0x0fff0000);
	BDEV_WR_RB(BCHP_RAC_MODE, 0xd3);

#endif
}

/***********************************************************************
 * Common operations for all chips
 ***********************************************************************/

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

static void bchip_usb_init_one(uintptr_t base)
{
	/* endianness setup */
	BDEV_UNSET_RB(USB_REG(base, SETUP), USB_ENDIAN_MASK);
	BDEV_SET_RB(USB_REG(base, SETUP), USB_ENDIAN);

	/* power control setup */
#ifdef CONFIG_BRCM_OVERRIDE_USB_PWR
#ifdef CONFIG_BRCM_FORCE_USB_PWR_LO
	BDEV_UNSET(USB_REG(base, SETUP), USB_IPP);
	printk(KERN_INFO "USB: forcing active low VBUS input\n");
#else
	BDEV_SET(USB_REG(base, SETUP), USB_IPP);
	printk(KERN_INFO "USB: forcing active high VBUS input\n");
#endif
#else /* CONFIG_BRCM_OVERRIDE_USB_PWR */
	if (!(BDEV_RD(USB_REG(base, SETUP)) & USB_IOC)) {
		printk(KERN_WARNING "warning: USB IOC not initialized by "
			"bootloader, assuming active low\n");
		BDEV_SET(USB_REG(base, SETUP), USB_IOC);
		BDEV_UNSET(USB_REG(base, SETUP), USB_IPP);
	}
#endif /* CONFIG_BRCM_OVERRIDE_USB_PWR */

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

void __init bchip_usb_init(void)
{
	bchip_usb_init_one(BCHP_USB_CTRL_REG_START);
#ifdef BCHP_USB1_CTRL_REG_START
	bchip_usb_init_one(BCHP_USB1_CTRL_REG_START);
#endif
}

#if defined(CONFIG_BRCM_HAS_MOCA)
void __init bchip_moca_init(void)
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
#elif defined(CONFIG_BCM7342) || defined(CONFIG_BCM7408) || \
	defined(CONFIG_BCM7420)
	BDEV_WR_F_RB(CLK_MISC, MOCA_ENET_GMII_TX_CLK_SEL, 0);
#elif defined(CONFIG_BCM7422A0)
	BDEV_WR_F(CLKGEN_PLL_NETWORK_PLL_CHANNEL_CTRL_CH_1, MDIV_CH1, 90);
#endif
}
#endif

#if defined(CONFIG_BRCM_HAS_SDIO)
int __init bchip_sdio_init(int id, uintptr_t cfg_base)
{
#ifdef BCHP_HIF_TOP_CTRL_SDIO_CTRL
	/* HIF_TOP_CTRL_SDIO_CTRL only exists for V0 controllers */
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, WORD_ABO, 0);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, FRAME_NBO, 0);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, FRAME_NHW, 1);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, BUFFER_ABO, 1);
#else
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, WORD_ABO, 1);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, FRAME_NBO, 1);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, FRAME_NHW, 1);
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, BUFFER_ABO, 0);
#endif
	BDEV_WR_F_RB(HIF_TOP_CTRL_SDIO_CTRL, SCB_SEQ_EN, 0);
#endif /* BCHP_HIF_TOP_CTRL_SDIO_CTRL */

#ifdef CONFIG_BRCM_HAS_SDIO_V1

#define SDIO_REG(x, y)		(x + BCHP_SDIO_0_CFG_##y - \
				 BCHP_SDIO_0_CFG_REG_START)

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
	/* address + data swap on byte/halfword accesses */
	BDEV_SET(SDIO_REG(cfg_base, SDIO_EMMC_CTRL2), 0x005f);
#endif
#endif /* CONFIG_BRCM_HAS_SDIO_V1 */

#if defined(CONFIG_BCM7422A0) || defined(CONFIG_BCM7425A0) || \
	defined(CONFIG_BCM7231A0)

	/* Disable SDHCI capabilities that are broken in A0 silicon */
	BDEV_UNSET(SDIO_REG(cfg_base, CAP_REG0), 1 << 18);	/* ADMA=0 */
	BDEV_UNSET(SDIO_REG(cfg_base, CAP_REG0), 1 << 24);	/* 1.8V=0 */
	BDEV_UNSET(SDIO_REG(cfg_base, CAP_REG1), 1 << 7);	/* Tuning=0 */
	BDEV_SET(SDIO_REG(cfg_base, CAP_REG1), 1 << 31);	/* Override=1 */

	/* Use better defaults for timing */
	BDEV_WR(SDIO_REG(cfg_base, IP_DLY), 0);
	BDEV_WR(SDIO_REG(cfg_base, OP_DLY), 0);
#endif

	return 0;
}
#endif

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
	brcm_emac_1_enabled = 1;
#endif
#if defined(CONFIG_BRCM_HAS_MOCA)
	brcm_moca_enabled = 1;
#endif
#if defined(CONFIG_BRCM_PM)
	brcm_pm_enabled = 1;
#endif
	brcm_usb_enabled = 1;

	/* now remove any features disabled in hardware */

#ifdef CONFIG_BCM7118
	/* 7118RNG - no SATA */
	if (BDEV_RD(BCHP_CLKGEN_REG_START) & 0x1c)
		brcm_sata_enabled = 0;
#endif

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
		brcm_emac_1_enabled = 0;
		break;
	case 0x3:
		/* 7106 */
		brcm_emac_1_enabled = 0;
		brcm_smp_enabled = 0;
		break;
	case 0x4:
	case 0x6:
		/* 7205/7213 */
		brcm_emac_1_enabled = 0;
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
	defined(CONFIG_BCM7342) || defined(CONFIG_BCM7420)

	switch (BRCM_CHIP_ID()) {
	case 0x7340:
	case 0x7342:
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
			brcm_emac_1_enabled = 0;
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
