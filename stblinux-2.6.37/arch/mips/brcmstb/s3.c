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

#if 0
#define DBG(...)	printk(KERN_DEBUG __VA_ARGS__)
#else
#define DBG(...)	do { } while (0)
#endif

#define CALCULATE_MEM_HASH		(0)
#define VERIFY_HASH			(0)
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

asmlinkage int brcm_pm_s3_standby_asm(unsigned long options,
	void (*dram_encoder_start)(void));

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

/***********************************************************************
 * Encryption setup for S3 suspend
 ***********************************************************************/
#if CALCULATE_MEM_HASH
/*
 * Default scheme encrypts specified memory region and writes encoded data
 * to a temporary buffer, overwriting previous data on every block write.
 * Upon encoding final block, last 16 bytes are stored into AON, then
 * entire temporary buffer is wiped out
 * Test code will repeat encoding immediately after fake S3 suspend to see
 * if hash value remains the same. Different hash would indicate that encoded
 * area has been written to since M2M DMA pass.
 * brcm_pm_s3_tmpbuf - temporary buffer
 * hash_pointer - pointer to location in memory where hash value is located
 * after encoding
 */
static char __section(.s3_enc_tmpbuf) brcm_pm_s3_tmpbuf[PAGE_SIZE];
static char *hash_pointer;

#define MEM_ENCRYPTED_START_ADDR	(0x80000000)
#define MEM_ENCRYPTED_LEN		(1*1024*1024)
#define	S3_HASH_LEN			16

#define MAX_TRANSFER_SIZE_MB		(16)
#define MAX_TRANSFER_SIZE		(MAX_TRANSFER_SIZE_MB*1024*1024)

#define AES_CBC_ENCRYPTION_KEY_SLOT	(5)
#define AES_CBC_DECRYPTION_KEY_SLOT	(6)

/*
 * brcm_pm_dram_encoder_set_area
 * Generates a list of descriptors needed to encrypt area
 * VA _begin.._begin+len-1 into PA outbuf..outbuf+out_len-1
 * On return out_len is the length of the last transfer, this
 * can be used as an offset into the outbuf to extract hash
 */
static struct brcm_mem_transfer *brcm_pm_dram_encoder_set_area(void *_begin,
	u32 len, dma_addr_t outbuf, u32 *out_len)
{
	int num_descr = (len + *out_len - 1) / *out_len;
	struct brcm_mem_transfer *xfer, *x, *last_x;
	void *_end = _begin + len;

	BUG_ON(*out_len > MAX_TRANSFER_SIZE);

	xfer = kzalloc(num_descr * sizeof(struct brcm_mem_transfer),
		GFP_ATOMIC);

	if (!xfer) {
		printk(KERN_ERR "%s: out of memory\n", __func__);
		return NULL;
	}

	last_x = x = xfer;
	while (_begin < _end) {
		dma_addr_t pa_src = virt_to_phys(_begin);
		if (_begin + *out_len > _end)
			*out_len = _end - _begin;
		/* workaround - M2M DMA does not like 0 address */
		x->len		= *out_len;
		if (!pa_src) {
			pa_src += 0x10;
			x->len -= 0x10;
		}
		x->pa_src	= pa_src;
		x->pa_dst	= outbuf;
		x->mode		= BRCM_MEM_DMA_SCRAM_BLOCK;
		x->key		= AES_CBC_ENCRYPTION_KEY_SLOT;
		x->next		= x + 1;
		last_x = x;
		x++;
		_begin += *out_len;
	}
	last_x->next = NULL;

	return xfer;
}

static void brcm_pm_dram_encoder_free_area(struct brcm_mem_transfer *xfer)
{
	kfree(xfer);
}

/* TODO: rewrite in assembly to avoid memory writes after encoding started */
static void brcm_pm_dram_encode(void)
{
	u32 *hp = (u32 *)hash_pointer;
	/* clear temporary buffer */
	memset(brcm_pm_s3_tmpbuf, 0, sizeof(brcm_pm_s3_tmpbuf));
	_dma_cache_wback_inv(0, ~0);
	/* start M2M encoder */
	brcm_pm_dram_encoder_start();
	/* save hash in AON register */
	BDEV_WR(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x10, *hp++);
	BDEV_WR(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x14, *hp++);
	BDEV_WR(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x18, *hp++);
	BDEV_WR(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x1c, *hp++);

	/* clear temporary buffer */
	memset(brcm_pm_s3_tmpbuf, 0, sizeof(brcm_pm_s3_tmpbuf));
}
#else
#define brcm_pm_dram_encode	NULL
#endif

int __ref brcm_pm_s3_standby(unsigned long options)
{
	int retval = 0;
	unsigned long flags;

#if CALCULATE_MEM_HASH
	struct brcm_mem_transfer *xfer;
	u32 outlen = PAGE_SIZE;
#if VERIFY_HASH
	u32 old_hash[S3_HASH_LEN/4], new_hash[S3_HASH_LEN/4];
#endif
	/*
	 * We are using bi-directional mapping to avoid synchronizing cache
	 * after encoding
	 */
	dma_addr_t pa_dst = dma_map_single(NULL, brcm_pm_s3_tmpbuf, PAGE_SIZE,
		DMA_BIDIRECTIONAL);

	if (dma_mapping_error(NULL, pa_dst))
		return -1;

	xfer = brcm_pm_dram_encoder_set_area((void *)MEM_ENCRYPTED_START_ADDR,
		MEM_ENCRYPTED_LEN, pa_dst, &outlen);
	hash_pointer = brcm_pm_s3_tmpbuf + outlen - S3_HASH_LEN;

	if (!xfer)
		return -1;

	if (brcm_pm_dram_encoder_prepare(xfer)) {
		brcm_pm_dram_encoder_free_area(xfer);
		dma_unmap_single(NULL, pa_dst, PAGE_SIZE, DMA_BIDIRECTIONAL);
		return -1;
	}
#endif

	local_irq_save(flags);
	/* save CP0 context */
	brcm_pm_init_cp0_context(&s3_context);
	brcm_pm_save_cp0_context(&s3_context);

	/* save I/O context */

	/* Test code to simulate power down of ONOFF part of the chip */
#if 0
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
#endif

	retval = brcm_pm_s3_standby_asm(options, brcm_pm_dram_encode);

#if CALCULATE_MEM_HASH && VERIFY_HASH
	/*
	 * Test assumes memory has not changed since previous encryption.
	 * This is why we cannot run the hash calculation over entire
	 * memory. In real life bootloader will be doing this at the early
	 * stages of warm boot
	 */
	/* save for comparison */
	old_hash[0] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x10);
	old_hash[1] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x14);
	old_hash[2] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x18);
	old_hash[3] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x1c);
	/* Calculate hash again */
	brcm_pm_dram_encoder_complete(xfer);
	brcm_pm_dram_encoder_prepare(xfer);
	brcm_pm_dram_encode();
#endif

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
#if defined(CONFIG_BRCM_HAS_MOCA)
	bchip_moca_init();
#endif
	local_irq_restore(flags);

#if CALCULATE_MEM_HASH
	brcm_pm_dram_encoder_complete(xfer);
	brcm_pm_dram_encoder_free_area(xfer);
	dma_unmap_single(NULL, pa_dst, PAGE_SIZE, DMA_BIDIRECTIONAL);

#if VERIFY_HASH
	{
		int ii;
		unsigned char *hp;
		new_hash[0] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x10);
		new_hash[1] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x14);
		new_hash[2] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x18);
		new_hash[3] = BDEV_RD(BCHP_AON_CTRL_SYSTEM_DATA_00 + 0x1c);
		if (memcmp(new_hash, old_hash, S3_HASH_LEN))
			DBG("Hash mismatch!\n");
		DBG("Old hash: ");
		hp = (unsigned char *)old_hash;
		for (ii = 0; ii < S3_HASH_LEN; ii++)
			DBG("%02x", *hp++);
		DBG("\nNew hash: ");
		hp = (unsigned char *)new_hash;
		for (ii = 0; ii < S3_HASH_LEN; ii++)
			DBG("%02x", *hp++);
		DBG("\n");
	}
#endif
#endif

	return retval;
}
