#include <features.h>

	.section .mdebug.abi32
	.previous
	.abicalls
#APP

#ifdef __UCLIBC_PROFILING__

#NO_APP
	.align	2
	.ent	call_gmon_start
	.type	call_gmon_start, @function
call_gmon_start:
	.frame	$sp,32,$31		# vars= 0, regs= 2/0, args= 16, extra= 8
	.mask	0x90000000,-4
	.fmask	0x00000000,0
	.set	noreorder
	.cpload	$25
	.set	reorder
	subu	$sp,$sp,32
	.cprestore 16
	la	$2,__gmon_start__
	sw	$31,28($sp)
	.set	noreorder
	.set	nomacro
	beq	$2,$0,$L3
	sw	$28,24($sp)
	.set	macro
	.set	reorder

	move	$25,$2
	jal	$31,$25
$L3:
	lw	$31,28($sp)
	#nop
	.set	noreorder
	.set	nomacro
	j	$31
	addu	$sp,$sp,32
	.set	macro
	.set	reorder

	.end	call_gmon_start
#APP

#endif

	.section .init
#NO_APP
	.align	2
	.globl	_init
	.ent	_init
	.type	_init, @function
_init:
	.frame	$sp,32,$31		# vars= 0, regs= 2/0, args= 16, extra= 8
	.mask	0x90000000,-4
	.fmask	0x00000000,0
	.set	noreorder
	.cpload	$25
	.set	reorder
	subu	$sp,$sp,32
	.cprestore 16
	sw	$31,28($sp)
	sw	$28,24($sp)
#APP

#ifdef __UCLIBC_PROFILING__

#NO_APP
	la	$25,call_gmon_start
	jal	$31,$25
#APP

#endif
	.align 2
	.end _init
	
	.section .fini
#NO_APP
	.align	2
	.globl	_fini
	.ent	_fini
	.type	_fini, @function
_fini:
	.frame	$sp,32,$31		# vars= 0, regs= 2/0, args= 16, extra= 8
	.mask	0x90000000,-4
	.fmask	0x00000000,0
	.set	noreorder
	.cpload	$25
	.set	reorder
	subu	$sp,$sp,32
	.cprestore 16
	sw	$31,28($sp)
	sw	$28,24($sp)
#APP
	.align 2
	.end _fini
	
	.weak	__gmon_start__
	.ident	"GCC: (GNU) 3.3.6"
