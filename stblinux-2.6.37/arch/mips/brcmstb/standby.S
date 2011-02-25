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

#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mipsregs.h>
#include <asm/cacheops.h>
#include <asm/brcmstb/brcmstb.h>

#define XTALFREQ		27000000
#define REG_OFFSET		(0xa0000000 | BCHP_PHYSICAL_OFFSET)
#define REG(x)			(REG_OFFSET + (x))
#define UPG_USEC_TICKS		(XTALFREQ / 1000000)

/* debug output - BRCM_STANDBY_VERBOSE */
#define UART_BASE		BCHP_UARTA_REG_START
#define UART_LSR_OFFSET		0x14
#define UART_LSR_MASK		BCHP_UARTA_LSR_THRE_MASK
#define UART_TX_OFFSET		0x00

#define USE_WATCHDOG		0

#define FLAGREG			s5

	.text
	.set	noreorder
	.align	5

/***********************************************************************
 * Lock code into I$
 ***********************************************************************/

# a0 is icache line size
# a1 is the interrupt vector (or general exception base)
# a2 is brcm_pm_standby_flags

# s1 is saved SCB refresh client setting

	LEAF(brcm_pm_standby_asm)
	subu	sp, 64
	sw	ra, 0(sp)
	sw	s0, 4(sp)
	sw	s1, 8(sp)
	sw	s2, 12(sp)
	sw	s3, 16(sp)
	sw	s4, 20(sp)
	sw	s5, 24(sp)
	sw	s6, 28(sp)
	sw	s7, 32(sp)

	move	FLAGREG, a2

	bal	memc_settings_valid
	nop
	bnez	v0, standby_exit
	nop

#ifdef CONFIG_BMIPS5000
	/*
	 * reset jump target buffer and call/return stack, to avoid
	 * unexpected accesses to DRAM
	 */
	li	t0, 0x00040000
	mtc0	t0, $22, 2
	mfc0	t0, $22, 2

	li	t0, 0x00060000
	mtc0	t0, $22, 2
	mfc0	t0, $22, 2

	ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ;
	ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ;
	ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ;
	ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop ; ssnop
#endif

# lock the asm section into the I-cache
	move	s0, a0
	addiu	t1, s0, -1
	not	t1

	la	t0, brcm_pm_standby_asm
	and	t0, t1

	la	t2, passiveIRQ_end
	and	t2, t1

1:	cache	0x1c, 0(t0)
	bne	t0, t2, 1b
	addu	t0, s0

# now lock the interrupt vector
	cache	0x1c, 0(a1)

	bal	dputc
	li	a0, 's'

/***********************************************************************
 * Flush memory transactions and put MEMC into standby
 ***********************************************************************/

	sync

#if !defined(CONFIG_BRCM_PWR_HANDSHAKE)
	li	t0, REG(BCHP_MEMC_DDR_0_SSPD_CMD)
	li	t1, 1
	sw	t1, 0(t0)

	li	t0, REG(BCHP_MEMC_DDR_0_POWER_DOWN_STATUS)
1:	lw	t1, 0(t0)
	andi	t1, 4
	beqz	t1, 1b
	nop
#endif

	/* delay if requested by the user: 4 * 30s */
	li	t0, BRCM_STANDBY_DELAY
	and	t0, FLAGREG
	beqz	t0, 2f
	nop

	bal	dputc
	li	a0, 'w'

	li	s1, 4
1:	li	a0, (27000000 * 30)
	bal	delay
	addiu	s1, -1
	bnez	s1, 1b
	nop

	bal	dputc
	li	a0, 'W'

# power down request

2:
	li	t0, REG(BCHP_SUN_TOP_CTRL_PM_CTRL)
#if defined(CONFIG_BMIPS5000) && !defined(CONFIG_BCM7420C0)
	li	t1, 0x0f
#else
	li	t1, 0x0b
#endif
	sw	zero, 0(t0)
	lw	zero, 0(t0)
	sw	t1, 0(t0)
	lw	t1, 0(t0)

/***********************************************************************
 * Wait for interrupt
 ***********************************************************************/

wait_loop:
# enable int2 and then wait for an interrupt
	mfc0	t0, CP0_STATUS
# save cp0 sr
	move	s2, t0

	li	t1, ~(ST0_IM | ST0_IE)
	and	t0, t1
	ori	t0, STATUSF_IP2
	mtc0	t0, CP0_STATUS
	nop
	nop
	nop
	ori	t0, ST0_IE
	mtc0	t0, CP0_STATUS
	wait
	nop

/***********************************************************************
 * Bring MEMC back up
 ***********************************************************************/

	bal	dputc
	li	a0, 'M'

#if USE_WATCHDOG
# arm the watchdog timer
	li	t0, REG(BCHP_TIMER_WDTIMEOUT)
	li	t1, 3 * 27000000
	sw	t1, 0(t0)
	lw	t1, 0(t0)

	li	t0, REG(BCHP_TIMER_WDCMD)
	li	t1, 0xff00
	sw	t1, 0(t0)
	lw	t1, 0(t0)
	li	t1, 0x00ff
	sw	t1, 0(t0)
	lw	t1, 0(t0)
#endif

	bal	wake_up_memc_0
	nop

	bal	dputc
	li	a0, 'm'

#ifdef CONFIG_BRCM_PWR_HANDSHAKE_V0
# disarm the BSP
	li	t0, REG(BCHP_BSP_GLB_CONTROL_REG_START + 0x38)
	lw	t1, 0(t0)
	ori	t1, 0xff00
	xori	t1, 0xfe00
	sw	t1, 0(t0)

	bal	dputc
	li	a0, 'B'

	li	t0, REG(BCHP_SUN_TOP_CTRL_PM_CTRL)
	li	t2, BCHP_SUN_TOP_CTRL_PM_CTRL_pm_bsp_ready_for_pwrdn_MASK
1:
	lw	t1, 0(t0)
	and	t1, t2
	bnez	t1, 1b
	nop

	bal	dputc
	li	a0, 'b'
#endif /* CONFIG_BRCM_PWR_HANDSHAKE_V0 */

	li	t0, REG(BCHP_SUN_TOP_CTRL_PM_CTRL)
	sw	zero, 0(t0)
	lw	zero, 0(t0)

/***********************************************************************
 * Unlock I$ lines and resume execution
 ***********************************************************************/

	sync

rtn_from_wait:
	bal	dputc
	li	a0, 'S'

# unlock I$ lines
	addiu	t1, s0, -1
	not	t1

	la	t0, brcm_pm_standby_asm
	and	t0, t1

	la	t2, passiveIRQ_end
	and	t2, t1

1:	cache	Hit_Invalidate_I, 0(t0)
	bne	t0, t2, 1b
	addu	t0, s0

	bal	dputc
	li	a0, 'L'
	bal	dputc
	li	a0, 0x0d
	bal	dputc
	li	a0, 0x0a

# restore sr
	sync
	nop

	mtc0	s2, CP0_STATUS
	nop

	li	v0, 0

standby_exit:

# return to caller
	lw	s7, 32(sp)
	lw	s6, 28(sp)
	lw	s5, 24(sp)
	lw	s4, 20(sp)
	lw	s3, 16(sp)
	lw	s2, 12(sp)
	lw	s1, 8(sp)
	lw	s0, 4(sp)
	lw	ra, 0(sp)
	addiu	sp, 64

	jr	ra
	nop
	END(brcm_pm_standby_asm)

# debug character output

dputc:
	/* INPUT: char in a0 */
	/* USES: t1, t2 */

	li	t1, BRCM_STANDBY_VERBOSE
	and	t1, FLAGREG
	bnez	t1, 1f
	nop
	jr	ra
	nop

1:
	li 	t1, REG(UART_BASE)
1:
	lw 	t2, UART_LSR_OFFSET(t1)
	andi	t2, UART_LSR_MASK
	beqz	t2, 1b
	nop

	sw	a0, UART_TX_OFFSET(t1)

1:	lw 	t2, UART_LSR_OFFSET(t1)
	andi	t2, UART_LSR_MASK
	beqz	t2, 1b
	nop
	jr	ra
	nop

/***********************************************************************
 * Delay loop
 ***********************************************************************/

delay:
# need to reset TIMER1 since it is not normally used
	li	t2, 0x0
	li	t1, REG(BCHP_TIMER_TIMER1_CTRL)
	sw	t2, 0(t1)
	lw	t2, 0(t1)

	li	t2, 0xbfffffff
	sw	t2, 0(t1)
	lw	t2, 0(t1)

	li	t2, 0x3fffffff
	li	t1, REG(BCHP_TIMER_TIMER1_STAT)
	lw	t0, 0(t1)
	and	t0, t2
	add	t0, a0
	not	t2
	and	t2, t0
	bnez	t2, delay_overflow
	nop

# wait for timer value (t2) to exceed expiry time (t0)
1:	lw	t2, 0(t1)
	sltu	t2, t2, t0
	bnez	t2, 1b
	nop

	jr	ra
	nop

# timer1 overflow (this should never happen)
delay_overflow:
	move	t4, ra
	bal	dputc
	li	a0, 'O'

	jr	t4
	nop

/***********************************************************************
 * PM IRQ handler
 ***********************************************************************/

	LEAF(brcm_pm_irq)

# Block interrupts and then return to the wait loop
	mfc0	k0, CP0_STATUS
	li	k1, ~ST0_IE
	and	k0, k1
	mtc0	k0, CP0_STATUS
	eret
	nop

	END(brcm_pm_irq)

/***********************************************************************
 * MEMC wakeup
 ***********************************************************************/

#ifdef CONFIG_BRCM_HAS_ANALOG_DDR_PHY

#define SLEEP_1_MILLI_SEC		27000

.globl	upg_sleep
.ent	upg_sleep
upg_sleep:

	li	t0, REG(BCHP_TIMER_TIMER0_CTRL)
	lw	t1, 0(t0)

	/* TIMER_TIMER0_CTRL.MODE  = 1 => count down mode */
	li	t2, BCHP_TIMER_TIMER0_CTRL_MODE_MASK
	or	t1, t1, t2

	/* TIMER_TIMER0_CTRL.TIMEOUT_VAL = a0 */

	li	t2, BCHP_TIMER_TIMER0_CTRL_TIMEOUT_VAL_MASK
	li	t3, BCHP_TIMER_TIMER0_CTRL_TIMEOUT_VAL_SHIFT

	not	t2, t2
	and	t1, t1, t2
	sll	a0, a0, t3
	or	t1, t1, a0

	sw	t1, 0(t0)

	li	t0, REG(BCHP_TIMER_TIMER_IS)
	lw	t1, 0(t0)

	/* TIMER_TIMER_IS.TMR0TO = 1 => clears this bit, resets time count */

	#li	t2, ~BCHP_TIMER_TIMER_IS_TMR0TO_MASK
	#and	t1, t1, t2
	sw	t1, 0(t0)

	/* TIMER_TIMER0_CTRL.ENA = 1 => start counting */

	li	t0, REG(BCHP_TIMER_TIMER0_CTRL)
	lw	t1, 0(t0)

	li	t2, BCHP_TIMER_TIMER0_CTRL_ENA_MASK
	or	t1, t1, t2
	sw	t1, 0(t0)

	li	t0, REG(BCHP_TIMER_TIMER_IS)
	li	t2, BCHP_TIMER_TIMER_IS_TMR0TO_MASK

1:	lw	t1, 0(t0)
	and	t1, t1, t2
	beq	zero, t1, 1b
	nop

	/* TIMER_TIMER0_CTRL.ENA = 0 => stop counting */

	li	t0, REG(BCHP_TIMER_TIMER0_CTRL)
	lw	t1, 0(t0)
	li	t2, ~BCHP_TIMER_TIMER0_CTRL_ENA_MASK
	and	t1, t1, t2
	sw	t1, 0(t0)

	jr	ra
	nop

.end	upg_sleep

	LEAF(wake_up_memc_0)

	move	s7, ra

	li	a0, 0
	bal	set_memc_0_write_dqs_phase_cntl_direct
	nop

	li	a0, 0
	bal	set_memc_0_write_dq_phase_cntl_direct
	nop

	li	a0, 0
	bal	set_memc_0_pll_ch2_clk_phase_cntrl
	nop

	li	a0, 0
	bal	set_memc_0_deskew_bypass_phase
	nop

	li	a0, REG(BCHP_MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL_DEVCLK_OFF_ON_SELFREF_MASK | BCHP_MEMC_DDR23_APHY_AC_0_DDR_PAD_CNTRL_IDDQ_MODE_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)


	li	a0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_WL0_0_DDR_PAD_CNTRL_IDDQ_MODE_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)

	li	a0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_WL1_0_DDR_PAD_CNTRL_IDDQ_MODE_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)


	li	a0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_WL0_0_WORDSLICE_CNTRL_1_PWRDN_DLL_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)

	li	a0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_WL1_0_WORDSLICE_CNTRL_1_PWRDN_DLL_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)

	li	a0, REG(BCHP_MEMC_DDR23_APHY_AC_0_POWERDOWN)
	lw	a1, 0(a0)
	li	a2, ~(BCHP_MEMC_DDR23_APHY_AC_0_POWERDOWN_PLLCLKS_OFF_ON_SELFREF_MASK)
	and 	a1, a1, a2
	sw	a1, 0(a0)

	bal	reprogram_ddr_pll
	nop

	bal	reset_ws_new
	nop

	jr 	s7
	nop

	END(wake_up_memc_0)

.global reprogram_ddr_pll
.ent reprogram_ddr_pll
reprogram_ddr_pll:

	move	s6, ra

	/* reset the freq divider */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, BCHP_MEMC_DDR23_APHY_AC_0_RESET_FREQ_DIV_RESET_MASK
	lw	a0, 0(v0)
	or	a0, a0, v1
	sw	a0, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	/* reset the vcxo */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, BCHP_MEMC_DDR23_APHY_AC_0_RESET_VCXO_RESET_MASK
	lw	a0, 0(v0)
	or	a0, a0, v1
	sw	a0, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	/* reset DATAPATH_216, RD_DATAPATH_RESET, RESET_DATAPATH_DDR  */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, (BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_216_RESET_MASK | \
		     BCHP_MEMC_DDR23_APHY_AC_0_RESET_RD_DATAPATH_RESET_MASK | \
		     BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_DDR_RESET_MASK)
	lw	a0, 0(v0)
	or	a0, a0, v1
	sw	a0, 0(v0)

	/* de-assert reset from vcxo */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, ~BCHP_MEMC_DDR23_APHY_AC_0_RESET_VCXO_RESET_MASK
	lw	a0, 0(v0)
	and	a0, a0, v1
	sw	a0, 0(v0)

	/* de-assert reset from freq divider */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, ~BCHP_MEMC_DDR23_APHY_AC_0_RESET_FREQ_DIV_RESET_MASK
	lw	a0, 0(v0)
	and	a0, a0, v1
	sw	a0, 0(v0)

	/* wait for 1ms for stable clock */
	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	/* reset the word slice dll */

	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_WORD_SLICE_DLL_RESET)
	li	v1, 1
	sw	v1, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_WORD_SLICE_DLL_RESET)
	sw	v1, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_WORD_SLICE_DLL_RESET)
	li	v1, 0
	sw	v1, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_WORD_SLICE_DLL_RESET)
	sw	v1, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	/* de-assert reset from DATAPATH_216 and DATAPATH_DDR */
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	lw	v1, 0(v0)


	/* de-assert reset from DATAPATH_216_RESET */
	li	a0, ~(BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_216_RESET_MASK | \
		    BCHP_MEMC_DDR23_APHY_AC_0_RESET_RD_DATAPATH_RESET_MASK | \
		    BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_DDR_RESET_MASK)
	and	a0, v1, a0
	sw	a0, 0(v0)

	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA4)
	lw	a0, 0(a1)

	bal	set_memc_0_wl0_dq_phase
	nop

	/* set wl1_dq phase */
	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA5)
	lw	a0, 0(a1)

	bal	set_memc_0_wl1_dq_phase
	nop

	/* set ch2 phase */
	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA6)
	lw	a0, 0(a1)

	move	a1, a0
	and	a0, a0, zero
1:
	bal	set_memc_0_pll_ch2_clk_phase_cntrl
	nop
	add	a0, a0, 1

	bne	a0, a1, 1b
	nop

	/* set ch6 phase */
	li	a1, REG(BCHP_MEMC_MISC_0_SCRATCH_0)
	lw	a0, 0(a1)

	move	a1, a0
	and	a0, a0, zero
1:
	bal	set_memc_0_deskew_bypass_phase
	nop

	add	a0, a0, 1

	bne	a0, a1, 1b
	nop

	/* set wl0_dqs0 phases */
	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA0)
	lw	a0, 0(a1)

	bal	set_memc_0_dqs0_phase
	nop


	/* set wl0_dqs1 phases */

	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA1)
	lw	a0, 0(a1)

	bal	set_memc_0_dqs1_phase
	nop

	/* set wl1_dqs0 phases */

	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA2)
	lw	a0, 0(a1)

	bal	set_memc_0_dqs2_phase
	nop
	/* set wl1_dqs1 phases */

	li	a1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA3)
	lw	a0, 0(a1)

	bal	set_memc_0_dqs3_phase
	nop

	/* check for pll lock */

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_DDR_PLL_LOCK_STATUS)
	li	a0, BCHP_MEMC_DDR23_APHY_AC_0_DDR_PLL_LOCK_STATUS_LOCK_STATUS_MASK
1:
	lw	v1, 0(v0)
	and	v1, v1, a0

	beqz	v1, 1b
	nop

	jr	s6
	nop

.end reprogram_ddr_pll


.global reset_ws_new
.ent reset_ws_new
reset_ws_new:

	move	s6, ra

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_216_RESET_MASK
	lw	a0, 0(v0)
	or	a0, a0, v1
	sw	a0, 0(v0)

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	li	v1, BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_DDR_RESET_MASK
	lw	a0, 0(v0)
	or	a0, a0, v1
	sw	a0, 0(v0)

	/* reset the word slice dll */

	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_WORD_SLICE_DLL_RESET)
	li	v1, 1
	sw	v1, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_WORD_SLICE_DLL_RESET)
	sw	v1, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL0_0_WORD_SLICE_DLL_RESET)
	li	v1, 0
	sw	v1, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_WL1_0_WORD_SLICE_DLL_RESET)
	sw	v1, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	/* de-assert reset from DATAPATH_216 and DATAPATH_DDR */

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	lw	v1, 0(v0)

	/* de-assert reset from DATAPATH_216_RESET */

	li	a0, ~(BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_216_RESET_MASK)
	and	a0, v1, a0
	sw	a0, 0(v0)


	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_RESET)
	lw	v1, 0(v0)
	li	a0, ~(BCHP_MEMC_DDR23_APHY_AC_0_RESET_DATAPATH_DDR_RESET_MASK)
	and	a0, v1, a0
	sw	a0, 0(v0)

	li	a0, (SLEEP_1_MILLI_SEC)
	bal	upg_sleep
	nop

	jr	s6
	nop

.end reset_ws_new

LEAF(set_memc_0_write_dqs_phase_cntl_direct)

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH4_WL0_DQS0_PHASE_CNTRL)
	sw	a0, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH5_WL0_DQS1_PHASE_CNTRL)
	sw	a0, 0(v0)

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH8_WL1_DQS0_PHASE_CNTRL)
	sw	a0, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH9_WL1_DQS1_PHASE_CNTRL)
	sw	a0, 0(v0)

	jr	ra
	nop

END(set_memc_0_write_dqs_phase_cntl_direct)

LEAF(set_memc_0_write_dq_phase_cntl_direct)

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH3_WL0_DQ_PHASE_CNTRL)
	sw	a0, 0(v0)
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH7_WL1_DQ_PHASE_CNTRL)
	sw	a0, 0(v0)

	jr	ra
	nop

END(set_memc_0_write_dq_phase_cntl_direct)

LEAF(set_memc_0_pll_ch2_clk_phase_cntrl)

	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH2_CLOCK_PHASE_CNTRL)
	sw	a0, 0(v0)

	jr	ra
	nop
END(set_memc_0_pll_ch2_clk_phase_cntrl)

LEAF(set_memc_0_deskew_bypass_phase)

	li	t8, REG(BCHP_MEMC_DDR23_APHY_AC_0_DESKEW_DLL_CNTRL)
	lw	t7, 0(t8)

	li	t6, ~BCHP_MEMC_DDR23_APHY_AC_0_DESKEW_DLL_CNTRL_BYPASS_PHASE_MASK
	and	t7, t7, t6

	li	t6, BCHP_MEMC_DDR23_APHY_AC_0_DESKEW_DLL_CNTRL_BYPASS_PHASE_SHIFT
	sll	t5, a0, t6
	or	t7, t7, t5
	sw	t7, 0(t8)

	jr	ra
	nop
END(set_memc_0_deskew_bypass_phase)

LEAF(set_memc_0_wl0_dq_phase)

	addi	a0, a0, 1
	and	a1, a1, zero
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH3_WL0_DQ_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	addi	a1, a1, 1
	bne	a1, a0, 2b
	nop

	jr	ra
	nop
END(set_memc_0_wl0_dq_phase)

LEAF(set_memc_0_wl1_dq_phase)

	addi	a0, a0, 1
	and	a1, a1, zero
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH7_WL1_DQ_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	addi	a1, a1, 1
	bne	a1, a0, 2b
	nop

	jr	ra
	nop
END(set_memc_0_wl1_dq_phase)


LEAF(set_memc_0_dqs0_phase)

	li	a2, -1
	blt	a0, zero, 1f
	nop

	li	a2, 1
1:
	and	a1, a1, zero
	add	a0, a0, a2
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH4_WL0_DQS0_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	add	a1, a1, a2
	bne	a1, a0, 2b
	nop

	jr	ra
	nop

END(set_memc_0_dqs0_phase)

LEAF(set_memc_0_dqs1_phase)

	li	a2, -1
	blt	a0, zero, 1f
	nop

	li	a2, 1
1:
	and	a1, a1, zero
	add	a0, a0, a2
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH5_WL0_DQS1_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	add	a1, a1, a2
	bne	a1, a0, 2b
	nop

	jr	ra
	nop


END(set_memc_0_dqs1_phase)

LEAF(set_memc_0_dqs2_phase)

	li	a2, -1
	blt	a0, zero, 1f
	nop

	li	a2, 1
1:
	and	a1, a1, zero
	add	a0, a0, a2
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH8_WL1_DQS0_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	add	a1, a1, a2
	bne	a1, a0, 2b
	nop

	jr	ra
	nop

END(set_memc_0_dqs2_phase)

LEAF(set_memc_0_dqs3_phase)

	li	a2, -1
	blt	a0, zero, 1f
	nop

	li	a2, 1
1:
	and	a1, a1, zero
	add	a0, a0, a2
	li	v0, REG(BCHP_MEMC_DDR23_APHY_AC_0_PLL_CH9_WL1_DQS1_PHASE_CNTRL)
2:
	sw	a1, 0(v0)
	add	a1, a1, a2
	bne	a1, a0, 2b
	nop

	jr	ra
	nop

END(set_memc_0_dqs3_phase)

LEAF(memc_settings_valid)

	li	t2, 0xdeadbeef
	li	t1, REG(BCHP_MEMC_GEN_0_MSA_WR_DATA7)
	lw	t0, 0(t1)
	li	v0, 0
	beq	t0, t2, 1f
	nop
	li	v0, -1

1:	jr	ra
	nop

END(memc_settings_valid)

#endif /* CONFIG_BRCM_HAS_ANALOG_DDR_PHY */

#ifdef CONFIG_BRCM_HAS_DIGITAL_DDR_PHY

LEAF(memc_settings_valid)

1:	jr	ra
	li	v0, 0

END(memc_settings_valid)

LEAF(wake_up_memc_0)

#if !defined(CONFIG_BRCM_PWR_HANDSHAKE)
	li	t0, REG(BCHP_MEMC_DDR_0_SSPD_CMD)
	li	t1, 0
	sw	t1, 0(t0)
#endif

	li	t0, REG(BCHP_DDR23_PHY_CONTROL_REGS_0_PLL_STATUS)

1:	lw	t1, 0(t0)
	andi	t1, 1
	beqz	t1, 1b
	nop

	jr	ra
	nop

END(wake_up_memc_0)

#endif /* CONFIG_BRCM_HAS_DIGITAL_DDR_PHY */

	.globl passiveIRQ_end
passiveIRQ_end:
	nop
