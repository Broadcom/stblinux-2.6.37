#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mipsregs.h>
#include <asm/addrspace.h>
#include <asm/brcmstb/brcmstb.h>

#define REG_OFFSET		(KSEG1 | BCHP_PHYSICAL_OFFSET)
#define REG(x)			(REG_OFFSET + (x))

	.macro dbg_save_val	src
	li 	t1, REG(BCHP_UARTA_SCR);
	sw	src, 0(t1)
	.endm

	.macro aon_write_data index reg
	li	t1, REG(BCHP_AON_CTRL_SYSTEM_DATA_00)
	sw	\reg, \index(t1)
	.endm

	.macro aon_read_data index reg
	li	t1, REG(BCHP_AON_CTRL_SYSTEM_DATA_00)
	lw	\reg, \index(t1)
	.endm

/*	AON data map
	offset		bits
	0:		0: 	0 - cold boot, 1 - warm boot
	4:		0-31:	reentry address
	8:		0-31:	temporary stack pointer
 */
	.text
	.set	noreorder
	.align	5

LEAF(brcm_pm_s3_standby_asm)
# we do not need stack frame, the entire context gets saved
# in a dedicated structure brcm_pm_s3_context
# upon warm reboot the context gets restored from this structure

	la	t0, s3_context
	/* general purpose registers */
	sw	ra, 0(t0)
	sw	s0, 4(t0)
	sw	s1, 8(t0)
	sw	s2, 12(t0)
	sw	s3, 16(t0)
	sw	s4, 20(t0)
	sw	s5, 24(t0)
	sw	s6, 28(t0)
	sw	s7, 32(t0)
	sw	gp, 36(t0)
	sw	sp, 40(t0)
	sw	fp, 44(t0)

	/* CP0 */

	bal	__dputc
	li	a0, 'S'
	bal	__dputc
	li	a0, '3'
	bal	__dputc
	li	a0, ':'
	bal	__dputc
	li	a0, 0x0d
	bal	__dputc
	li	a0, 0x0a

	li	t0, REG(BCHP_AON_CTRL_PM_MIPS_WAIT_COUNT)
	li	t1, 0xffff
	sw	t1, 0(t0)
	lw	zero, 0(t0)

	li	t1, 0x1fff
	li	t0, REG(BCHP_AON_CTRL_PM_S3_STANDBY_TIMER)
	sw	t1, 0(t0)
	lw	zero, 0(t0)
	li	t0, REG(BCHP_AON_CTRL_PM_S3_WAKEUP_TIMER)
	sw	t1, 0(t0)
	lw	t1, 0(t0)

# save cp0 sr
	mfc0	s2, CP0_STATUS

# set 'warm boot' flag
	li	t0, 1
	aon_write_data	0, t0
# save reentry point and stack pointer in AON
	la	t0, s3_reentry
	aon_write_data  4, t0
	aon_write_data	8, sp

#if 1
# deep power down request
pd_request:
	li	t0, REG(BCHP_AON_CTRL_PM_CTRL)
# SET: pm_start_pwrdn, pm_deep_standby, pm_warm_boot, pm_enable_pll_pwrdn
	li	t1, 0x33
	sw	zero, 0(t0)
	lw	zero, 0(t0)
	sw	t1, 0(t0)
	lw	zero, 0(t0)

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
#else
# issue restart
	li	t1, 1
	li	t0, REG(BCHP_SUN_TOP_CTRL_RESET_SOURCE_ENABLE)
	sw	t1, 0(t0)
	lw	zero, 0(t0)
	li	t0, REG(BCHP_SUN_TOP_CTRL_SW_MASTER_RESET)
#	sw	t1, 0(t0)
#	lw	zero, 0(t0)

	/* simulate return from CFE */
	aon_read_data	4, t0
	aon_read_data	8, sp
	jr	t0
	nop

	/* we are never here */
	j 	panic
#endif

/***********************************************************************
 * Bring MEMC back up
 ***********************************************************************/

s3_reentry:
	bal	__dputc
	li	a0, 'M'
	bal	__dputc
	li	a0, 0x0d
	bal	__dputc
	li	a0, 0x0a

# restore sr
	sync
	nop

	mtc0	s2, CP0_STATUS
	nop

	li	v0, 0

s3_standby_exit:

# return to caller
	/* general purpose registers */
	la	t0, s3_context
	lw	ra, 0(t0)
	lw	s0, 4(t0)
	lw	s1, 8(t0)
	lw	s2, 12(t0)
	lw	s3, 16(t0)
	lw	s4, 20(t0)
	lw	s5, 24(t0)
	lw	s6, 28(t0)
	lw	s7, 32(t0)
	lw	gp, 36(t0)
	lw	sp, 40(t0)
	lw	fp, 44(t0)

	jr	ra
	nop
	END(brcm_pm_s3_standby_asm)

#define UART_LSR_OFFSET		0x14
#define UART_LSR_MASK		BCHP_UARTA_LSR_THRE_MASK
#define UART_TX_OFFSET		0x00

__dputc:

	li 	t1, REG(BCHP_UARTA_REG_START)
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
