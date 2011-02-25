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

#include <stdarg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/smp.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/mii.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/compiler.h>

#include <asm/cpu-info.h>
#include <asm/r4kcache.h>
#include <asm/mipsregs.h>
#include <asm/cacheflush.h>
#include <asm/brcmstb/brcmstb.h>

#if 0
#define DBG			printk
#else
#define DBG(...)		do { } while (0)
#endif

#define BRCM_PM_LIGHTWEIGHT_RESUME	(0)

/***********************************************************************
 * CPU divisor / PLL manipulation
 ***********************************************************************/

#ifdef CONFIG_BMIPS5000
/* CP0 COUNT/COMPARE frequency is affected by the PLL input but not divisor */
#define FIXED_COUNTER_FREQ	1
#else
/* CP0 COUNT/COMPARE frequency is affected by the PLL input AND the divisor */
#define FIXED_COUNTER_FREQ	0
#endif

/* MIPS active standby on 7550 */
#define CPU_PLL_MODE1		216000

/*
 * current ADJUSTED base frequency (reflects the current PLL settings)
 * brcm_cpu_khz (in time.c) always has the ORIGINAL clock frequency and
 *   is never changed after bootup
 */
unsigned long brcm_adj_cpu_khz;

/* current CPU divisor, as set by the user */
static int cpu_div = 1;

/*
 * MIPS clockevent code always assumes the original boot-time CP0 clock rate.
 * This function scales the number of ticks according to the current HW
 * settings.
 */
unsigned long brcm_fixup_ticks(unsigned long delta)
{
	unsigned long long tmp = delta;

	if (unlikely(!brcm_adj_cpu_khz))
		brcm_adj_cpu_khz = brcm_cpu_khz;
#if FIXED_COUNTER_FREQ
	tmp = (tmp * brcm_adj_cpu_khz) / brcm_cpu_khz;
#else
	tmp = (tmp * brcm_adj_cpu_khz) / (brcm_cpu_khz * cpu_div);
#endif
	return (unsigned long)tmp;
}

static unsigned int orig_udelay_val[NR_CPUS];

struct spd_change {
	int			old_div;
	int			new_div;
	int			old_base;
	int			new_base;
};

void brcm_set_cpu_speed(void *arg)
{
	struct spd_change *c = arg;
	uint32_t new_div = (uint32_t)c->new_div;
	unsigned long __maybe_unused count, compare, delta;
	int cpu = smp_processor_id();
	uint32_t __maybe_unused tmp0, tmp1, tmp2, tmp3;

	/* scale udelay_val */
	if (!orig_udelay_val[cpu])
		orig_udelay_val[cpu] = current_cpu_data.udelay_val;
	if (c->new_base == brcm_cpu_khz)
		current_cpu_data.udelay_val = orig_udelay_val[cpu] / new_div;
	else
		current_cpu_data.udelay_val =
			(unsigned long long)orig_udelay_val[cpu] *
			c->new_base / (new_div * c->old_base);

	/* scale any pending timer events */
	compare = read_c0_compare();
	count = read_c0_count();

#if !FIXED_COUNTER_FREQ
	if (compare > count)
		delta = ((unsigned long long)(compare - count) *
			c->old_div * c->new_base) /
			(new_div * c->old_base);
	else
		delta = ((unsigned long long)(count - compare) *
			c->old_div * c->new_base) /
			(new_div * c->old_base);
#else
	if (compare > count)
		delta = ((unsigned long long)(compare - count) *
			c->new_base) / c->old_base;
	else
		delta = ((unsigned long long)(count - compare) *
			c->new_base) / c->old_base;
#endif
	write_c0_compare(read_c0_count() + delta);

	if (cpu != 0)
		return;

#if defined(CONFIG_BRCM_CPU_PLL)
	brcm_adj_cpu_khz = c->new_base;
#if defined(CONFIG_BCM7550)
	if (brcm_adj_cpu_khz == CPU_PLL_MODE1) {
		/* 216Mhz */
		BDEV_WR_RB(BCHP_VCXO_CTL_CONFIG_FSM_PLL_NEXT_CFG_3A,
			0x801b2806);
		BDEV_WR_RB(BCHP_VCXO_CTL_CONFIG_FSM_PLL_NEXT_CFG_3B,
			0x00300618);
	} else {
		/* 324Mhz */
		BDEV_WR_RB(BCHP_VCXO_CTL_CONFIG_FSM_PLL_NEXT_CFG_3A,
			0x801b2806);
		BDEV_WR_RB(BCHP_VCXO_CTL_CONFIG_FSM_PLL_NEXT_CFG_3B,
			0x00300418);
	}
	BDEV_WR_RB(BCHP_VCXO_CTL_CONFIG_FSM_PLL_UPDATE, 1);
#else
#error CPU PLL adjustment not supported on this chip
#endif
#endif

	new_div = ffs(new_div) - 1;

	/* see BMIPS datasheet, CP0 register $22 */

#if defined(CONFIG_BMIPS3300)
	change_c0_brcm_bus_pll(0x07 << 22, (new_div << 23) | (0 << 22));
#elif defined(CONFIG_BMIPS5000)
	change_c0_brcm_mode(0x0f << 4, (1 << 7) | (new_div << 4));
#elif defined(CONFIG_BMIPS4380)
	__asm__ __volatile__(
	"	.set	push\n"
	"	.set	noreorder\n"
	"	.set	nomacro\n"
	"	.set	mips32\n"
	/* get kseg1 address for CBA into %3 */
	"	mfc0	%3, $22, 6\n"
	"	li	%2, 0xfffc0000\n"
	"	and	%3, %2\n"
	"	li	%2, 0xa0000000\n"
	"	add	%3, %2\n"
	/* %1 = async bit, %2 = mask out everything but 30:28 */
	"	lui	%1, 0x1000\n"
	"	lui	%2, 0x8fff\n"
	"	beqz	%0, 1f\n"
	"	ori	%2, 0xffff\n"
	/* handle SYNC to ASYNC */
	"	sync\n"
	"	mfc0	%4, $22, 5\n"
	"	and	%4, %2\n"
	"	or	%4, %1\n"
	"	mtc0	%4, $22, 5\n"
	"	nop\n"
	"	nop\n"
	"	lw	%2, 4(%3)\n"
	"	sw	%2, 4(%3)\n"
	"	sync\n"
	"	sll	%0, 29\n"
	"	or	%4, %0\n"
	"	mtc0	%4, $22, 5\n"
	"	nop; nop; nop; nop\n"
	"	nop; nop; nop; nop\n"
	"	nop; nop; nop; nop\n"
	"	nop; nop; nop; nop\n"
	"	b	2f\n"
	"	nop\n"
	/* handle ASYNC to SYNC */
	"1:\n"
	"	mfc0	%4, $22, 5\n"
	"	and	%4, %2\n"
	"	or	%4, %1\n"
	"	mtc0	%4, $22, 5\n"
	"	nop; nop; nop; nop\n"
	"	nop; nop; nop; nop\n"
	"	sync\n"
	"	and	%4, %2\n"
	"	mtc0	%4, $22, 5\n"
	"	nop\n"
	"	nop\n"
	"	lw	%2, 4(%3)\n"
	"	sw	%2, 4(%3)\n"
	"	sync\n"
	"2:\n"
	"	.set	pop\n"
	: "+r" (new_div),
	  "=&r" (tmp0), "=&r" (tmp1), "=&r" (tmp2), "=&r" (tmp3));
#endif
}

#ifdef CONFIG_BRCM_CPU_DIV

ssize_t brcm_pm_show_cpu_div(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", cpu_div);
}

ssize_t brcm_pm_store_cpu_div(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int val;
	struct spd_change chg;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if (val != 1 && val != 2 && val != 4 && val != 8
#if defined(CONFIG_BMIPS5000)
		&& val != 16
#endif
			)
		return -EINVAL;

	chg.old_div = cpu_div;
	chg.new_div = val;
	chg.old_base = brcm_adj_cpu_khz;
	chg.new_base = brcm_adj_cpu_khz;

	on_each_cpu(brcm_set_cpu_speed, &chg, 1);
	cpu_div = val;
	return count;
}

#endif /* CONFIG_BRCM_CPU_DIV */

#ifdef CONFIG_BRCM_CPU_PLL

static int cpu_pll_mode;

ssize_t brcm_pm_show_cpu_pll(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", cpu_pll_mode);
}

ssize_t brcm_pm_store_cpu_pll(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int val;
	struct spd_change chg;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if (cpu_pll_mode == val)
		return count;

	switch (val) {
	case 0:
		chg.new_base = brcm_cpu_khz;
		break;
	case 1:
		chg.new_base = CPU_PLL_MODE1;
		break;
	default:
		return -EINVAL;
	}

	chg.old_div = cpu_div;
	chg.new_div = cpu_div;
	chg.old_base = brcm_adj_cpu_khz;
	on_each_cpu(brcm_set_cpu_speed, &chg, 1);

	cpu_pll_mode = val;
	return count;
}

#endif /* CONFIG_BRCM_CPU_PLL */

/***********************************************************************
 * USB / ENET / GENET / MoCA / SATA PM common internal functions
 ***********************************************************************/
struct clk {
	char			name[16];
	spinlock_t      lock;
	int             refcnt;
	struct clk     *parent;
	int			    (*cb)(int, void *);
	void			*cb_arg;
	void			(*disable)(void);
	void			(*enable)(void);
};

static void brcm_pm_sata_disable(void);
static void brcm_pm_sata_enable(void);
static void brcm_pm_enet_disable(void);
static void brcm_pm_enet_enable(void);
static void brcm_pm_moca_disable(void);
static void brcm_pm_moca_enable(void);
static void brcm_pm_moca_genet_disable(void);
static void brcm_pm_moca_genet_enable(void);
static void brcm_pm_usb_disable(void);
static void brcm_pm_usb_enable(void);
static void brcm_pm_set_ddr_timeout(int);

static int brcm_pm_ddr_timeout;
static unsigned long brcm_pm_standby_flags;

enum {
	BRCM_CLK_SATA,
	BRCM_CLK_ENET,
	BRCM_CLK_MOCA,
	BRCM_CLK_USB,
	BRCM_CLK_MOCA_ENET
};

static struct clk brcm_clk_table[] = {
	[BRCM_CLK_SATA] = {
		.name		= "sata",
		.disable	= &brcm_pm_sata_disable,
		.enable		= &brcm_pm_sata_enable,
	},
	[BRCM_CLK_ENET] = {
		.name		= "enet",
		.disable	= &brcm_pm_enet_disable,
		.enable		= &brcm_pm_enet_enable,
		.parent     = &brcm_clk_table[BRCM_CLK_MOCA_ENET],
	},
	[BRCM_CLK_MOCA] = {
		.name		= "moca",
		.disable	= &brcm_pm_moca_disable,
		.enable		= &brcm_pm_moca_enable,
		.parent     = &brcm_clk_table[BRCM_CLK_MOCA_ENET],
	},
	[BRCM_CLK_USB] = {
		.name		= "usb",
		.disable	= &brcm_pm_usb_disable,
		.enable		= &brcm_pm_usb_enable,
	},
	[BRCM_CLK_MOCA_ENET] = {
		.name		= "moca_enet",
		.disable	= &brcm_pm_moca_genet_disable,
		.enable		= &brcm_pm_moca_genet_enable,
	},

};

static struct clk *brcm_pm_clk_find(const char *name)
{
	int i;
	struct clk *clk = brcm_clk_table;
	for (i = 0; i < ARRAY_SIZE(brcm_clk_table); i++, clk++)
		if (!strcmp(name, clk->name))
			return clk;
	return NULL;
}

/* sysfs attributes */

ssize_t brcm_pm_show_sata_power(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct clk *clk = brcm_pm_clk_find("sata");
	return snprintf(buf, PAGE_SIZE, "%d\n", !!clk->refcnt);
}

ssize_t brcm_pm_store_sata_power(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct clk *clk = brcm_pm_clk_find("sata");
	int val;

	if (!clk || !clk->cb || sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	return clk->cb(val ? PM_EVENT_RESUME : PM_EVENT_SUSPEND,
		clk->cb_arg) ? : count;
}

ssize_t brcm_pm_show_ddr_timeout(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", brcm_pm_ddr_timeout);
}

ssize_t brcm_pm_store_ddr_timeout(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	brcm_pm_ddr_timeout = val;
	if (brcm_pm_enabled)
		brcm_pm_set_ddr_timeout(val);
	return count;
}

ssize_t brcm_pm_show_standby_flags(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%lx\n", brcm_pm_standby_flags);
}

ssize_t brcm_pm_store_standby_flags(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;
	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	brcm_pm_standby_flags = val;
	return count;
}

/* Boot time functions */

static int __init brcm_pm_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(brcm_clk_table); i++)
		spin_lock_init(&brcm_clk_table[i].lock);
	if (!brcm_pm_enabled)
		return 0;
	if (!brcm_moca_enabled)
		brcm_pm_moca_disable();
	return 0;
}

early_initcall(brcm_pm_init);

static int nopm_setup(char *str)
{
	brcm_pm_enabled = 0;
	return 0;
}

__setup("nopm", nopm_setup);

/***********************************************************************
 * USB / ENET / GENET / MoCA / SATA PM external API
 ***********************************************************************/

struct clk *clk_get(struct device *dev, const char *id)
{
	return brcm_pm_clk_find(id) ? : ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL(clk_get);

/* internal functions assume the lock is held */
static int __clk_enable(struct clk *clk)
{
	if (++(clk->refcnt) == 1 && brcm_pm_enabled) {
		printk(KERN_INFO "brcm-pm: enabling %s clocks\n",
			clk->name);
		clk->enable();
	}
	return 0;
}

static void __clk_disable(struct clk *clk)
{
	if (--(clk->refcnt) == 0 && brcm_pm_enabled) {
		printk(KERN_INFO "brcm-pm: disabling %s clocks\n",
			clk->name);
		clk->disable();
	}
}

int clk_enable(struct clk *clk)
{
	unsigned long flags;
	if (clk && !IS_ERR(clk)) {
		spin_lock_irqsave(&clk->lock, flags);
		if (clk->parent)
			clk_enable(clk->parent);
		__clk_enable(clk);
		spin_unlock_irqrestore(&clk->lock, flags);
		return 0;
	} else {
		return -EINVAL;
	}
}
EXPORT_SYMBOL(clk_enable);

void clk_disable(struct clk *clk)
{
	unsigned long flags;
	if (clk && !IS_ERR(clk)) {
		spin_lock_irqsave(&clk->lock, flags);
		__clk_disable(clk);
		if (clk->parent)
			clk_disable(clk->parent);
		spin_unlock_irqrestore(&clk->lock, flags);
	}
}
EXPORT_SYMBOL(clk_disable);

void clk_put(struct clk *clk)
{
}
EXPORT_SYMBOL(clk_put);

int clk_set_parent(struct clk *clk, struct clk *parent)
{
	unsigned long flags;
	if (clk && !IS_ERR(clk)) {
		spin_lock_irqsave(&clk->lock, flags);
		clk->parent = parent;
		spin_unlock_irqrestore(&clk->lock, flags);
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(clk_set_parent);

struct clk *clk_get_parent(struct clk *clk)
{
	if (clk && !IS_ERR(clk))
		return clk->parent;

	return NULL;
}
EXPORT_SYMBOL(clk_get_parent);

int brcm_pm_register_cb(char *name, int (*fn)(int, void *), void *arg)
{
	struct clk *clk = brcm_pm_clk_find(name);
	unsigned long flags;

	if (!clk)
		return -ENOENT;

	spin_lock_irqsave(&clk->lock, flags);
	BUG_ON(fn && clk->cb);
	clk->cb = fn;
	clk->cb_arg = arg;
	spin_unlock_irqrestore(&clk->lock, flags);

	return 0;
}
EXPORT_SYMBOL(brcm_pm_register_cb);

int brcm_pm_unregister_cb(char *name)
{
	return brcm_pm_register_cb(name, NULL, NULL);
}
EXPORT_SYMBOL(brcm_pm_unregister_cb);

#ifdef CONFIG_BRCM_HAS_STANDBY
/***********************************************************************
 * Lightweight suspend/resume
 ***********************************************************************/
struct brcm_wakeup_source {
	struct brcm_wakeup_ops		*ops;
	void				*ref;
	struct list_head		list;
	struct kref			kref;
	char				name[16];
};

struct brcm_wakeup_control {
	spinlock_t			lock;
	struct list_head		head;
	int				count;
};

static struct brcm_wakeup_control bwc;

int brcm_pm_wakeup_register(struct brcm_wakeup_ops *ops, void* ref, char* name)
{
	struct brcm_wakeup_source *bws;
	unsigned long flags;

	list_for_each_entry(bws, &bwc.head, list) {
		if (bws->ops == ops && bws->ref == ref) {
			/* already registered */
			spin_lock_irqsave(&bwc.lock, flags);
			kref_get(&bws->kref);
			spin_unlock_irqrestore(&bwc.lock, flags);
			return 0;
		}
	}

	bws = kmalloc(sizeof(struct brcm_wakeup_source), GFP_ATOMIC);
	if (!bws)
		return -1;

	bws->ops = ops;
	bws->ref = ref;
	if (name)
		strncpy(bws->name, name, 16);

	kref_init(&bws->kref);
	kref_get(&bws->kref);

	spin_lock_irqsave(&bwc.lock, flags);
	list_add_tail(&bws->list, &bwc.head);
	spin_unlock_irqrestore(&bwc.lock, flags);

	return 0;
	}

/* This function is called with bwc lock held*/
static void brcm_pm_wakeup_cleanup(struct kref *kref)
{
	struct brcm_wakeup_source *bws =
		container_of(kref, struct brcm_wakeup_source, kref);
	list_del(&bws->list);
	kfree(bws);
}

int brcm_pm_wakeup_unregister(struct brcm_wakeup_ops *ops, void* ref)
{
	struct brcm_wakeup_source *bws;
	unsigned long flags;

	spin_lock_irqsave(&bwc.lock, flags);

	list_for_each_entry(bws, &bwc.head, list)
		if (bws->ops == ops && bws->ref == ref)
			kref_put(&bws->kref, brcm_pm_wakeup_cleanup);

	spin_unlock_irqrestore(&bwc.lock, flags);

	return 0;
}

static int brcm_pm_wakeup_enable(void)
{
	struct brcm_wakeup_source *bws;
	unsigned long flags;

	spin_lock_irqsave(&bwc.lock, flags);

	list_for_each_entry(bws, &bwc.head, list) {
		if (bws->ops && bws->ops->enable)
			bws->ops->enable(bws->ref);
	}
	spin_unlock_irqrestore(&bwc.lock, flags);
	return 0;
}

static int brcm_pm_wakeup_disable(void)
{
	struct brcm_wakeup_source *bws;
	unsigned long flags;

	spin_lock_irqsave(&bwc.lock, flags);

	list_for_each_entry(bws, &bwc.head, list) {
		if (bws->ops && bws->ops->disable)
			bws->ops->disable(bws->ref);
	}
	spin_unlock_irqrestore(&bwc.lock, flags);
	return 0;
}

/*
Function asks all registered objects if a wakeup event has happened.
It returns 0 if no wake up event occurred, 1 otherwise.
*/
static int brcm_pm_wakeup_poll(void)
{
	int ev_occurred = !BRCM_PM_LIGHTWEIGHT_RESUME;
	struct brcm_wakeup_source *bws;
	unsigned long flags;

	spin_lock_irqsave(&bwc.lock, flags);

	list_for_each_entry(bws, &bwc.head, list) {
		if (bws->ops && bws->ops->poll)
			ev_occurred |= bws->ops->poll(bws->ref);
	}
	spin_unlock_irqrestore(&bwc.lock, flags);
	return ev_occurred;
}

int brcm_pm_wakeup_init(void)
{
	spin_lock_init(&bwc.lock);
	INIT_LIST_HEAD(&bwc.head);
	return 0;
}
early_initcall(brcm_pm_wakeup_init);

#endif

/***********************************************************************
 * USB / ENET / GENET / MoCA / SATA PM implementations (per-chip)
 ***********************************************************************/

/* Per-block power management operations pair.
 * Parameter flags can be later used to control wake-up capabilities
 */
struct brcm_chip_pm_block_ops {
	void (*enable)(u32 flags);
	void (*disable)(u32 flags);
};

struct brcm_chip_pm_ops {
	struct brcm_chip_pm_block_ops sata;
	struct brcm_chip_pm_block_ops genet;
	struct brcm_chip_pm_block_ops moca;
	struct brcm_chip_pm_block_ops usb;
	struct brcm_chip_pm_block_ops moca_genet;
	void (*suspend)(void);
	void (*resume)(void);
};

#define PLL_DIS(x)		BDEV_WR_RB(BCHP_##x, 0x04)
#define PLL_ENA(x)		do { \
					BDEV_WR_RB(BCHP_##x, 0x03); \
					mdelay(1); \
				} while (0)


#if defined(CONFIG_BCM7125)
static void bcm7125_pm_sata_disable(u32 flags)
{
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 1);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_99P7, 1);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_216, 1);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_108, 1);
	PLL_DIS(VCXO_CTL_MISC_RAP_AVD_PLL_CHL_4);
}

static void bcm7125_pm_sata_enable(u32 flags)
{
	PLL_ENA(VCXO_CTL_MISC_RAP_AVD_PLL_CHL_4);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_108, 0);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_216, 0);
	BDEV_WR_F_RB(CLKGEN_SATA_CLK_PM_CTRL, DIS_CLK_99P7, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 0);
}

static void bcm7125_pm_moca_genet_disable(u32 flags)
{
	BDEV_SET_RB(BCHP_CLKGEN_MOCA_CLK_PM_CTRL, 0xf77);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH3_PM_CTRL, 0x04);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH4_PM_CTRL, 0x04);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH5_PM_CTRL, 0x04);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH6_PM_CTRL, 0x04);
	BDEV_SET_RB(BCHP_CLKGEN_PLL_MOCA_CTRL, 0x13);
}

static void bcm7125_pm_moca_genet_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLKGEN_PLL_MOCA_CTRL, 0x13);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH6_PM_CTRL, 0x01);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH5_PM_CTRL, 0x01);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH4_PM_CTRL, 0x01);
	BDEV_WR_RB(BCHP_CLKGEN_PLL_MOCA_CH3_PM_CTRL, 0x01);
	BDEV_UNSET_RB(BCHP_CLKGEN_MOCA_CLK_PM_CTRL, 0xf77);
}

static void bcm7125_pm_usb_disable(u32 flags)
{
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x00);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_216, 1);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_108, 1);
	PLL_DIS(CLKGEN_PLL_MAIN_CH4_PM_CTRL);
}

static void bcm7125_pm_usb_enable(u32 flags)
{
	PLL_ENA(CLKGEN_PLL_MAIN_CH4_PM_CTRL);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_108, 0);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_216, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x0f);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
}

static void bcm7125_pm_suspend(void)
{
	/* PCI/EBI */
	BDEV_WR_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x3ff8);
	BDEV_WR_F_RB(CLKGEN_CLK_27_OUT_PM_CTRL, DIS_CLK_27_OUT, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_PM_DIS_CHL_1, DIS_CH, 1);
	BDEV_SET_RB(BCHP_CLKGEN_HIF_CLK_PM_CTRL, 0x3c);

	/* system PLLs */
	BDEV_SET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x0b);
	BDEV_UNSET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x04);
	BDEV_SET_RB(BCHP_VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, 0x07);
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_216, 1);
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_108, 1);

	/* MEMC0 */
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_POWERDOWN,
		PLLCLKS_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
}

static void bcm7125_pm_resume(void)
{
	/* system PLLs */
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_108, 0);
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_216, 0);
	BDEV_UNSET_RB(BCHP_VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, 0x07);
	BDEV_SET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x04);
	BDEV_UNSET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x0b);

	/* PCI/EBI */
	BDEV_UNSET_RB(BCHP_CLKGEN_HIF_CLK_PM_CTRL, 0x3c);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_PM_DIS_CHL_1, DIS_CH, 0);
	BDEV_WR_F_RB(CLKGEN_CLK_27_OUT_PM_CTRL, DIS_CLK_27_OUT, 0);
	BDEV_WR_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x00);

}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.sata.enable		= bcm7125_pm_sata_enable,
	.sata.disable		= bcm7125_pm_sata_disable,
	.moca_genet.enable	= bcm7125_pm_moca_genet_enable,
	.moca_genet.disable	= bcm7125_pm_moca_genet_disable,
	.usb.enable		= bcm7125_pm_usb_enable,
	.usb.disable		= bcm7125_pm_usb_disable,
	.suspend		= bcm7125_pm_suspend,
	.resume			= bcm7125_pm_resume,
};
#endif

#if defined(CONFIG_BCM7420)

static void bcm7420_pm_usb_disable(u32 flags)
{
	/* TODO: per-port power control */
	/* power down ports */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_PWDNB, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI1_PWDNB, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY1_PWDNB, 0);
	/* power down USB PLL */
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 0);
	/* disable the clocks */
	BDEV_SET_RB(BCHP_CLK_USB_PM_CTRL,
		BCHP_CLK_USB_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_216M_CLK_MASK);
	/* power down system PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_4, DIS_CH, 1);
}

static void bcm7420_pm_usb_enable(u32 flags)
{
	/* power up system PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_4, DIS_CH, 0);
	/* enable the clocks */
	BDEV_UNSET_RB(BCHP_CLK_USB_PM_CTRL,
		BCHP_CLK_USB_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_216M_CLK_MASK);
	/* power up PLL */
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 1);
	/* power up ports */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_PWDNB, 3);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 3);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI1_PWDNB, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY1_PWDNB, 1);
}

static void bcm7420_pm_sata_disable(u32 flags)
{
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_SATA_PCI_CLK, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_108M_CLK, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_216M_CLK, 1);
}

static void bcm7420_pm_sata_enable(u32 flags)
{
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_216M_CLK, 0);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_108M_CLK, 0);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_SATA_PCI_CLK, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 0);
}

static void bcm7420_pm_moca_genet_disable(u32 flags)
{
	PLL_DIS(CLK_SYS_PLL_1_3);
	PLL_DIS(CLK_SYS_PLL_1_4);
	PLL_DIS(CLK_SYS_PLL_1_5);
	PLL_DIS(CLK_SYS_PLL_1_6);
	BDEV_SET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x7bf);
}

static void bcm7420_pm_moca_genet_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x7bf);
	PLL_ENA(CLK_SYS_PLL_1_6);
	PLL_ENA(CLK_SYS_PLL_1_5);
	PLL_ENA(CLK_SYS_PLL_1_4);
	PLL_ENA(CLK_SYS_PLL_1_3);
}

static void bcm7420_pm_suspend(void)
{
	PLL_DIS(CLK_GENET_NETWORK_PLL_4);

	/* BT */
	PLL_DIS(CLK_SYS_PLL_1_1);
	BDEV_SET_RB(BCHP_CLK_BLUETOOTH_CLK_PM_CTRL, 1);

	/* PCIe */
	BDEV_WR_F_RB(HIF_RGR1_SW_RESET_1, PCIE_SW_PERST, 1);
	PLL_DIS(CLK_SYS_PLL_1_2);
	BDEV_SET_RB(BCHP_CLK_PCIE_CLK_PM_CTRL, 1);

	/* PCI/EBI */
	BDEV_WR_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x3fff);
	BDEV_WR_RB(BCHP_CLK_PCI_OUT_CLK_PM_CTRL, 0x1);
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_0_PLL_5, 0x2);

	/* system PLLs */
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, FREQ_DOUBLER_POWER_DOWN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, CML_2_N_P_EN, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, RESET, 1);
	PLL_DIS(CLK_GENET_NETWORK_PLL_1);
	BDEV_WR_F_RB(CLK_GENET_NETWORK_PLL_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(CLK_GENET_NETWORK_PLL_CTRL, RESET, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SCRATCH, CML_REPEATER_2_POWERDOWN, 1);

	/* MEMC1 */
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_1_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_1_DDR_PAD_CNTRL,
		HIZ_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_1_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_1_POWERDOWN,
		PLLCLKS_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_1_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_1_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_1_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_1_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);

	/* MEMC0 */
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_POWERDOWN,
		PLLCLKS_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
}

static void bcm7420_pm_resume(void)
{
	/* system PLLs */
	BDEV_WR_F_RB(CLK_SCRATCH, CML_REPEATER_2_POWERDOWN, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_GENET_NETWORK_PLL_CTRL, RESET, 0);
	BDEV_WR_F_RB(CLK_GENET_NETWORK_PLL_CTRL, POWERDOWN, 0);
	PLL_ENA(CLK_GENET_NETWORK_PLL_1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, RESET, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, CML_2_N_P_EN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, FREQ_DOUBLER_POWER_DOWN, 1);

	/* Sundry */
	BDEV_WR_F(CLK_SUN_27M_CLK_PM_CTRL, DIS_SUN_27M_CLK, 0);

	/* PCI/EBI */
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_0_PLL_5, 0x1);
	BDEV_WR_RB(BCHP_CLK_PCI_OUT_CLK_PM_CTRL, 0x0);
	BDEV_WR_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x0);

	/* PCIe */
	BDEV_UNSET_RB(BCHP_CLK_PCIE_CLK_PM_CTRL, 1);
	PLL_ENA(CLK_SYS_PLL_1_2);
	BDEV_WR_F_RB(HIF_RGR1_SW_RESET_1, PCIE_SW_PERST, 0);

	/* BT */
	BDEV_UNSET_RB(BCHP_CLK_BLUETOOTH_CLK_PM_CTRL, 1);
	PLL_ENA(CLK_SYS_PLL_1_1);

	PLL_ENA(CLK_GENET_NETWORK_PLL_4);
}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.usb.enable		= bcm7420_pm_usb_enable,
	.usb.disable		= bcm7420_pm_usb_disable,
	.sata.enable		= bcm7420_pm_sata_enable,
	.sata.disable		= bcm7420_pm_sata_disable,
	.moca_genet.enable	= bcm7420_pm_moca_genet_enable,
	.moca_genet.disable	= bcm7420_pm_moca_genet_disable,
	.suspend		= bcm7420_pm_suspend,
	.resume			= bcm7420_pm_resume,
};
#endif

#if defined(CONFIG_BCM7468)

static void bcm7468_pm_enet_disable(u32 flags)
{
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_1_4, 1);
	BDEV_SET_RB(BCHP_CLK_GENET_CLK_PM_CTRL, 0x767);
}

static void bcm7468_pm_enet_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_GENET_CLK_PM_CTRL, 0x767);
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_1_4, 0);
}

static void bcm7468_pm_usb_disable(u32 flags)
{
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0x00);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x00);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 0);
	BDEV_SET_RB(BCHP_CLK_USB_CLK_PM_CTRL, 0x07);
}

static void bcm7468_pm_usb_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_USB_CLK_PM_CTRL, 0x07);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x0f);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0x0f);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 0);
}

static void bcm7468_pm_suspend(void)
{
	/* SDIO */
	BDEV_WR_F_RB(CLK_HIF_SDIO_CLK_PM_CTRL, DIS_HIF_SDIO_48M_CLK, 1);
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_1_1, 1);

	/* EBI */
	BDEV_SET_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x2ff0);

	/* system PLLs */
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_0_3, 0x02);
	BDEV_SET_RB(BCHP_CLK_SYS_PLL_1_CTRL, 0x83);
	BDEV_WR_RB(BCHP_VCXO_CTL_MISC_AC1_CTRL, 0x06);
	BDEV_SET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x0b);
}

static void bcm7468_pm_resume(void)
{
	/* system PLLs */
	BDEV_UNSET_RB(BCHP_VCXO_CTL_MISC_VC0_CTRL, 0x0b);
	BDEV_WR_RB(BCHP_VCXO_CTL_MISC_AC1_CTRL, 0x00);
	BDEV_UNSET_RB(BCHP_CLK_SYS_PLL_1_CTRL, 0x83);
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_0_3, 0x01);

	/* EBI */
	BDEV_UNSET_RB(BCHP_HIF_TOP_CTRL_PM_CTRL, 0x2ff0);

	/* SDIO */
	BDEV_WR_RB(BCHP_CLK_SYS_PLL_1_1, 0);
	BDEV_WR_F_RB(CLK_HIF_SDIO_CLK_PM_CTRL, DIS_HIF_SDIO_48M_CLK, 0);
}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.genet.enable		= bcm7468_pm_enet_enable,
	.genet.disable		= bcm7468_pm_enet_disable,
	.usb.enable		= bcm7468_pm_usb_enable,
	.usb.disable		= bcm7468_pm_usb_disable,
	.suspend		= bcm7468_pm_suspend,
	.resume			= bcm7468_pm_resume,
};
#endif

#if defined(CONFIG_BCM7340)

static void bcm7340_pm_enet_disable(u32 flags)
{
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, PWRDN_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, ENB_CLOCKOUT_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, EN_CMLBUF_CH4, 0);
	BDEV_SET_RB(BCHP_CLKGEN_GENET_CLK_PM_CTRL,
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_250_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_RX_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_TX_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_L2_INTR_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_HFB_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_25_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_GMII_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_54_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_108_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_216_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_27X_PM_MASK);
}

static void bcm7340_pm_enet_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLKGEN_GENET_CLK_PM_CTRL,
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_250_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_RX_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_TX_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_L2_INTR_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_HFB_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_25_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_GMII_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_54_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_108_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_216_MASK|
		BCHP_CLKGEN_GENET_CLK_PM_CTRL_DIS_CLK_27X_PM_MASK);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, EN_CMLBUF_CH4, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, ENB_CLOCKOUT_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH4_PM_CTRL, PWRDN_CH, 0);
}

static void bcm7340_pm_moca_disable(u32 flags)
{
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, PWRDN_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, ENB_CLOCKOUT_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, EN_CMLBUF_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, PWRDN_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, ENB_CLOCKOUT_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, EN_CMLBUF_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, PWRDN_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, ENB_CLOCKOUT_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, EN_CMLBUF_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, PWRDN_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, ENB_CLOCKOUT_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, EN_CMLBUF_CH6, 0);
	BDEV_SET_RB(BCHP_CLKGEN_MOCA_CLK_PM_CTRL,
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_250_GENET_RGMII_MOCA_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_216_GENET_RGMII_CG_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_RX_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_TX_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_L2_INTR_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_HFB_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_GMII_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_27X_PM_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_54_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_108_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_216_MASK);
}

static void bcm7340_pm_moca_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLKGEN_MOCA_CLK_PM_CTRL,
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_250_GENET_RGMII_MOCA_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_216_GENET_RGMII_CG_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_RX_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_UNIMAC_SYS_TX_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_L2_INTR_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_HFB_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_GMII_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_27X_PM_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_54_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_108_MASK|
		BCHP_CLKGEN_MOCA_CLK_PM_CTRL_DIS_CLK_216_MASK);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, EN_CMLBUF_CH6, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, ENB_CLOCKOUT_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH6_PM_CTRL, PWRDN_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, EN_CMLBUF_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, ENB_CLOCKOUT_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH3_PM_CTRL, PWRDN_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, EN_CMLBUF_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, ENB_CLOCKOUT_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH2_PM_CTRL, PWRDN_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, EN_CMLBUF_CH, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, ENB_CLOCKOUT_CH, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CH1_PM_CTRL, PWRDN_CH, 0);
}

static void bcm7340_pm_usb_disable(u32 flags)
{
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0xC);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0xC);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 0);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_216, 1);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_108, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMAIN_CH4_PM_CTRL, PWRDN_CH4_PLLMAIN, 1);
}

static void bcm7340_pm_usb_enable(u32 flags)
{
	BDEV_WR_F_RB(CLKGEN_PLLMAIN_CH4_PM_CTRL, PWRDN_CH4_PLLMAIN, 0);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_108, 0);
	BDEV_WR_F_RB(CLKGEN_USB_CLK_PM_CTRL, DIS_CLK_216, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0xF);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0xF);
}

static void bcm7340_pm_suspend(void)
{
	/* PCI/EBI */
	BDEV_WR_F_RB(CLKGEN_PAD_CLK_PM_CTRL, DIS_CLK_33_27_PCI, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMAIN_CH5_PM_CTRL, PWRDN_CH5_PLLMAIN, 1);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_81, 1);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_27, 1);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_54, 1);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_216_CG, 1);
	BDEV_WR_F_RB(CLKGEN_HIF_CLK_PM_CTRL, DIS_CLK_SPI, 1);

	/* system PLLs */
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, DRESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, ARESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, DRESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, ARESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, POWERDOWN, 1);

	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_PM_CH2_CTRL, EN_CMLBUF, 0);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_PM_CH2_CTRL, PWRDN, 1);

	BDEV_WR_F_RB(VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, RESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, POWERDOWN, 1);

	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, ENB_CLOCKOUT, 1);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, DRESET, 1);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, ARESET, 1);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, PWRDN, 1);

	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_216, 1);
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_108, 1);

	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, DRESET, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, ARESET, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, PWRDN, 1);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, PWRDN_LDO, 1);

	/* MEMC0 */
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_POWERDOWN,
		PLLCLKS_OFF_ON_SELFREF, 1);
}

static void bcm7340_pm_resume(void)
{
	/* system PLLs */
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, PWRDN_LDO, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, PWRDN, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, ARESET, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMOCA_CTRL, DRESET, 0);

	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_108, 0);
	BDEV_WR_F_RB(CLKGEN_VCXO_CLK_PM_CTRL, DIS_CLK_216, 0);

	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, PWRDN, 0);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, ARESET, 0);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, DRESET, 0);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_CTRL, ENB_CLOCKOUT, 0);

	BDEV_WR_F_RB(VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, RESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_RAP_AVD_PLL_CTRL, POWERDOWN, 0);

	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_PM_CH2_CTRL, PWRDN, 1);
	BDEV_WR_F_RB(CLKGEN_PLLAVD_RDSP_PM_CH2_CTRL, EN_CMLBUF, 0);

	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, POWERDOWN, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, ARESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC1_CTRL, DRESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, POWERDOWN, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, ARESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, DRESET, 0);

	/* PCI/EBI */
	BDEV_WR_F_RB(CLKGEN_HIF_CLK_PM_CTRL, DIS_CLK_SPI, 0);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_216_CG, 0);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_54, 0);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_27, 0);
	BDEV_WR_F_RB(CLKGEN_CG_CLK_PM_CTRL, DIS_CLK_81, 0);
	BDEV_WR_F_RB(CLKGEN_PLLMAIN_CH5_PM_CTRL, PWRDN_CH5_PLLMAIN, 0);
	BDEV_WR_F_RB(CLKGEN_PAD_CLK_PM_CTRL, DIS_CLK_33_27_PCI, 0);
}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.genet.enable		= bcm7340_pm_enet_enable,
	.genet.disable		= bcm7340_pm_enet_disable,
	.moca.enable		= bcm7340_pm_moca_enable,
	.moca.disable		= bcm7340_pm_moca_disable,
	.usb.enable		= bcm7340_pm_usb_enable,
	.usb.disable		= bcm7340_pm_usb_disable,
	.suspend		= bcm7340_pm_suspend,
	.resume			= bcm7340_pm_resume,
};
#endif

#if defined(CONFIG_BCM7342)

static void bcm7342_pm_sata_disable(u32 flags)
{
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_216M_CLK, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_SATA_PCI_CLK, 1);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_108M_CLK, 1);
	PLL_DIS(CLK_SYS_PLL_1_6);
}

static void bcm7342_pm_sata_enable(u32 flags)
{
	PLL_ENA(CLK_SYS_PLL_1_6);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_108M_CLK, 0);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_216M_CLK, 0);
	BDEV_WR_F_RB(CLK_SATA_CLK_PM_CTRL, DIS_SATA_PCI_CLK, 0);
	BDEV_WR_F_RB(SUN_TOP_CTRL_GENERAL_CTRL_1, sata_ana_pwrdn, 0);
}


static void bcm7342_pm_enet_disable(u32 flags)
{
	PLL_DIS(VCXO_CTL_MISC_MOCA_PLL_CHL_1);
	BDEV_SET_RB(BCHP_CLK_GENET_CLK_PM_CTRL, 0x0000076f);
}

static void bcm7342_pm_enet_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_GENET_CLK_PM_CTRL, 0x0000076f);
	PLL_ENA(VCXO_CTL_MISC_MOCA_PLL_CHL_1);
}

static void bcm7342_pm_moca_disable(u32 flags)
{
	PLL_DIS(VCXO_CTL_MISC_MOCA_PLL_CHL_3);
	PLL_DIS(VCXO_CTL_MISC_MOCA_PLL_CHL_4);
	PLL_DIS(VCXO_CTL_MISC_MOCA_PLL_CHL_5);
	PLL_DIS(VCXO_CTL_MISC_MOCA_PLL_CHL_6);
	BDEV_SET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x7bf);
}

static void bcm7342_pm_moca_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x7bf);
	PLL_ENA(VCXO_CTL_MISC_MOCA_PLL_CHL_6);
	PLL_ENA(VCXO_CTL_MISC_MOCA_PLL_CHL_5);
	PLL_ENA(VCXO_CTL_MISC_MOCA_PLL_CHL_4);
	PLL_ENA(VCXO_CTL_MISC_MOCA_PLL_CHL_3);
}

static void bcm7342_pm_usb_disable(u32 flags)
{
	/* reset and power down all 4 ports */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0x0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x0);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 0);
	/* disable the clocks */
	BDEV_SET_RB(BCHP_CLK_USB_PM_CTRL,
		BCHP_CLK_USB_PM_CTRL_DIS_54M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_216M_CLK_MASK);
	/* disable PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_3, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_3, EN_CMLBUF, 0);
}

static void bcm7342_pm_usb_enable(u32 flags)
{
	/* enable PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_3, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_3, DIS_CH, 0);
	/* enable the clocks */
	BDEV_UNSET_RB(BCHP_CLK_USB_PM_CTRL,
		BCHP_CLK_USB_PM_CTRL_DIS_54M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_PM_CTRL_DIS_216M_CLK_MASK);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 0);
	/* power up all 4 ports */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0xF);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0xF);
}

static void bcm7342_pm_suspend(void)
{
	/* PCI/EBI */
	BDEV_WR_F_RB(CLK_PCI_OUT_CLK_PM_CTRL, DIS_PCI_OUT_CLK, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, EN_CMLBUF, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_1, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_1, EN_CMLBUF, 0);

	/* system PLLs */
	/* RMY: powering down 3OT oscillator kills UART */
	if (brcm_pm_standby_flags & BRCM_STANDBY_VERBOSE) {
		BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
			FREQ_DOUBLER_POWER_DOWN, 0);
		BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
			CML_2_N_P_EN, 0);
	} else {
		BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
			FREQ_DOUBLER_POWER_DOWN, 1);
		BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
			CML_2_N_P_EN, 1);
	}
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, RESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_MOCA_PLL_CTRL, RESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_MOCA_PLL_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_5, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_5, EN_CMLBUF, 0);
	/* MEMC0 */
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1,
		PWRDN_DLL_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_APHY_AC_0_POWERDOWN, PLLCLKS_OFF_ON_SELFREF, 1);
}

static void bcm7342_pm_resume(void)
{
	/* system PLLs */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_5, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_5, DIS_CH, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_MOCA_PLL_CTRL, POWERDOWN, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_MOCA_PLL_CTRL, RESET, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, RESET, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, CML_2_N_P_EN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1, FREQ_DOUBLER_POWER_DOWN, 0);

	/* PCI/EBI */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_1, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_1, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_PLL_6, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_PCI_OUT_CLK_PM_CTRL, DIS_PCI_OUT_CLK, 0);
}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.sata.enable		= bcm7342_pm_sata_enable,
	.sata.disable		= bcm7342_pm_sata_disable,
	.genet.enable		= bcm7342_pm_enet_enable,
	.genet.disable		= bcm7342_pm_enet_disable,
	.moca.enable		= bcm7342_pm_moca_enable,
	.moca.disable		= bcm7342_pm_moca_disable,
	.usb.enable		= bcm7342_pm_usb_enable,
	.usb.disable		= bcm7342_pm_usb_disable,
	.suspend		= bcm7342_pm_suspend,
	.resume			= bcm7342_pm_resume,
};
#endif

#if defined(CONFIG_BCM7408)
static void bcm7408_pm_moca_disable(u32 flags)
{
	BDEV_WR_F_RB(CLK_MOCA_CLK_PM_CTRL, DIS_MOCA_ENET_HFB_27_108M_CLK, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_3, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_3, EN_CMLBUF, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_4, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_4, EN_CMLBUF, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_5, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_5, EN_CMLBUF, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_6, DIS_CH, 1);
	BDEV_SET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x6ab);
}

static void bcm7408_pm_moca_enable(u32 flags)
{
	BDEV_UNSET_RB(BCHP_CLK_MOCA_CLK_PM_CTRL, 0x6ab);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_6, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_5, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_5, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_4, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_4, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_3, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_3, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_MOCA_CLK_PM_CTRL, DIS_MOCA_ENET_HFB_27_108M_CLK, 0);
}

static void bcm7408_pm_usb_disable(u32 flags)
{
	/* reset and power down all 4 ports */
/*	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0x0); */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0x0);
	/* disable the clocks */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 0);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 0);
	BDEV_SET_RB(BCHP_CLK_USB_CLK_PM_CTRL,
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_54M_CLK_MASK|
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_216M_CLK_MASK);
	/* disable PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_4, DIS_CH, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_4, EN_CMLBUF, 0);
}

static void bcm7408_pm_usb_enable(u32 flags)
{
	/* enable PLL */
	BDEV_WR_F_RB(CLK_SYS_PLL_0_4, EN_CMLBUF, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_0_4, DIS_CH, 0);
	/* enable the clocks */
	BDEV_UNSET_RB(BCHP_CLK_USB_CLK_PM_CTRL,
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_54M_CLK_MASK|
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_108M_CLK_MASK|
		BCHP_CLK_USB_CLK_PM_CTRL_DIS_216M_CLK_MASK);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, XTAL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_PLL_CTL_1, PLL_PWRDWNB, 1);
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_IDDQ, 0);
	/* power up all 4 ports */
	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, PHY_PWDNB, 0xF);
/*	BDEV_WR_F_RB(USB_CTRL_UTMI_CTL_1, UTMI_SOFT_RESETB, 0xE); */
}

static void bcm7408_pm_suspend(void)
{
	/* UART */
	if (!(brcm_pm_standby_flags & BRCM_STANDBY_VERBOSE)) {
		BDEV_WR_F_RB(CLK_SUN_27M_CLK_PM_CTRL,
			DIS_SUN_27M_CLK, 1);
		BDEV_WR_F_RB(CLK_SUN_UART_CLK_PM_CTRL,
			DIS_SUN_UART_108M_CLK, 1);
	}
	/* PAD clocks */
	BDEV_WR_F_RB(CLK_MISC, VCXOA_OUTCLK_ENABLE, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_PM_DIS_CHL_1, DIS_CH, 1);

	/* system PLLs */
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, DRESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, ARESET, 1);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, POWERDOWN, 1);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
		CML_2_N_P_EN, 1);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
		FREQ_DOUBLER_POWER_DOWN, 1);

	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, DRESET, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, ARESET, 1);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 1);

	/* MEMC0 */
	BDEV_WR_F_RB(MEMC_DDR23_SHIM_ADDR_CNTL_DDR_PAD_CNTRL,
		IDDQ_MODE_ON_SELFREF, 0);
	BDEV_WR_F_RB(MEMC_DDR23_SHIM_ADDR_CNTL_DDR_PAD_CNTRL,
		HIZ_ON_SELFREF, 1);
	BDEV_WR_F_RB(MEMC_DDR23_SHIM_ADDR_CNTL_DDR_PAD_CNTRL,
		DEVCLK_OFF_ON_SELFREF, 1);
	BDEV_WR_RB(BCHP_DDR23_PHY_CONTROL_REGS_0_IDLE_PAD_CONTROL, 0x132);
	BDEV_SET_RB(BCHP_DDR23_PHY_BYTE_LANE_0_0_IDLE_PAD_CONTROL, 0xfffff);
	BDEV_SET_RB(BCHP_DDR23_PHY_BYTE_LANE_1_0_IDLE_PAD_CONTROL, 0xfffff);

}

static void bcm7408_pm_resume(void)
{
	/* system PLLs */
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, DRESET, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, ARESET, 0);
	BDEV_WR_F_RB(CLK_SYS_PLL_1_CTRL, POWERDOWN, 0);

	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
		CML_2_N_P_EN, 0);
	BDEV_WR_F_RB(CLK_THIRD_OT_CONTROL_1,
		FREQ_DOUBLER_POWER_DOWN, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, DRESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, ARESET, 0);
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_CTRL, POWERDOWN, 0);

	/* PAD clocks */
	BDEV_WR_F_RB(VCXO_CTL_MISC_VC0_PM_DIS_CHL_1, DIS_CH, 0);
	BDEV_WR_F_RB(CLK_MISC, VCXOA_OUTCLK_ENABLE, 1);

	/* UART */
	if (!(brcm_pm_standby_flags & BRCM_STANDBY_VERBOSE)) {
		BDEV_WR_F_RB(CLK_SUN_UART_CLK_PM_CTRL,
			DIS_SUN_UART_108M_CLK, 0);
		BDEV_WR_F_RB(CLK_SUN_27M_CLK_PM_CTRL,
			DIS_SUN_27M_CLK, 0);
	}
}

#define PM_OPS_DEFINED
static struct brcm_chip_pm_ops chip_pm_ops = {
	.moca.enable		= bcm7408_pm_moca_enable,
	.moca.disable		= bcm7408_pm_moca_disable,
	.usb.enable		= bcm7408_pm_usb_enable,
	.usb.disable		= bcm7408_pm_usb_disable,
	.suspend		= bcm7408_pm_suspend,
	.resume			= bcm7408_pm_resume,
};
#endif

#ifndef PM_OPS_DEFINED
/* default structure - no pm callbacks available */
static struct brcm_chip_pm_ops chip_pm_ops;
#endif

static void brcm_pm_sata_disable(void)
{
	if (chip_pm_ops.sata.disable)
		chip_pm_ops.sata.disable(0);
}

static void brcm_pm_sata_enable(void)
{
	if (chip_pm_ops.sata.enable)
		chip_pm_ops.sata.enable(0);
}

static void brcm_pm_moca_genet_disable(void)
{
	if (chip_pm_ops.moca_genet.disable)
		chip_pm_ops.moca_genet.disable(0);
}

static void brcm_pm_moca_genet_enable(void)
{
	if (chip_pm_ops.moca_genet.enable)
		chip_pm_ops.moca_genet.enable(0);
}

static void brcm_pm_enet_disable(void)
{
	if (chip_pm_ops.genet.disable)
		chip_pm_ops.genet.disable(0);
}

static void brcm_pm_enet_enable(void)
{
	if (chip_pm_ops.genet.enable)
		chip_pm_ops.genet.enable(0);
}

static void brcm_pm_moca_disable(void)
{
	if (chip_pm_ops.moca.disable)
		chip_pm_ops.moca.disable(0);
}

static void brcm_pm_moca_enable(void)
{
	if (chip_pm_ops.moca.enable)
		chip_pm_ops.moca.enable(0);
}

static void brcm_pm_usb_disable(void)
{
	if (chip_pm_ops.usb.disable)
		chip_pm_ops.usb.disable(0);
}

static void brcm_pm_usb_enable(void)
{
	if (chip_pm_ops.usb.enable)
		chip_pm_ops.usb.enable(0);
}

static void brcm_pm_set_ddr_timeout(int val)
{
#if defined(CONFIG_BCM7125) || defined(CONFIG_BCM7420) || \
		defined(CONFIG_BCM7340) || defined(CONFIG_BCM7342)
	if (val) {
		BDEV_WR_F(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
			IDDQ_MODE_ON_SELFREF, 1);
		BDEV_WR_F(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
			HIZ_ON_SELFREF, 1);
		BDEV_WR_F(MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL,
			DEVCLK_OFF_ON_SELFREF, 1);
		BDEV_WR_F(MEMC_DDR23_APHY_AC_0_POWERDOWN,
			PLLCLKS_OFF_ON_SELFREF, 1);
		BDEV_WR_F(MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL,
			IDDQ_MODE_ON_SELFREF, 1);
		BDEV_WR_F(MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL,
			IDDQ_MODE_ON_SELFREF, 1);

		BDEV_WR_F_RB(MEMC_DDR_0_SRPD_CONFIG, INACT_COUNT, 0xdff);
		BDEV_WR_F(MEMC_DDR_0_SRPD_CONFIG, SRPD_EN, 1);
	} else {
		unsigned long flags;

		local_irq_save(flags);
		BDEV_WR_F(MEMC_DDR_0_SRPD_CONFIG, INACT_COUNT, 0xffff);
		do {
			DEV_RD(KSEG1);
		} while (BDEV_RD_F(MEMC_DDR_0_POWER_DOWN_STATUS, SRPD));
		BDEV_WR_F(MEMC_DDR_0_SRPD_CONFIG, SRPD_EN, 0);
		local_irq_restore(flags);
	}
#endif
}

#ifdef CONFIG_BRCM_HAS_STANDBY

/***********************************************************************
 * Passive standby - per-chip
 ***********************************************************************/

static void brcm_system_standby(void)
{
	if (chip_pm_ops.suspend)
		chip_pm_ops.suspend();
}

static void brcm_system_resume(void)
{
	if (chip_pm_ops.resume)
		chip_pm_ops.resume();
}

/***********************************************************************
 * Passive standby - common functions
 ***********************************************************************/

static suspend_state_t suspend_state;

static void brcm_pm_handshake(void)
{
#ifdef CONFIG_BRCM_PWR_HANDSHAKE_V0
	int i;
	unsigned long base = BCHP_BSP_CMDBUF_REG_START & ~0xffff;
	u32 tmp;

	i = 0;
	while (!(BDEV_RD(base + 0xb008) & 0x02)) {
		if (i++ == 10) {
			printk(KERN_WARNING "%s: CMD_IDRY2 timeout\n",
				__func__);
			break;
		}
		msleep(20);
	}
	BDEV_WR_RB(base + 0x7980, 0x00000010);
	BDEV_WR_RB(base + 0x7984, 0x00000098);
	BDEV_WR_RB(base + 0x7988, 0xabcdef00);
	BDEV_WR_RB(base + 0x798c, 0xb055aa4f);
	BDEV_WR_RB(base + 0x7990, 0x789a0004);
	BDEV_WR_RB(base + 0x7994, 0x00000000);

	BDEV_WR_RB(base + 0xb028, 1);

	i = 0;
	while (!(BDEV_RD(base + 0xb020) & 0x01)) {
		if (i++ == 10) {
			printk(KERN_WARNING "%s: CMD_OLOAD2 timeout\n",
				__func__);
			break;
		}
		mdelay(10);
	}

	BDEV_WR_RB(base + 0xb010, 0);
	BDEV_WR_RB(base + 0xb020, 0);
	tmp = BDEV_RD(base + 0x7c94);
	if (tmp != 0 && tmp != 1) {
		printk(KERN_WARNING "%s: command failed: %08lx\n",
			__func__, (unsigned long)tmp);
		mdelay(10);
		return;
	}
	BDEV_UNSET_RB(base + 0xb038, 0xff00);
#endif /* CONFIG_BRCM_PWR_HANDSHAKE_V0 */
}

static int brcm_pm_prepare(void)
{
	DBG("%s:%d\n", __func__, __LINE__);
	return 0;
}

static int brcm_pm_standby(void)
{
	int ret = 0;
	unsigned long restart_vec = BRCM_WARM_RESTART_VEC;
	DBG("%s:%d\n", __func__, __LINE__);

	brcm_irq_standby_enter(BRCM_IRQ_PM);

#ifdef CONFIG_BCM7468
	{
	u32 oldvec[5];
	const int vecsize = 0x14;
	void *vec = (void *)ebase + 0x200;

	restart_vec = (unsigned long)vec;

	memcpy(oldvec, vec, vecsize);
	memcpy(vec, brcm_tp1_int_vec, vecsize);
	flush_icache_range(restart_vec, restart_vec + vecsize);
#else
	/* send all IRQs to BRCM_WARM_RESTART_VEC */
	clear_c0_cause(CAUSEF_IV);
	irq_disable_hazard();
	set_c0_status(ST0_BEV);
	irq_disable_hazard();
#endif

	brcm_system_standby();
	brcm_pm_wakeup_enable();
	if (brcm_pm_standby_flags & BRCM_STANDBY_NO_SLEEP) {
		if (brcm_pm_standby_flags & BRCM_STANDBY_DELAY)
			mdelay(120000);
		else
			mdelay(5000);
	} else {
		do {
			if (brcm_pm_standby_flags & BRCM_STANDBY_TEST) {
				BDEV_WR_RB(BCHP_PM_L2_CPU_MASK_SET, 0xffffffff);
				BDEV_WR_RB(BCHP_PM_L2_CPU_CLEAR, 0xffffffff);
				BDEV_WR_F_RB(PM_L2_CPU_MASK_CLEAR,
					TIMER_INTR, 1);

				BDEV_WR_RB(BCHP_WKTMR_EVENT, 1);
				BDEV_WR_RB(BCHP_WKTMR_ALARM,
					BDEV_RD(BCHP_WKTMR_COUNTER) + 1);
			}
			brcm_pm_handshake();
			ret = brcm_pm_standby_asm(
				current_cpu_data.icache.linesz,
				restart_vec, brcm_pm_standby_flags);
		} while (!brcm_pm_wakeup_poll());
	}
	brcm_pm_wakeup_disable();
	brcm_system_resume();

#ifdef CONFIG_BCM7468
	memcpy(vec, oldvec, vecsize);
	flush_icache_range(restart_vec, restart_vec + vecsize);
	}
#else
	/* send IRQs back to the normal runtime vectors */
	clear_c0_status(ST0_BEV);
	irq_disable_hazard();
	set_c0_cause(CAUSEF_IV);
	irq_disable_hazard();
#endif
	brcm_irq_standby_exit();

	if (ret)
		printk(KERN_WARNING "%s: standby returned %d\n",
			__func__, ret);

	return 0;
}

static int brcm_pm_enter(suspend_state_t unused)
{
	int ret = 0;

	DBG("%s:%d\n", __func__, __LINE__);
	switch (suspend_state) {
	case PM_SUSPEND_STANDBY:
		ret = brcm_pm_standby();
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static void brcm_pm_finish(void)
{
	DBG("%s:%d\n", __func__, __LINE__);
}

/* Hooks to enable / disable UART interrupts during suspend */
static int brcm_pm_begin(suspend_state_t state)
{
	DBG("%s:%d\n", __func__, __LINE__);
	suspend_state = state;
	return 0;
}

static void brcm_pm_end(void)
{
	DBG("%s:%d\n", __func__, __LINE__);
	suspend_state = PM_SUSPEND_ON;
	return;
}

static int brcm_pm_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_STANDBY;
}

static struct platform_suspend_ops brcm_pm_ops = {
	.begin		= brcm_pm_begin,
	.end		= brcm_pm_end,
	.prepare	= brcm_pm_prepare,
	.enter		= brcm_pm_enter,
	.finish		= brcm_pm_finish,
	.valid		= brcm_pm_valid,
};

static int brcm_suspend_init(void)
{
	DBG("%s:%d\n", __func__, __LINE__);
	suspend_set_ops(&brcm_pm_ops);
	return 0;
}
late_initcall(brcm_suspend_init);

#endif /* CONFIG_BRCM_HAS_STANDBY */
