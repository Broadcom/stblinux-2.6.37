/*
 * Copyright (C) 2011 Broadcom Corporation
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

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/compiler.h>

#include <asm/mipsregs.h>
#include <asm/brcmstb/brcmstb.h>

#define MAX_GP_REGS	32
#define MAX_CP0_REGS	32
#define MAX_IO_REGS	(256 - MAX_GP_REGS - MAX_CP0_REGS)

#define DBG(...)	printk(KERN_INFO __VA_ARGS__)

struct cp0_value {
	u32	value;
	u32	index;
	u32	select;
};

struct	brcm_pm_s3_context {
	u32			gp_regs[MAX_GP_REGS];
	struct cp0_value	cp0_regs[MAX_CP0_REGS];
	u32			io_regs[MAX_IO_REGS];
	int			gp_regs_idx;
	int			cp0_regs_idx;
	int			io_regs_idx;
};

struct brcm_pm_s3_context s3_context;
static struct brcm_pm_s3_context s3_context2;

asmlinkage int brcm_pm_s3_standby_asm(unsigned long flags);

extern void brcmstb_enable_xks01(void);

static void brcm_pm_dump_context(struct brcm_pm_s3_context *cxt);

#define CPO_SAVE_INFO(cxt, ci, idx, _select) \
	do { \
		cxt->cp0_regs[ci].index = idx; \
		cxt->cp0_regs[ci].select = _select; \
	} while (0);
#define read_c0_reg(n, s)	__read_32bit_c0_register($##n, s)
#define write_c0_reg(n, s, v)	__write_32bit_c0_register($##n, s, v)

static __maybe_unused void brcm_pm_init_cp0_context(
		struct brcm_pm_s3_context *cxt)
{
	cxt->cp0_regs_idx = 0;
}

static __maybe_unused void brcm_pm_save_cp0_context(
		struct brcm_pm_s3_context *cxt)
{
	int ci = cxt->cp0_regs_idx;

	CPO_SAVE_INFO(cxt, ci, 4, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(4, 0); /* context */
	CPO_SAVE_INFO(cxt, ci, 4, 2);
	cxt->cp0_regs[ci++].value = read_c0_reg(4, 2); /* userlocal */
	CPO_SAVE_INFO(cxt, ci, 5, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(5, 0); /* pagemask */
	CPO_SAVE_INFO(cxt, ci, 6, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(6, 0); /* wired */
	CPO_SAVE_INFO(cxt, ci, 7, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(7, 0); /* hwrena */
	CPO_SAVE_INFO(cxt, ci, 9, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(9, 0); /* count */
	CPO_SAVE_INFO(cxt, ci, 11, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(11, 0); /* compare */
	CPO_SAVE_INFO(cxt, ci, 12, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(12, 0); /* status */
	CPO_SAVE_INFO(cxt, ci, 13, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(13, 0); /* cause */
	CPO_SAVE_INFO(cxt, ci, 14, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(14, 0); /* epc */
	CPO_SAVE_INFO(cxt, ci, 15, 1);
	cxt->cp0_regs[ci++].value = read_c0_reg(15, 1); /* ebase */

	/* Broadcom specific */
	CPO_SAVE_INFO(cxt, ci, 22, 0);
	cxt->cp0_regs[ci++].value = read_c0_reg(22, 0); /* config */
	CPO_SAVE_INFO(cxt, ci, 22, 1);
	cxt->cp0_regs[ci++].value = read_c0_reg(22, 1); /* mode */
	CPO_SAVE_INFO(cxt, ci, 22, 3);
	cxt->cp0_regs[ci++].value = read_c0_reg(22, 3); /* eDSP */
	CPO_SAVE_INFO(cxt, ci, 22, 4);
	cxt->cp0_regs[ci++].value = read_c0_reg(22, 4); /* bootvec */

	cxt->cp0_regs_idx = ci;
}

static __maybe_unused void brcm_pm_restore_cp0_context(
		struct brcm_pm_s3_context *cxt)
{
	int ci = cxt->cp0_regs_idx;

	/* Broadcom specific */
	write_c0_reg(22, 4, cxt->cp0_regs[--ci].value); /* bootvec */
	write_c0_reg(22, 3, cxt->cp0_regs[--ci].value); /* eDSP */
	write_c0_reg(22, 1, cxt->cp0_regs[--ci].value); /* mode */
	write_c0_reg(22, 0, cxt->cp0_regs[--ci].value); /* config */

	write_c0_reg(15, 1, cxt->cp0_regs[--ci].value); /* ebase */
	write_c0_reg(14, 0, cxt->cp0_regs[--ci].value); /* epc */
	write_c0_reg(13, 0, cxt->cp0_regs[--ci].value); /* cause */
	write_c0_reg(12, 0, cxt->cp0_regs[--ci].value); /* status */
	write_c0_reg(11, 0, cxt->cp0_regs[--ci].value); /* compare */
	write_c0_reg(9, 0, cxt->cp0_regs[--ci].value); /* count */
	write_c0_reg(7, 0, cxt->cp0_regs[--ci].value); /* hwrena */
	write_c0_reg(6, 0, cxt->cp0_regs[--ci].value); /* wired */
	write_c0_reg(5, 0, cxt->cp0_regs[--ci].value); /* pagemask */
	write_c0_reg(4, 2, cxt->cp0_regs[--ci].value); /* userlocal */
	write_c0_reg(4, 0, cxt->cp0_regs[--ci].value); /* context */

	cxt->cp0_regs_idx = ci;
}

static __maybe_unused void brcm_pm_cmp_context(
		struct brcm_pm_s3_context *cxt,
		struct brcm_pm_s3_context *cxt2, const char* title)
{
	int i;
	int identical = 1;
	if (title)
		DBG("%s\n", title);
	if (cxt->cp0_regs_idx != cxt2->cp0_regs_idx) {
		identical = 0;
		DBG("CP0 reg # is different: %d %d\n",
			cxt->cp0_regs_idx, cxt2->cp0_regs_idx);
	} else {
		for (i = 0; i < cxt->cp0_regs_idx; i++) {
			if (cxt->cp0_regs[i].value
					!= cxt2->cp0_regs[i].value) {
				identical = 0;
				DBG("\tCP0[%02d.%01d]:  %08x != %08x\n",
					cxt->cp0_regs[i].index,
					cxt->cp0_regs[i].select,
					cxt->cp0_regs[i].value,
					cxt2->cp0_regs[i].value);
			}
		}
	}
	if (identical)
		DBG("Contexts are identical\n");
	else {
		brcm_pm_dump_context(cxt);
		brcm_pm_dump_context(cxt2);
	}
}

static void brcm_pm_dump_context(struct brcm_pm_s3_context *cxt)
{
	int i;
	DBG("GP:\n");
	for (i = 0; i < 32; i++) {
		DBG("[%02d]=%08x ", i, cxt->gp_regs[i]);
		if (i%8 == 7)
			DBG("\n");
	}
	DBG("CPO:\n");
	for (i = 0; i < cxt->cp0_regs_idx; i++) {
		DBG("[%02d.%0d]=%08x ",
			cxt->cp0_regs[i].index,
			cxt->cp0_regs[i].select,
			cxt->cp0_regs[i].value);
		if (i%8 == 7)
			DBG("\n");
	}
	if (cxt->io_regs_idx) {
		DBG("\nI/O:\n");
		for (i = 0; i < cxt->io_regs_idx; i++) {
			DBG("[%02d]=%08x ", i, cxt->io_regs[i]);
			if (i%8 == 7)
				DBG("\n");
		}
	}
	DBG("\n\n");

}

int __ref brcm_pm_s3_standby(int icache_linesize, unsigned long options)
{
	int retval = 0;
	unsigned long flags;

	local_irq_save(flags);
	/* save CP0 context */
	brcm_pm_init_cp0_context(&s3_context);
	brcm_pm_save_cp0_context(&s3_context);

	/* save I/O context */
	/*
	 * reset uart
	 */
	BDEV_WR_RB(BCHP_UARTA_IER, 0);
	BDEV_WR_RB(BCHP_UARTA_FCR, 6);
	/*
	 * reset usb0
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x01000000);
	/*
	 * reset usb1
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x02000000);
	/*
	 * reset genet0
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x04000000);
	/*
	 * reset genet1
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x08000000);
	/*
	 * reset moca
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x10000000);
	/*
	 * reset sata
	 */
	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_SET, 0x20000000);

	BDEV_WR_RB(BCHP_SUN_TOP_CTRL_SW_INIT_0_CLEAR, 0xff000000);

	retval = brcm_pm_s3_standby_asm(flags);

	/* CPU reconfiguration */
	brcmstb_enable_xks01();

	bchip_mips_setup();

	/* brcm_setup_ebase */
	ebase = 0x80001000;
	write_c0_brcm_bootvec(0x80088008);
	write_c0_ebase(ebase);

	/* restore I/O context */
	board_pinmux_setup();

	/* restore CP0 context */
	brcm_pm_init_cp0_context(&s3_context2);
	brcm_pm_save_cp0_context(&s3_context2);
	brcm_pm_cmp_context(&s3_context, &s3_context2,
		"After regular restore:");

	if (options & 0x100) {
		DBG("Restoring identical pre-suspend CP0 context\n");
		brcm_pm_restore_cp0_context(&s3_context);
	}

	bchip_usb_init();
	bchip_moca_init();
	local_irq_restore(flags);
	return retval;
}
