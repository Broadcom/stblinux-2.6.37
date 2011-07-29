/*
 * Copyright (C) 2010 Broadcom Corporation
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

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>
#include <linux/ioport.h>
#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#include <asm/addrspace.h>
#include <asm/brcmstb/brcmstb.h>

/*
 * SWLINUX-1818: this flag controls if WP stays on between erase/write
 * commands to mitigate flash corruption due to power glitches. Values:
 * 0: NAND_WP is not used or not available
 * 1: NAND_WP is set by default, cleared for erase/write operations
 * 2: NAND_WP is always cleared
 */
static int wp_on = 1;
module_param(wp_on, int, 0444);

/***********************************************************************
 * Definitions
 ***********************************************************************/

#define DBG(args...)		DEBUG(MTD_DEBUG_LEVEL3, args)

#define DRV_NAME		"brcmstb_nand"
#define CONTROLLER_VER		(10 * CONFIG_BRCMNAND_MAJOR_VERS + \
		CONFIG_BRCMNAND_MINOR_VERS)

#define CMD_PAGE_READ		0x01
#define CMD_SPARE_AREA_READ	0x02
#define CMD_STATUS_READ		0x03
#define CMD_PROGRAM_PAGE	0x04
#define CMD_PROGRAM_SPARE_AREA	0x05
#define CMD_COPY_BACK		0x06
#define CMD_DEVICE_ID_READ	0x07
#define CMD_BLOCK_ERASE		0x08
#define CMD_FLASH_RESET		0x09
#define CMD_BLOCKS_LOCK		0x0a
#define CMD_BLOCKS_LOCK_DOWN	0x0b
#define CMD_BLOCKS_UNLOCK	0x0c
#define CMD_READ_BLOCKS_LOCK_STATUS	0x0d

#if CONTROLLER_VER >= 40
#define CMD_PARAMETER_READ	0x0e
#define CMD_PARAMETER_CHANGE_COL	0x0f
#define CMD_LOW_LEVEL_OP	0x10
#endif

/* 512B flash cache in the NAND controller HW */
#define FC_SHIFT		9U
#define FC_BYTES		512U
#define FC_WORDS		(FC_BYTES >> 2)
#define FC(x)			(BCHP_NAND_FLASH_CACHEi_ARRAY_BASE + ((x) << 2))

#if CONTROLLER_VER >= 50
#define MAX_CONTROLLER_OOB	32
#define OFS_10_RD		BCHP_NAND_SPARE_AREA_READ_OFS_10
#define OFS_10_WR		BCHP_NAND_SPARE_AREA_WRITE_OFS_10
#else
#define MAX_CONTROLLER_OOB	16
#define OFS_10_RD		-1
#define OFS_10_WR		-1
#endif

#define EDU_CMD_WRITE		0x00
#define EDU_CMD_READ		0x01

#ifdef CONFIG_BRCM_HAS_EDU
#define EDU_VA_OK(x)		(!is_vmalloc_addr((const void *)(x)))
#else
#define EDU_VA_OK(x)		0
#endif

#define REG_ACC_CONTROL(cs) \
	((cs) == 0 ? BCHP_NAND_ACC_CONTROL : \
	 (BCHP_NAND_ACC_CONTROL_CS1 + (((cs) - 1) << 4)))

#define REG_CONFIG(cs) \
	((cs) == 0 ? BCHP_NAND_CONFIG : \
	 (BCHP_NAND_CONFIG_CS1 + (((cs) - 1) << 4)))

#define WR_CONFIG(cs, field, val) do { \
	u32 reg = REG_CONFIG(cs), contents = BDEV_RD(reg); \
	contents &= ~(BCHP_NAND_CONFIG_##field##_MASK); \
	contents |= (val) << BCHP_NAND_CONFIG_##field##_SHIFT; \
	BDEV_WR(reg, contents); \
	} while (0)

#define RD_CONFIG(cs, field) \
	((BDEV_RD(REG_CONFIG(cs)) & BCHP_NAND_CONFIG_##field##_MASK) \
	 >> BCHP_NAND_CONFIG_##field##_SHIFT)

#define WR_ACC_CONTROL(cs, field, val) do { \
	u32 reg = REG_ACC_CONTROL(cs), contents = BDEV_RD(reg); \
	contents &= ~(BCHP_NAND_ACC_CONTROL_##field##_MASK); \
	contents |= (val) << BCHP_NAND_ACC_CONTROL_##field##_SHIFT; \
	BDEV_WR(reg, contents); \
	} while (0)

#define RD_ACC_CONTROL(cs, field) \
	((BDEV_RD(REG_ACC_CONTROL(cs)) & BCHP_NAND_ACC_CONTROL_##field##_MASK) \
		>> BCHP_NAND_ACC_CONTROL_##field##_SHIFT)

/* Helper functions for reading and writing OOB registers */
static inline unsigned char oob_reg_read(int offs)
{
	if (offs >= MAX_CONTROLLER_OOB)
		return 0x77;

	if (offs < 16)
		return BDEV_RD(BCHP_NAND_SPARE_AREA_READ_OFS_0 + (offs & ~0x03))
			>> (24 - ((offs & 0x03) << 3));

	offs -= 16;

	return BDEV_RD(OFS_10_RD + (offs & ~0x03))
		>> (24 - ((offs & 0x03) << 3));
}

static inline void oob_reg_write(int offs, unsigned long data)
{
	if (offs >= MAX_CONTROLLER_OOB)
		return;

	if (offs < 16) {
		BDEV_WR(BCHP_NAND_SPARE_AREA_WRITE_OFS_0 + (offs & ~0x03),
				data);
		return;
	}

	offs -= 16;

	BDEV_WR(OFS_10_WR + (offs & ~0x03), data);
}

struct brcmstb_nand_controller {
	struct nand_hw_control	controller;
	unsigned int		irq;
	int			cmd_pending;
	struct completion	done;

	int			edu_count;
	u64			edu_dram_addr;
	u32			edu_ext_addr;
	u32			edu_cmd;
};

static struct brcmstb_nand_controller ctrl;

struct brcmstb_nand_cfg {
	u64			device_size;
	unsigned int		block_size;
	unsigned int		page_size;
	unsigned int		spare_area_size;
	unsigned int		device_width;
	unsigned int		col_adr_bytes;
	unsigned int		blk_adr_bytes;
	unsigned int		ful_adr_bytes;
	unsigned int		sector_size_1k;
};

struct brcmstb_nand_host {
	u32			buf[FC_WORDS];
	struct nand_chip	chip;
	struct mtd_info		mtd;
	struct platform_device	*pdev;
	int			cs;

	unsigned int		last_cmd;
	unsigned int		last_byte;
	u64			last_addr;
	struct brcmstb_nand_cfg	hwcfg;
};

static struct nand_ecclayout brcmstb_nand_oob_layout = {
	.eccbytes		= 16,
	.eccpos			= { 0, 1, 2, 3, 4, 5, 6, 7,
				    8, 9, 10, 11, 12, 13, 14, 15 },
};

struct brcmstb_nand_exception {
	const char		*name;
	int			id[7];
	int			idlen; /* usable */
	unsigned int		chipsize; /* MB */
	unsigned int		writesize; /* B */
	unsigned int		erasesize; /* B */
	unsigned int		oobsize; /* B per page */
	int			chipoptions;
	int			badblockpos;
};

static struct brcmstb_nand_exception brcmstb_exceptions_list[] = {
	{"Micron MT29F16G08ABABA",
		{0x2C, 0x48, 0x00, 0x26, 0x89, 0x00, 0x00},
		5, 0x00800, 4096, 0x080000, 224},
	{"Micron MT29F16G08CBABA",
		{0x2C, 0x48, 0x04, 0x46, 0x85, 0x00, 0x00},
		5, 0x00800, 4096, 0x100000, 224},
	{"Micron MT29F16G08MAA",
		{0x2C, 0xD5, 0x94, 0x3E, 0x74, 0x00, 0x00},
		5, 0x00800, 4096, 0x080000, 218},
	{"Micron MT29F32G08CBACA",
		{0x2C, 0x68, 0x04, 0x4A, 0xA9, 0x00, 0x00},
		5, 0x01000, 4096, 0x100000, 224},
	{"Micron MT29F64G08CBAAA",
		{0x2C, 0x88, 0x04, 0x4B, 0xA9, 0x00, 0x00},
		5, 0x02000, 8192, 0x200000, 448},
	{"Micron MT29F256G08CJAAA",
		{0x2C, 0xA8, 0x05, 0xCB, 0xA9, 0x00, 0x00},
		5, 0x08000, 8192, 0x200000, 448},
	{NULL,}
};

/* Used for running nand_scan_ident without the built-in heuristics */
static struct nand_flash_dev brcmstb_empty_flash_table[] = {
	{NULL,}
};

static void brcmstb_erase_cmd(struct mtd_info *mtd, int page)
{
	struct nand_chip *chip = mtd->priv;
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page);
}

static void brcmstb_nand_wp(struct mtd_info *mtd, int wp)
{
#ifdef BCHP_NAND_CS_NAND_SELECT_NAND_WP_MASK
	if (wp_on == 1) {
		static int old_wp = -1;
		if (old_wp != wp) {
			DBG("%s: WP %s\n", __func__, wp ? "on" : "off");
			old_wp = wp;
		}
		BDEV_WR_F_RB(NAND_CS_NAND_SELECT, NAND_WP, wp);
	}
#endif
}

static int brcmstb_check_exceptions(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct brcmstb_nand_exception *list = brcmstb_exceptions_list;
	int i;
	u8 id_data[8];

	/* run default nand_base initialization w/o built-in ID table;
	 * should return error, so we tell it to be "silent"
	 */
	chip->options |= NAND_SCAN_SILENT_NODEV;
	nand_scan_ident(mtd, 1, brcmstb_empty_flash_table);
	chip->options &= ~NAND_SCAN_SILENT_NODEV;

	/* Send the command for reading device ID */
	chip->cmdfunc(mtd, NAND_CMD_READID, 0x00, -1);

	for (i = 0; i < 8; i++)
		id_data[i] = chip->read_byte(mtd);

	for (; list->name != NULL; list++) {
		for (i = 0; i < list->idlen; i++)
			if (id_data[i] != list->id[i])
				break;
		if (i == list->idlen)
			break;
	}

	if (!list->name)
		return -ENODEV;

	chip->chipsize = (uint64_t)list->chipsize << 20;
	mtd->size = chip->chipsize;

	mtd->erasesize = list->erasesize;
	mtd->writesize = list->writesize;
	mtd->oobsize = list->oobsize;

	chip->options |= list->chipoptions;
	chip->badblockpos = list->badblockpos;

	/* The 3rd id byte holds MLC / multichip data */
	chip->cellinfo = id_data[2];

	chip->numchips = 1;

	/* Calculate the address shift from the page size */
	chip->page_shift = ffs(mtd->writesize) - 1;
	/* Convert chipsize to number of pages per chip -1. */
	chip->pagemask = (chip->chipsize >> chip->page_shift) - 1;

	chip->bbt_erase_shift = chip->phys_erase_shift =
		ffs(mtd->erasesize) - 1;
	chip->chip_shift = fls64(chip->chipsize) - 1;

	chip->erase_cmd = brcmstb_erase_cmd;

	printk(KERN_INFO "%s: heuristics exception detected, %s\n",
		mtd->name, list->name);
	return 0;
}

/***********************************************************************
 * Internal support functions
 ***********************************************************************/

static irqreturn_t brcmstb_nand_irq(int irq, void *data)
{
	int mine = 0;

	if (HIF_TEST_IRQ(NAND_CTLRDY)) {
		HIF_ACK_IRQ(NAND_CTLRDY);
		mine = 1;
	}
	if (mine) {
#ifdef CONFIG_BRCM_HAS_EDU
		if (ctrl.edu_count) {
			ctrl.edu_dram_addr += FC_BYTES;
			ctrl.edu_ext_addr += FC_BYTES;
			ctrl.edu_count--;

			BDEV_WR_RB(BCHP_EDU_DRAM_ADDR, (u32)ctrl.edu_dram_addr);
			BDEV_WR_RB(BCHP_EDU_EXT_ADDR, ctrl.edu_ext_addr);
			mb();
			BDEV_WR_RB(BCHP_EDU_CMD, ctrl.edu_cmd);

			return IRQ_HANDLED;
		}
#endif
		complete(&ctrl.done);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static void brcmstb_nand_send_cmd(int cmd)
{
	DBG("%s: native cmd %d addr_lo 0x%lx\n", __func__, cmd,
		BDEV_RD(BCHP_NAND_CMD_ADDRESS));
	BUG_ON(ctrl.cmd_pending != 0);
	ctrl.cmd_pending = cmd;
	mb();
	BDEV_WR(BCHP_NAND_CMD_START, cmd << 24);
}

/***********************************************************************
 * NAND MTD API: read/program/erase
 ***********************************************************************/

static void brcmstb_nand_cmd_ctrl(struct mtd_info *mtd, int dat,
	unsigned int ctrl)
{
	/* intentionally left blank */
}

static int brcmstb_nand_waitfunc(struct mtd_info *mtd, struct nand_chip *this)
{
	struct nand_chip *chip = mtd->priv;
	struct brcmstb_nand_host *host = chip->priv;

	DBG("%s: native cmd %d\n", __func__, ctrl.cmd_pending);
	if (ctrl.cmd_pending &&
			wait_for_completion_timeout(&ctrl.done, HZ / 10) <= 0) {
		dev_err(&host->pdev->dev,
			"timeout waiting for command %u (%ld)\n",
			host->last_cmd, BDEV_RD(BCHP_NAND_CMD_START) >> 24);
		dev_err(&host->pdev->dev,
			"irq status %08lx, intfc status %08lx\n",
			BDEV_RD(BCHP_HIF_INTR2_CPU_STATUS),
			BDEV_RD(BCHP_NAND_INTFC_STATUS));
	}
	ctrl.cmd_pending = 0;
	brcmstb_nand_wp(mtd, 1);
	return BDEV_RD_F(NAND_INTFC_STATUS, FLASH_STATUS);
}

static void brcmstb_nand_cmdfunc(struct mtd_info *mtd, unsigned command,
	int column, int page_addr)
{
	struct nand_chip *chip = mtd->priv;
	struct brcmstb_nand_host *host = chip->priv;
	u64 addr = (u64)page_addr << chip->page_shift;
	int native_cmd = 0;

	if (command == NAND_CMD_READID || command == NAND_CMD_PARAM)
		addr = (u64)column;

	DBG("%s: cmd 0x%x addr 0x%llx\n", __func__, command,
		(unsigned long long)addr);
	host->last_cmd = command;
	host->last_byte = 0;
	host->last_addr = addr;

	switch (command) {
	case NAND_CMD_RESET:
		native_cmd = CMD_FLASH_RESET;
		break;
	case NAND_CMD_STATUS:
		native_cmd = CMD_STATUS_READ;
		break;
	case NAND_CMD_READID:
		native_cmd = CMD_DEVICE_ID_READ;
		break;
	case NAND_CMD_READOOB:
		native_cmd = CMD_SPARE_AREA_READ;
		break;
	case NAND_CMD_ERASE1:
		native_cmd = CMD_BLOCK_ERASE;
		brcmstb_nand_wp(mtd, 0);
		break;
#if CONTROLLER_VER >= 40
	case NAND_CMD_PARAM:
		native_cmd = CMD_PARAMETER_READ;
		break;
#endif
	}

	if (!native_cmd)
		return;

	BDEV_WR_RB(BCHP_NAND_CMD_EXT_ADDRESS,
		(host->cs << 16) | ((addr >> 32) & 0xffff));
	BDEV_WR_RB(BCHP_NAND_CMD_ADDRESS, addr & 0xffffffff);

	brcmstb_nand_send_cmd(native_cmd);
	brcmstb_nand_waitfunc(mtd, chip);
}

static uint8_t brcmstb_nand_read_byte(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct brcmstb_nand_host *host = chip->priv;
	uint8_t ret = 0;

	switch (host->last_cmd) {
	case NAND_CMD_READID:
		if (host->last_byte < 4)
			ret = BDEV_RD(BCHP_NAND_FLASH_DEVICE_ID) >>
				(24 - (host->last_byte << 3));
		else if (host->last_byte < 8)
			ret = BDEV_RD(BCHP_NAND_FLASH_DEVICE_ID_EXT) >>
				(56 - (host->last_byte << 3));
		break;

	case NAND_CMD_READOOB:
		ret = oob_reg_read(host->last_byte);
		break;

	case NAND_CMD_STATUS:
		ret = BDEV_RD(BCHP_NAND_INTFC_STATUS) & 0xff;
		if (wp_on) /* SWLINUX-1818: hide WP status from MTD */
			ret |= NAND_STATUS_WP;
		break;

#if CONTROLLER_VER >= 40
	case NAND_CMD_PARAM:
		if (host->last_byte < FC_BYTES)
			ret = BDEV_RD(FC(host->last_byte >> 2)) >>
				(24 - ((host->last_byte & 0x03) << 3));
		break;
#endif
	}

	DBG("%s: byte = 0x%02x\n", __func__, ret);
	host->last_byte++;

	return ret;
}

static void brcmstb_nand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	int i;

	for (i = 0; i < len; i++, buf++)
		*buf = brcmstb_nand_read_byte(mtd);
}

static int brcmstb_nand_edu_trans(struct brcmstb_nand_host *host, u64 addr,
	u32 *buf, unsigned int trans, u32 edu_cmd)
{
	int ret = 0;

#ifdef CONFIG_BRCM_HAS_EDU
	int dir = edu_cmd == EDU_CMD_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	unsigned int len = trans * FC_BYTES;
	dma_addr_t pa = dma_map_single(&host->pdev->dev, buf, len, dir);
	struct mtd_info *mtd = &host->mtd;
	struct nand_chip *chip = &host->chip;

	ctrl.edu_dram_addr = pa;
	ctrl.edu_ext_addr = addr;
	ctrl.edu_cmd = edu_cmd;
	ctrl.edu_count = trans - 1;

	brcmstb_nand_wp(mtd, edu_cmd == EDU_CMD_READ);

	BDEV_WR_RB(BCHP_EDU_DRAM_ADDR, (u32)ctrl.edu_dram_addr);
	BDEV_WR_RB(BCHP_EDU_EXT_ADDR, ctrl.edu_ext_addr);
	BDEV_WR_RB(BCHP_EDU_LENGTH, FC_BYTES);

	ctrl.cmd_pending = (edu_cmd == EDU_CMD_READ) ?
		CMD_PAGE_READ : CMD_PROGRAM_PAGE;
	mb();
	BDEV_WR_RB(BCHP_EDU_CMD, ctrl.edu_cmd);

	/* wait for completion, then (for program page) check NAND status */

	if ((brcmstb_nand_waitfunc(mtd, chip) & NAND_STATUS_FAIL) &&
			edu_cmd == EDU_CMD_WRITE) {
		dev_info(&host->pdev->dev, "program failed at %llx\n",
			(unsigned long long)addr);
		ret = -EIO;
	}

	dma_unmap_single(&host->pdev->dev, pa, len, dir);

	/* Make sure the EDU status is clean */

	if (BDEV_RD_F(EDU_STATUS, Active))
		dev_warn(&host->pdev->dev, "EDU still active: %08lx\n",
			BDEV_RD(BCHP_EDU_STATUS));

	if (unlikely(BDEV_RD_F(EDU_ERR_STATUS, ErrAck))) {
		dev_warn(&host->pdev->dev, "EDU RBUS error at addr %llx\n",
			(unsigned long long)addr);
		ret = -EIO;
	}

	BDEV_WR(BCHP_EDU_ERR_STATUS, 0);
#endif /* CONFIG_BRCM_HAS_EDU */

	return ret;
}

/*
 * Assumes proper CS is already set
 */
static void brcmstb_nand_read_by_pio(struct mtd_info *mtd,
	struct nand_chip *chip, u64 addr, unsigned int trans,
	u32 *buf, u8 *oob)
{
	struct brcmstb_nand_host *host = chip->priv;
	unsigned int i, j;

	for (i = 0; i < trans; i++, addr += FC_BYTES) {
		BDEV_WR_RB(BCHP_NAND_CMD_ADDRESS, addr & 0xffffffff);
		brcmstb_nand_send_cmd(
			buf ? CMD_PAGE_READ : CMD_SPARE_AREA_READ);
		brcmstb_nand_waitfunc(mtd, chip);

		if (likely(buf))
			for (j = 0; j < FC_WORDS; j++, buf++)
				*buf = le32_to_cpu(BDEV_RD(FC(j)));

		if (unlikely(oob)) {
			int tbytes = mtd->oobsize / trans;
			int rbytes = min(tbytes, MAX_CONTROLLER_OOB);

			/* Adjust OOB read values for 1K sector size */
			if (host->hwcfg.sector_size_1k) {
				tbytes <<= 1;
				if (i & 0x01)
					tbytes = max(0, tbytes -
							MAX_CONTROLLER_OOB);
				else
					tbytes = min(tbytes,
							MAX_CONTROLLER_OOB);
				rbytes = min(tbytes, MAX_CONTROLLER_OOB);
			}

			for (j = 0; j < rbytes; j++)
				oob[j] = oob_reg_read(j);
			oob += tbytes;
		}
	}
}

static int brcmstb_nand_read(struct mtd_info *mtd,
	struct nand_chip *chip, u64 addr, unsigned int trans,
	u32 *buf, u8 *oob)
{
	struct brcmstb_nand_host *host = chip->priv;
	u64 err_addr;
	bool use_edu;

	DBG("%s %llx -> %p\n", __func__, (unsigned long long)addr, buf);

	BDEV_WR_RB(BCHP_NAND_ECC_UNC_ADDR, 0);
	BDEV_WR_RB(BCHP_NAND_ECC_CORR_ADDR, 0);
	BDEV_WR_RB(BCHP_NAND_CMD_EXT_ADDRESS,
		(host->cs << 16) | ((addr >> 32) & 0xffff));

	if (unlikely(oob))
		memset(oob, 0x99, mtd->oobsize);

	/* Don't use EDU if buffer is not 32-bit aligned */
	use_edu = buf && !oob && EDU_VA_OK(buf) && likely(!((u32)buf & 0x03));

	if (use_edu) {
		if (brcmstb_nand_edu_trans(host, addr, buf, trans,
				EDU_CMD_READ))
			return -EIO;
	} else {
		brcmstb_nand_read_by_pio(mtd, chip, addr, trans, buf, oob);
	}

	err_addr = BDEV_RD(BCHP_NAND_ECC_UNC_ADDR) |
		((u64)(BDEV_RD(BCHP_NAND_ECC_UNC_EXT_ADDR) & 0xffff) << 32);
	if (err_addr != 0) {
		dev_warn(&host->pdev->dev, "uncorrectable error at 0x%llx\n",
			(unsigned long long)err_addr);
		mtd->ecc_stats.failed++;
		if (use_edu)
			brcmstb_nand_read_by_pio(mtd, chip, addr, trans, buf,
					oob);
		return -EIO;
	}

	err_addr = BDEV_RD(BCHP_NAND_ECC_CORR_ADDR) |
		((u64)(BDEV_RD(BCHP_NAND_ECC_CORR_EXT_ADDR) & 0xffff) << 32);
	if (err_addr) {
		dev_info(&host->pdev->dev, "corrected error at 0x%llx\n",
			(unsigned long long)err_addr);
		mtd->ecc_stats.corrected++;
		if (use_edu)
			brcmstb_nand_read_by_pio(mtd, chip, addr, trans, buf,
					oob);
		return -EUCLEAN;
	}

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
static int brcmstb_nand_read_page(struct mtd_info *mtd,
	struct nand_chip *chip, uint8_t *buf, int page)
#else
static int brcmstb_nand_read_page(struct mtd_info *mtd,
	struct nand_chip *chip, uint8_t *buf)
#endif
{
	struct brcmstb_nand_host *host = chip->priv;

	brcmstb_nand_read(mtd, chip, host->last_addr,
		mtd->writesize >> FC_SHIFT, (u32 *)buf, NULL);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
static int brcmstb_nand_read_page_raw(struct mtd_info *mtd,
	struct nand_chip *chip, uint8_t *buf, int page)
#else
static int brcmstb_nand_read_page_raw(struct mtd_info *mtd,
	struct nand_chip *chip, uint8_t *buf)
#endif
{
	struct brcmstb_nand_host *host = chip->priv;

	WR_ACC_CONTROL(host->cs, RD_ECC_EN, 0);
	brcmstb_nand_read(mtd, chip, host->last_addr,
		mtd->writesize >> FC_SHIFT,
		(u32 *)buf, (u8 *)(chip->oob_poi));
	WR_ACC_CONTROL(host->cs, RD_ECC_EN, 1);
	return 0;
}

static int brcmstb_nand_read_oob(struct mtd_info *mtd,
	struct nand_chip *chip, int page, int sndcmd)
{
	brcmstb_nand_read(mtd, chip, page << chip->page_shift,
		mtd->writesize >> FC_SHIFT,
		NULL, (u8 *)chip->oob_poi);
	return 0;
}

static int brcmstb_nand_read_subpage(struct mtd_info *mtd,
	struct nand_chip *chip, uint32_t data_offs, uint32_t readlen,
	uint8_t *bufpoi)
{
	struct brcmstb_nand_host *host = chip->priv;

	brcmstb_nand_read(mtd, chip, host->last_addr + data_offs,
		readlen >> FC_SHIFT, (u32 *)bufpoi, NULL);
	return 0;
}

static int brcmstb_nand_verify_buf(struct mtd_info *mtd, const uint8_t *buf,
	int len)
{
	struct nand_chip *chip = mtd->priv;
	struct brcmstb_nand_host *host = chip->priv;
	unsigned int i, j;
	int err;
	u64 addr = host->last_addr;
	u32 *src = (u32 *)buf, *dst;

	for (i = 0; i < (mtd->writesize >> FC_SHIFT); i++) {
		err = brcmstb_nand_read(mtd, chip, addr, 1, host->buf, NULL);

		if (err) {
			dev_info(&host->pdev->dev,
				"verify failed at 0x%llx (error %d)\n",
				(unsigned long long)addr, err);
			return -EFAULT;
		}
		dst = host->buf;

		for (j = 0; j < FC_WORDS; j++, src++, dst++)
			if (*src != le32_to_cpu(*dst)) {
				dev_info(&host->pdev->dev,
					"mismatch at 0x%llx (read %08lx, "
					"expected %08lx)\n",
					(unsigned long long)addr + (j << 2),
					(unsigned long)*dst,
					(unsigned long)*src);
				return -EFAULT;
			}
		addr += FC_BYTES;
	}
	return 0;
}

static int brcmstb_nand_write(struct mtd_info *mtd,
	struct nand_chip *chip, u64 addr, const u32 *buf, const u8 *oob)
{
	struct brcmstb_nand_host *host = chip->priv;
	unsigned int i = 0, j, trans = mtd->writesize >> FC_SHIFT;
	int status;

	DBG("%s %llx <- %p\n", __func__, (unsigned long long)addr, buf);

	if (unlikely((u32)buf & 0x03)) {
		dev_warn(&host->pdev->dev, "unaligned buffer: %p\n", buf);
		buf = (u32 *)((u32)buf & ~0x03);
	}

	BDEV_WR_RB(BCHP_NAND_CMD_EXT_ADDRESS,
		(host->cs << 16) | ((addr >> 32) & 0xffff));

	for (j = 0; j < MAX_CONTROLLER_OOB; j += 4)
		oob_reg_write(j, 0xffffffff);

	if (buf && !oob && EDU_VA_OK(buf)) {
		if (brcmstb_nand_edu_trans(host, addr, (u32 *)buf, trans,
				EDU_CMD_WRITE))
			return -EIO;
		i = trans;
	}

	for (; i < trans; i++, addr += FC_BYTES) {
		/* full address MUST be set before populating FC */
		BDEV_WR_RB(BCHP_NAND_CMD_ADDRESS, addr & 0xffffffff);

		if (buf)
			for (j = 0; j < FC_WORDS; j++, buf++)
				BDEV_WR(FC(j), cpu_to_le32(*buf));
		else if (oob)
			for (j = 0; j < FC_WORDS; j++)
				BDEV_WR(FC(j), 0xffffffff);

		if (unlikely(oob)) {
			int tbytes = mtd->oobsize / trans;
			int rwords = min(tbytes, MAX_CONTROLLER_OOB) >> 2;

			/* Adjust OOB read values for 1K sector size */
			if (host->hwcfg.sector_size_1k) {
				tbytes <<= 1;
				if (i & 0x01)
					tbytes = max(0, tbytes -
							MAX_CONTROLLER_OOB);
				else
					tbytes = min(tbytes,
							MAX_CONTROLLER_OOB);
				rwords = tbytes >> 2;
			}

			for (j = 0; j < rwords; j++)
				oob_reg_write(j << 2,
					(oob[j * 4 + 0] << 24) |
					(oob[j * 4 + 1] << 16) |
					(oob[j * 4 + 2] <<  8) |
					(oob[j * 4 + 3] <<  0));
			oob += tbytes;
		}

		brcmstb_nand_wp(mtd, 0);

		/* we cannot use SPARE_AREA_PROGRAM when PARTIAL_PAGE_EN=0 */
		brcmstb_nand_send_cmd(CMD_PROGRAM_PAGE);
		status = brcmstb_nand_waitfunc(mtd, chip);

		if (status & NAND_STATUS_FAIL) {
			dev_info(&host->pdev->dev, "program failed at %llx\n",
				(unsigned long long)addr);
			return -EIO;
		}
	}
	return 0;
}

static void brcmstb_nand_write_page(struct mtd_info *mtd,
	struct nand_chip *chip, const uint8_t *buf)
{
	struct brcmstb_nand_host *host = chip->priv;

	brcmstb_nand_write(mtd, chip, host->last_addr, (u32 *)buf, NULL);
}

static void brcmstb_nand_write_page_raw(struct mtd_info *mtd,
	struct nand_chip *chip, const uint8_t *buf)
{
	struct brcmstb_nand_host *host = chip->priv;

	WR_ACC_CONTROL(host->cs, WR_ECC_EN, 0);
	brcmstb_nand_write(mtd, chip, host->last_addr, (u32 *)buf,
		(u8 *)(chip->oob_poi));
	WR_ACC_CONTROL(host->cs, WR_ECC_EN, 1);
}

static int brcmstb_nand_write_oob(struct mtd_info *mtd,
	struct nand_chip *chip, int page)
{
	return brcmstb_nand_write(mtd, chip, page << chip->page_shift, NULL,
		(u8 *)chip->oob_poi);
}

/***********************************************************************
 * Per-CS setup (1 NAND device)
 ***********************************************************************/

#if CONTROLLER_VER >= 40
static const unsigned int block_sizes[] = { 16, 128, 8, 512, 256, 1024, 2048 };
static const unsigned int page_sizes[] = { 512, 2048, 4096, 8192 };
#else
static const unsigned int block_sizes[] = { 16, 128, 8, 512, 256 };
static const unsigned int page_sizes[] = { 512, 2048, 4096 };
#endif

static void brcmstb_nand_set_cfg(struct brcmstb_nand_host *host,
	struct brcmstb_nand_cfg *cfg)
{
	int i, found;

	for (i = 0, found = 0; i < ARRAY_SIZE(block_sizes); i++)
		if ((block_sizes[i] << 10) == cfg->block_size) {
			WR_CONFIG(host->cs, BLOCK_SIZE, i);
			found = 1;
		}
	if (!found)
		dev_warn(&host->pdev->dev, "invalid block size %u\n",
			cfg->block_size);

	for (i = 0, found = 0; i < ARRAY_SIZE(page_sizes); i++)
		if (page_sizes[i] == cfg->page_size) {
			WR_CONFIG(host->cs, PAGE_SIZE, i);
			found = 1;
		}
	if (!found)
		dev_warn(&host->pdev->dev, "invalid page size %u\n",
			cfg->page_size);

	if (fls64(cfg->device_size) < 23)
		dev_warn(&host->pdev->dev, "invalid device size 0x%llx\n",
			(unsigned long long)cfg->device_size);

	WR_CONFIG(host->cs, DEVICE_SIZE, fls64(cfg->device_size) - 23);
	WR_CONFIG(host->cs, DEVICE_WIDTH, cfg->device_width == 16 ? 1 : 0);
	WR_CONFIG(host->cs, COL_ADR_BYTES, cfg->col_adr_bytes);
	WR_CONFIG(host->cs, BLK_ADR_BYTES, cfg->blk_adr_bytes);
	WR_CONFIG(host->cs, FUL_ADR_BYTES, cfg->ful_adr_bytes);

	WR_ACC_CONTROL(host->cs, SPARE_AREA_SIZE,
		cfg->spare_area_size >= 27 ? 27 : 16);
#if CONTROLLER_VER >= 50
	WR_ACC_CONTROL(host->cs, SECTOR_SIZE_1K, cfg->sector_size_1k);
#endif
}

static void brcmstb_nand_get_cfg(struct brcmstb_nand_host *host,
	struct brcmstb_nand_cfg *cfg)
{
	cfg->block_size = RD_CONFIG(host->cs, BLOCK_SIZE);
	cfg->device_size = (4ULL << 20) << RD_CONFIG(host->cs, DEVICE_SIZE);
	cfg->page_size = RD_CONFIG(host->cs, PAGE_SIZE);
	cfg->device_width = RD_CONFIG(host->cs, DEVICE_WIDTH) ? 16 : 8;
	cfg->col_adr_bytes = RD_CONFIG(host->cs, COL_ADR_BYTES);
	cfg->blk_adr_bytes = RD_CONFIG(host->cs, BLK_ADR_BYTES);
	cfg->ful_adr_bytes = RD_CONFIG(host->cs, FUL_ADR_BYTES);
	cfg->spare_area_size = RD_ACC_CONTROL(host->cs, SPARE_AREA_SIZE);
#if CONTROLLER_VER >= 50
	cfg->sector_size_1k = RD_ACC_CONTROL(host->cs, SECTOR_SIZE_1K);
#else
	cfg->sector_size_1k = 0;
#endif

	if (cfg->block_size < ARRAY_SIZE(block_sizes))
		cfg->block_size = block_sizes[cfg->block_size] << 10;
	else
		cfg->block_size = 128 << 10;

	if (cfg->page_size < ARRAY_SIZE(page_sizes))
		cfg->page_size = page_sizes[cfg->page_size];
	else
		cfg->page_size = 2048;
}

static void brcmstb_nand_print_cfg(char *buf, struct brcmstb_nand_cfg *cfg)
{
	sprintf(buf,
		"%lluMiB total, %uKiB blocks, %u%s pages, %uB OOB, %u-bit",
		(unsigned long long)cfg->device_size >> 20,
		cfg->block_size >> 10,
		cfg->page_size >= 1024 ? cfg->page_size >> 10 : cfg->page_size,
		cfg->page_size >= 1024 ? "KiB" : "B",
		cfg->spare_area_size, cfg->device_width);
}

static int __devinit brcmstb_nand_setup_dev(struct brcmstb_nand_host *host)
{
	struct mtd_info *mtd = &host->mtd;
	struct nand_chip *chip = &host->chip;
	struct brcmstb_nand_cfg orig_cfg, new_cfg;
	char msg[128];
	unsigned int ecclevel;

	brcmstb_nand_get_cfg(host, &orig_cfg);
	host->hwcfg = orig_cfg;

	memset(&new_cfg, 0, sizeof(new_cfg));
	new_cfg.device_size = mtd->size;
	new_cfg.block_size = mtd->erasesize;
	new_cfg.page_size = mtd->writesize;
	new_cfg.spare_area_size = mtd->oobsize / (mtd->writesize >> FC_SHIFT);
	new_cfg.device_width = (chip->options & NAND_BUSWIDTH_16) ? 16 : 8;
	new_cfg.col_adr_bytes = 2;

	if (mtd->writesize > 512)
		if (mtd->size >= (256 << 20))
			new_cfg.blk_adr_bytes = 3;
		else
			new_cfg.blk_adr_bytes = 2;
	else
		if (mtd->size >= (64 << 20))
			new_cfg.blk_adr_bytes = 3;
		else
			new_cfg.blk_adr_bytes = 2;
	new_cfg.ful_adr_bytes = new_cfg.blk_adr_bytes + new_cfg.col_adr_bytes;

	/* use bootloader spare_area_size if it's "close enough" */
	if (abs(new_cfg.spare_area_size - orig_cfg.spare_area_size) < 2)
		new_cfg.spare_area_size = orig_cfg.spare_area_size;

	if (orig_cfg.device_size != new_cfg.device_size ||
			orig_cfg.block_size != new_cfg.block_size ||
			orig_cfg.page_size != new_cfg.page_size ||
			orig_cfg.spare_area_size != new_cfg.spare_area_size ||
			orig_cfg.device_width != new_cfg.device_width ||
			orig_cfg.col_adr_bytes != new_cfg.col_adr_bytes ||
			orig_cfg.blk_adr_bytes != new_cfg.blk_adr_bytes ||
			orig_cfg.ful_adr_bytes != new_cfg.ful_adr_bytes) {
		if (BDEV_RD(BCHP_NAND_CS_NAND_SELECT) & (0x100 << host->cs)) {
			/* bootloader activated this CS */
			dev_warn(&host->pdev->dev, "overriding bootloader "
				"settings on CS%d\n", host->cs);
			brcmstb_nand_print_cfg(msg, &orig_cfg);
			dev_warn(&host->pdev->dev, "was: %s\n", msg);
			brcmstb_nand_print_cfg(msg, &new_cfg);
			dev_warn(&host->pdev->dev, "now: %s\n", msg);
		} else {
			/*
			 * nandcs= argument activated this CS; assume that
			 * nobody even tried to set the device configuration
			 */
			brcmstb_nand_print_cfg(msg, &new_cfg);
			dev_info(&host->pdev->dev, "detected %s\n", msg);
		}

#if CONTROLLER_VER >= 50
		/* default to 1K sector size (if page is large enough) */
		new_cfg.sector_size_1k = (new_cfg.page_size >= 1024) ? 1 : 0;
#endif

		brcmstb_nand_set_cfg(host, &new_cfg);
		host->hwcfg = new_cfg;

		WR_ACC_CONTROL(host->cs, RD_ECC_EN, 1);
		WR_ACC_CONTROL(host->cs, WR_ECC_EN, 1);

		if (new_cfg.spare_area_size >= 21)
			ecclevel = 12;
		else if (chip->badblockpos == NAND_SMALL_BADBLOCK_POS)
			ecclevel = 6;
		else
			ecclevel = 8;

		WR_ACC_CONTROL(host->cs, ECC_LEVEL, ecclevel);

		/* Account for 24-bit per 1024-byte ECC settings */
		if (new_cfg.sector_size_1k)
			dev_info(&host->pdev->dev, "ECC set to BCH-%u (1KiB "
					"sector)\n", ecclevel << 1);
		else
			dev_info(&host->pdev->dev, "ECC set to BCH-%u\n",
					ecclevel);
	}

	WR_ACC_CONTROL(host->cs, FAST_PGM_RDIN, 0);
	WR_ACC_CONTROL(host->cs, RD_ERASED_ECC_EN, 0);
	WR_ACC_CONTROL(host->cs, PARTIAL_PAGE_EN, 0);
	WR_ACC_CONTROL(host->cs, PAGE_HIT_EN, 1);
	mb();

	return 0;
}

static int __devinit brcmstb_nand_probe(struct platform_device *pdev)
{
	struct brcmnand_platform_data *pd = pdev->dev.platform_data;
	struct brcmstb_nand_host *host;
	struct mtd_info *mtd;
	struct nand_chip *chip;
	int ret = 0;

#ifdef CONFIG_MTD_PARTITIONS
	int nr_parts;
	struct mtd_partition *parts;
	const char *part_probe_types[] = { "cmdlinepart", "RedBoot", NULL };
#endif

	DBG("%s: id %d cs %d\n", __func__, pdev->id, pd->chip_select);

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host) {
		dev_err(&pdev->dev, "can't allocate memory\n");
		return -ENOMEM;
	}

	host->cs = pd->chip_select;

	mtd = &host->mtd;
	chip = &host->chip;
	host->pdev = pdev;
	dev_set_drvdata(&pdev->dev, host);

	chip->priv = host;
	mtd->priv = chip;
	mtd->name = dev_name(&pdev->dev);
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = &pdev->dev;

	chip->IO_ADDR_R = (void *)0xdeadbeef;
	chip->IO_ADDR_W = (void *)0xdeadbeef;

	chip->cmd_ctrl = brcmstb_nand_cmd_ctrl;
	chip->cmdfunc = brcmstb_nand_cmdfunc;
	chip->waitfunc = brcmstb_nand_waitfunc;
	chip->read_byte = brcmstb_nand_read_byte;
	chip->read_buf = brcmstb_nand_read_buf;
	chip->verify_buf = brcmstb_nand_verify_buf;

	chip->ecc.mode = NAND_ECC_HW;
	chip->ecc.size = 512;
	chip->ecc.layout = &brcmstb_nand_oob_layout;
	chip->ecc.read_page = brcmstb_nand_read_page;
	chip->ecc.read_subpage = brcmstb_nand_read_subpage;
	chip->ecc.write_page = brcmstb_nand_write_page;
	chip->ecc.read_page_raw = brcmstb_nand_read_page_raw;
	chip->ecc.write_page_raw = brcmstb_nand_write_page_raw;
	chip->ecc.read_oob = brcmstb_nand_read_oob;
	chip->ecc.write_oob = brcmstb_nand_write_oob;

	chip->controller = &ctrl.controller;

	if (brcmstb_check_exceptions(mtd) && nand_scan_ident(mtd, 1, NULL)) {
		ret = -ENXIO;
		goto out;
	}
	chip->options |= NAND_NO_SUBPAGE_WRITE | NAND_NO_AUTOINCR |
		NAND_SKIP_BBTSCAN | NAND_USE_FLASH_BBT |
		NAND_USE_FLASH_BBT_NO_OOB;
	if (nand_scan_tail(mtd) || brcmstb_nand_setup_dev(host) ||
			chip->scan_bbt(mtd)) {
		ret = -ENXIO;
		goto out;
	}

#ifdef CONFIG_MTD_PARTITIONS
	nr_parts = parse_mtd_partitions(mtd, part_probe_types, &parts, 0);
	if (nr_parts <= 0) {
		nr_parts = pd->nr_parts;
		parts = pd->parts;
	}

	if (nr_parts)
		add_mtd_partitions(mtd, parts, nr_parts);
	else
#endif
		add_mtd_device(mtd);
	return 0;

out:
	kfree(host);
	return ret;
}

static int __devexit brcmstb_nand_remove(struct platform_device *pdev)
{
	struct brcmstb_nand_host *host = dev_get_drvdata(&pdev->dev);
	struct mtd_info *mtd = &host->mtd;

	nand_release(mtd);
	dev_set_drvdata(&pdev->dev, NULL);
	kfree(host);

	return 0;
}

/***********************************************************************
 * Platform driver setup (per controller)
 ***********************************************************************/

static struct platform_driver brcmstb_nand_driver = {
	.probe			= brcmstb_nand_probe,
	.remove			= __devexit_p(brcmstb_nand_remove),
	.driver = {
		.name		= "brcmnand",
		.owner		= THIS_MODULE,
	},
};

#define BREG_PA(x)		(BPHYSADDR(BCHP_##x##_REG_START))
#define BREG_LEN(x)		(BCHP_##x##_REG_END + 4 - BCHP_##x##_REG_START)

static int __init brcmstb_nand_init(void)
{
	int err = -ENODEV;

	init_completion(&ctrl.done);
	spin_lock_init(&ctrl.controller.lock);
	init_waitqueue_head(&ctrl.controller.wq);

	if (!request_mem_region(BREG_PA(NAND), BREG_LEN(NAND), DRV_NAME)) {
		printk(KERN_ERR "%s: can't request memory region\n", __func__);
		return err;
	}

#ifdef CONFIG_BRCM_HAS_EDU
	if (!request_mem_region(BREG_PA(EDU), BREG_LEN(EDU), DRV_NAME)) {
		printk(KERN_ERR "%s: can't request memory region\n", __func__);
		release_mem_region(BREG_PA(NAND), BREG_LEN(NAND));
		return err;
	}

#ifdef CONFIG_CPU_LITTLE_ENDIAN
	BDEV_WR_RB(BCHP_EDU_CONFIG, 0x01);
#else
	BDEV_WR_RB(BCHP_EDU_CONFIG, 0x03);
#endif

	BDEV_WR(BCHP_EDU_ERR_STATUS, 0);
	BDEV_WR(BCHP_EDU_DONE, 0);
	BDEV_WR(BCHP_EDU_DONE, 0);
	BDEV_WR(BCHP_EDU_DONE, 0);
	BDEV_WR(BCHP_EDU_DONE, 0);
#endif /* CONFIG_BRCM_HAS_EDU */

	BDEV_WR_F(NAND_CS_NAND_SELECT, AUTO_DEVICE_ID_CONFIG, 0);

	/* disable direct addressing + XOR for all NAND devices */
	BDEV_UNSET(BCHP_NAND_CS_NAND_SELECT, 0xff);
	BDEV_UNSET(BCHP_NAND_CS_NAND_XOR, 0xff);

#ifdef BCHP_NAND_CS_NAND_SELECT_NAND_WP_MASK
	if (wp_on == 2) /* SWLINUX-1818: Permanently remove write-protection */
		BDEV_WR_F_RB(NAND_CS_NAND_SELECT, NAND_WP, 0);
#else
	wp_on = 0;
#endif

	ctrl.irq = BRCM_IRQ_HIF;

	HIF_ACK_IRQ(NAND_CTLRDY);
	HIF_ENABLE_IRQ(NAND_CTLRDY);

	err = request_irq(ctrl.irq, brcmstb_nand_irq, IRQF_SHARED,
		DRV_NAME, &ctrl);
	if (err < 0) {
		printk(KERN_ERR "%s: can't allocate IRQ %d: error %d\n",
			__func__, ctrl.irq, err);
		goto out2;
	}

	err = platform_driver_register(&brcmstb_nand_driver);
	if (err < 0) {
		printk(KERN_ERR "%s: can't register platform driver "
			"(error %d)\n", __func__, err);
		goto out;
	}

	printk(KERN_INFO DRV_NAME ": NAND controller driver is loaded\n");
	return 0;

out:
	HIF_DISABLE_IRQ(NAND_CTLRDY);
	free_irq(ctrl.irq, &ctrl);
out2:
#ifdef CONFIG_BRCM_HAS_EDU
	release_mem_region(BREG_PA(EDU), BREG_LEN(EDU));
#endif
	release_mem_region(BREG_PA(NAND), BREG_LEN(NAND));
	return err;
}

static void __exit brcmstb_nand_exit(void)
{
	HIF_DISABLE_IRQ(NAND_CTLRDY);
	free_irq(ctrl.irq, &ctrl);
	platform_driver_unregister(&brcmstb_nand_driver);

#ifdef CONFIG_BRCM_HAS_EDU
	release_mem_region(BREG_PA(EDU), BREG_LEN(EDU));
#endif
	release_mem_region(BREG_PA(NAND), BREG_LEN(NAND));
}

module_init(brcmstb_nand_init);
module_exit(brcmstb_nand_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");
MODULE_DESCRIPTION("NAND driver for STB chips");
