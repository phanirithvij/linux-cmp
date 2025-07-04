/*
 *  arch/ppc64/kernel/entry.S
 *
 *  PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *  Rewritten by Cort Dougan (cort@cs.nmt.edu) for PReP
 *    Copyright (C) 1996 Cort Dougan <cort@cs.nmt.edu>
 *  Adapted for Power Macintosh by Paul Mackerras.
 *  Low-level exception handlers and MMU support
 *  rewritten by Paul Mackerras.
 *    Copyright (C) 1996 Paul Mackerras.
 *  MPC8xx modifications Copyright (C) 1997 Dan Malek (dmalek@jlc.net).
 *
 *  This file contains the system call entry code, context switch
 *  code, and exception/interrupt return code for PowerPC.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <asm/unistd.h>
#include <asm/processor.h>
#include <asm/page.h>
#include <asm/mmu.h>
#include <asm/thread_info.h>
#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/cputable.h>

#ifdef CONFIG_PPC_ISERIES
#define DO_SOFT_DISABLE
#endif

/*
 * System calls.
 */
	.section	".toc","aw"
.SYS_CALL_TABLE:
	.tc .sys_call_table[TC],.sys_call_table

/* This value is used to mark exception frames on the stack. */
exception_marker:
	.tc	ID_72656773_68657265[TC],0x7265677368657265

	.section	".text"
	.align 7

#undef SHOW_SYSCALLS

	.globl system_call_common
system_call_common:
	andi.	r10,r12,MSR_PR
	mr	r10,r1
	addi	r1,r1,-INT_FRAME_SIZE
	beq-	1f
	ld	r1,PACAKSAVE(r13)
1:	std	r10,0(r1)
	std	r11,_NIP(r1)
	std	r12,_MSR(r1)
	std	r0,GPR0(r1)
	std	r10,GPR1(r1)
	std	r2,GPR2(r1)
	std	r3,GPR3(r1)
	std	r4,GPR4(r1)
	std	r5,GPR5(r1)
	std	r6,GPR6(r1)
	std	r7,GPR7(r1)
	std	r8,GPR8(r1)
	li	r11,0
	std	r11,GPR9(r1)
	std	r11,GPR10(r1)
	std	r11,GPR11(r1)
	std	r11,GPR12(r1)
	std	r9,GPR13(r1)
	crclr	so
	mfcr	r9
	mflr	r10
	li	r11,0xc01
	std	r9,_CCR(r1)
	std	r10,_LINK(r1)
	std	r11,_TRAP(r1)
	mfxer	r9
	mfctr	r10
	std	r9,_XER(r1)
	std	r10,_CTR(r1)
	std	r3,ORIG_GPR3(r1)
	ld	r2,PACATOC(r13)
	addi	r9,r1,STACK_FRAME_OVERHEAD
	ld	r11,exception_marker@toc(r2)
	std	r11,-16(r9)		/* "regshere" marker */
#ifdef CONFIG_PPC_ISERIES
	/* Hack for handling interrupts when soft-enabling on iSeries */
	cmpdi	cr1,r0,0x5555		/* syscall 0x5555 */
	andi.	r10,r12,MSR_PR		/* from kernel */
	crand	4*cr0+eq,4*cr1+eq,4*cr0+eq
	beq	hardware_interrupt_entry
	lbz	r10,PACAPROCENABLED(r13)
	std	r10,SOFTE(r1)
#endif
	mfmsr	r11
	ori	r11,r11,MSR_EE
	mtmsrd	r11,1

#ifdef SHOW_SYSCALLS
	bl	.do_show_syscall
	REST_GPR(0,r1)
	REST_4GPRS(3,r1)
	REST_2GPRS(7,r1)
	addi	r9,r1,STACK_FRAME_OVERHEAD
#endif
	clrrdi	r11,r1,THREAD_SHIFT
	ld	r10,TI_FLAGS(r11)
	andi.	r11,r10,_TIF_SYSCALL_T_OR_A
	bne-	syscall_dotrace
syscall_dotrace_cont:
	cmpldi	0,r0,NR_syscalls
	bge-	syscall_enosys

system_call:			/* label this so stack traces look sane */
/*
 * Need to vector to 32 Bit or default sys_call_table here,
 * based on caller's run-mode / personality.
 */
	ld	r11,.SYS_CALL_TABLE@toc(2)
	andi.	r10,r10,_TIF_32BIT
	beq	15f
	addi	r11,r11,8	/* use 32-bit syscall entries */
	clrldi	r3,r3,32
	clrldi	r4,r4,32
	clrldi	r5,r5,32
	clrldi	r6,r6,32
	clrldi	r7,r7,32
	clrldi	r8,r8,32
15:
	slwi	r0,r0,4
	ldx	r10,r11,r0	/* Fetch system call handler [ptr] */
	mtctr   r10
	bctrl			/* Call handler */

syscall_exit:
	std	r3,RESULT(r1)
#ifdef SHOW_SYSCALLS
	bl	.do_show_syscall_exit
	ld	r3,RESULT(r1)
#endif
	clrrdi	r12,r1,THREAD_SHIFT

	/* disable interrupts so current_thread_info()->flags can't change,
	   and so that we don't get interrupted after loading SRR0/1. */
	ld	r8,_MSR(r1)
	andi.	r10,r8,MSR_RI
	beq-	unrecov_restore
	mfmsr	r10
	rldicl	r10,r10,48,1
	rotldi	r10,r10,16
	mtmsrd	r10,1
	ld	r9,TI_FLAGS(r12)
	li	r11,-_LAST_ERRNO
	andi.	r0,r9,(_TIF_SYSCALL_T_OR_A|_TIF_SINGLESTEP|_TIF_SIGPENDING|_TIF_NEED_RESCHED|_TIF_RESTOREALL|_TIF_SAVE_NVGPRS|_TIF_NOERROR|_TIF_RESTORE_SIGMASK)
	bne-	syscall_exit_work
	cmpld	r3,r11
	ld	r5,_CCR(r1)
	bge-	syscall_error
syscall_error_cont:
	ld	r7,_NIP(r1)
	stdcx.	r0,0,r1			/* to clear the reservation */
	andi.	r6,r8,MSR_PR
	ld	r4,_LINK(r1)
	beq-	1f			/* only restore r13 if */
	ld	r13,GPR13(r1)		/* returning to usermode */
1:	ld	r2,GPR2(r1)
	li	r12,MSR_RI
	andc	r11,r10,r12
	mtmsrd	r11,1			/* clear MSR.RI */
	ld	r1,GPR1(r1)
	mtlr	r4
	mtcr	r5
	mtspr	SPRN_SRR0,r7
	mtspr	SPRN_SRR1,r8
	rfid
	b	.	/* prevent speculative execution */

syscall_error:	
	oris	r5,r5,0x1000	/* Set SO bit in CR */
	neg	r3,r3
	std	r5,_CCR(r1)
	b	syscall_error_cont
	
/* Traced system call support */
syscall_dotrace:
	bl	.save_nvgprs
	addi	r3,r1,STACK_FRAME_OVERHEAD
	bl	.do_syscall_trace_enter
	ld	r0,GPR0(r1)	/* Restore original registers */
	ld	r3,GPR3(r1)
	ld	r4,GPR4(r1)
	ld	r5,GPR5(r1)
	ld	r6,GPR6(r1)
	ld	r7,GPR7(r1)
	ld	r8,GPR8(r1)
	addi	r9,r1,STACK_FRAME_OVERHEAD
	clrrdi	r10,r1,THREAD_SHIFT
	ld	r10,TI_FLAGS(r10)
	b	syscall_dotrace_cont

syscall_enosys:
	li	r3,-ENOSYS
	b	syscall_exit
	
syscall_exit_work:
	/* If TIF_RESTOREALL is set, don't scribble on either r3 or ccr.
	 If TIF_NOERROR is set, just save r3 as it is. */

	andi.	r0,r9,_TIF_RESTOREALL
	bne-	2f
	cmpld	r3,r11		/* r10 is -LAST_ERRNO */
	blt+	1f
	andi.	r0,r9,_TIF_NOERROR
	bne-	1f
	ld	r5,_CCR(r1)
	neg	r3,r3
	oris	r5,r5,0x1000	/* Set SO bit in CR */
	std	r5,_CCR(r1)
1:	std	r3,GPR3(r1)
2:	andi.	r0,r9,(_TIF_PERSYSCALL_MASK)
	beq	4f

	/* Clear per-syscall TIF flags if any are set, but _leave_
	_TIF_SAVE_NVGPRS set in r9 since we haven't dealt with that
	yet.  */

	li	r11,_TIF_PERSYSCALL_MASK
	addi	r12,r12,TI_FLAGS
3:	ldarx	r10,0,r12
	andc	r10,r10,r11
	stdcx.	r10,0,r12
	bne-	3b
	subi	r12,r12,TI_FLAGS
	
4:	bl	.save_nvgprs
	/* Anything else left to do? */
	andi.	r0,r9,(_TIF_SYSCALL_T_OR_A|_TIF_SINGLESTEP|_TIF_SAVE_NVGPRS)
	beq	.ret_from_except_lite

	/* Re-enable interrupts */
	mfmsr	r10
	ori	r10,r10,MSR_EE
	mtmsrd	r10,1

	andi.	r0,r9,_TIF_SAVE_NVGPRS
	bne	save_user_nvgprs

	/* If tracing, re-enable interrupts and do it */
save_user_nvgprs_cont:	
	andi.	r0,r9,(_TIF_SYSCALL_T_OR_A|_TIF_SINGLESTEP)
	beq	5f
	
	addi	r3,r1,STACK_FRAME_OVERHEAD
	bl	.do_syscall_trace_leave
	REST_NVGPRS(r1)
	clrrdi	r12,r1,THREAD_SHIFT

	/* Disable interrupts again and handle other work if any */
5:	mfmsr	r10
	rldicl	r10,r10,48,1
	rotldi	r10,r10,16
	mtmsrd	r10,1

	b	.ret_from_except_lite

/* Save non-volatile GPRs, if not already saved. */
_GLOBAL(save_nvgprs)
	ld	r11,_TRAP(r1)
	andi.	r0,r11,1
	beqlr-
	SAVE_NVGPRS(r1)
	clrrdi	r0,r11,1
	std	r0,_TRAP(r1)
	blr


save_user_nvgprs:
	ld	r10,TI_SIGFRAME(r12)
	andi.	r0,r9,_TIF_32BIT
	beq-	save_user_nvgprs_64

	/* 32-bit save to userspace */

.macro savewords start, end
  1:	stw \start,4*(\start)(r10)
	.section __ex_table,"a"
	.align	3
	.llong	1b,save_user_nvgprs_fault
	.previous
	.if \end - \start
	savewords "(\start+1)",\end
	.endif
.endm	
	savewords 14,31
	b	save_user_nvgprs_cont

save_user_nvgprs_64:
	/* 64-bit save to userspace */

.macro savelongs start, end
  1:	std \start,8*(\start)(r10)
	.section __ex_table,"a"
	.align	3
	.llong	1b,save_user_nvgprs_fault
	.previous
	.if \end - \start
	savelongs "(\start+1)",\end
	.endif
.endm	
	savelongs 14,31
	b	save_user_nvgprs_cont

save_user_nvgprs_fault:
	li	r3,11		/* SIGSEGV */
	ld	r4,TI_TASK(r12)
	bl	.force_sigsegv

	clrrdi	r12,r1,THREAD_SHIFT
	ld	r9,TI_FLAGS(r12)
	b	save_user_nvgprs_cont
	
/*
 * The sigsuspend and rt_sigsuspend system calls can call do_signal
 * and thus put the process into the stopped state where we might
 * want to examine its user state with ptrace.  Therefore we need
 * to save all the nonvolatile registers (r14 - r31) before calling
 * the C code.  Similarly, fork, vfork and clone need the full
 * register state on the stack so that it can be copied to the child.
 */

_GLOBAL(ppc_fork)
	bl	.save_nvgprs
	bl	.sys_fork
	b	syscall_exit

_GLOBAL(ppc_vfork)
	bl	.save_nvgprs
	bl	.sys_vfork
	b	syscall_exit

_GLOBAL(ppc_clone)
	bl	.save_nvgprs
	bl	.sys_clone
	b	syscall_exit

_GLOBAL(ret_from_fork)
	bl	.schedule_tail
	REST_NVGPRS(r1)
	li	r3,0
	b	syscall_exit

/*
 * This routine switches between two different tasks.  The process
 * state of one is saved on its kernel stack.  Then the state
 * of the other is restored from its kernel stack.  The memory
 * management hardware is updated to the second process's state.
 * Finally, we can return to the second process, via ret_from_except.
 * On entry, r3 points to the THREAD for the current task, r4
 * points to the THREAD for the new task.
 *
 * Note: there are two ways to get to the "going out" portion
 * of this code; either by coming in via the entry (_switch)
 * or via "fork" which must set up an environment equivalent
 * to the "_switch" path.  If you change this you'll have to change
 * the fork code also.
 *
 * The code which creates the new task context is in 'copy_thread'
 * in arch/ppc64/kernel/process.c
 */
	.align	7
_GLOBAL(_switch)
	mflr	r0
	std	r0,16(r1)
	stdu	r1,-SWITCH_FRAME_SIZE(r1)
	/* r3-r13 are caller saved -- Cort */
	SAVE_8GPRS(14, r1)
	SAVE_10GPRS(22, r1)
	mflr	r20		/* Return to switch caller */
	mfmsr	r22
	li	r0, MSR_FP
#ifdef CONFIG_ALTIVEC
BEGIN_FTR_SECTION
	oris	r0,r0,MSR_VEC@h	/* Disable altivec */
	mfspr	r24,SPRN_VRSAVE	/* save vrsave register value */
	std	r24,THREAD_VRSAVE(r3)
END_FTR_SECTION_IFSET(CPU_FTR_ALTIVEC)
#endif /* CONFIG_ALTIVEC */
	and.	r0,r0,r22
	beq+	1f
	andc	r22,r22,r0
	mtmsrd	r22
	isync
1:	std	r20,_NIP(r1)
	mfcr	r23
	std	r23,_CCR(r1)
	std	r1,KSP(r3)	/* Set old stack pointer */

#ifdef CONFIG_SMP
	/* We need a sync somewhere here to make sure that if the
	 * previous task gets rescheduled on another CPU, it sees all
	 * stores it has performed on this one.
	 */
	sync
#endif /* CONFIG_SMP */

	addi	r6,r4,-THREAD	/* Convert THREAD to 'current' */
	std	r6,PACACURRENT(r13)	/* Set new 'current' */

	ld	r8,KSP(r4)	/* new stack pointer */
BEGIN_FTR_SECTION
	clrrdi	r6,r8,28	/* get its ESID */
	clrrdi	r9,r1,28	/* get current sp ESID */
	clrldi.	r0,r6,2		/* is new ESID c00000000? */
	cmpd	cr1,r6,r9	/* or is new ESID the same as current ESID? */
	cror	eq,4*cr1+eq,eq
	beq	2f		/* if yes, don't slbie it */

	/* Bolt in the new stack SLB entry */
	ld	r7,KSP_VSID(r4)	/* Get new stack's VSID */
	oris	r0,r6,(SLB_ESID_V)@h
	ori	r0,r0,(SLB_NUM_BOLTED-1)@l
	slbie	r6
	slbie	r6		/* Workaround POWER5 < DD2.1 issue */
	slbmte	r7,r0
	isync

2:
END_FTR_SECTION_IFSET(CPU_FTR_SLB)
	clrrdi	r7,r8,THREAD_SHIFT	/* base of new stack */
	/* Note: this uses SWITCH_FRAME_SIZE rather than INT_FRAME_SIZE
	   because we don't need to leave the 288-byte ABI gap at the
	   top of the kernel stack. */
	addi	r7,r7,THREAD_SIZE-SWITCH_FRAME_SIZE

	mr	r1,r8		/* start using new stack pointer */
	std	r7,PACAKSAVE(r13)

	ld	r6,_CCR(r1)
	mtcrf	0xFF,r6

#ifdef CONFIG_ALTIVEC
BEGIN_FTR_SECTION
	ld	r0,THREAD_VRSAVE(r4)
	mtspr	SPRN_VRSAVE,r0		/* if G4, restore VRSAVE reg */
END_FTR_SECTION_IFSET(CPU_FTR_ALTIVEC)
#endif /* CONFIG_ALTIVEC */

	/* r3-r13 are destroyed -- Cort */
	REST_8GPRS(14, r1)
	REST_10GPRS(22, r1)

	/* convert old thread to its task_struct for return value */
	addi	r3,r3,-THREAD
	ld	r7,_NIP(r1)	/* Return to _switch caller in new task */
	mtlr	r7
	addi	r1,r1,SWITCH_FRAME_SIZE
	blr

	.align	7
_GLOBAL(ret_from_except)
	ld	r11,_TRAP(r1)
	andi.	r0,r11,1
	bne	.ret_from_except_lite
	REST_NVGPRS(r1)

_GLOBAL(ret_from_except_lite)
	/*
	 * Disable interrupts so that current_thread_info()->flags
	 * can't change between when we test it and when we return
	 * from the interrupt.
	 */
	mfmsr	r10		/* Get current interrupt state */
	rldicl	r9,r10,48,1	/* clear MSR_EE */
	rotldi	r9,r9,16
	mtmsrd	r9,1		/* Update machine state */

#ifdef CONFIG_PREEMPT
	clrrdi	r9,r1,THREAD_SHIFT	/* current_thread_info() */
	li	r0,_TIF_NEED_RESCHED	/* bits to check */
	ld	r3,_MSR(r1)
	ld	r4,TI_FLAGS(r9)
	/* Move MSR_PR bit in r3 to _TIF_SIGPENDING position in r0 */
	rlwimi	r0,r3,32+TIF_SIGPENDING-MSR_PR_LG,_TIF_SIGPENDING
	and.	r0,r4,r0	/* check NEED_RESCHED and maybe SIGPENDING */
	bne	do_work

#else /* !CONFIG_PREEMPT */
	ld	r3,_MSR(r1)	/* Returning to user mode? */
	andi.	r3,r3,MSR_PR
	beq	restore		/* if not, just restore regs and return */

	/* Check current_thread_info()->flags */
	clrrdi	r9,r1,THREAD_SHIFT
	ld	r4,TI_FLAGS(r9)
	andi.	r0,r4,_TIF_USER_WORK_MASK
	bne	do_work
#endif

restore:
#ifdef CONFIG_PPC_ISERIES
	ld	r5,SOFTE(r1)
	cmpdi	0,r5,0
	beq	4f
	/* Check for pending interrupts (iSeries) */
	ld	r3,PACALPPACAPTR(r13)
	ld	r3,LPPACAANYINT(r3)
	cmpdi	r3,0
	beq+	4f			/* skip do_IRQ if no interrupts */

	li	r3,0
	stb	r3,PACAPROCENABLED(r13)	/* ensure we are soft-disabled */
	ori	r10,r10,MSR_EE
	mtmsrd	r10			/* hard-enable again */
	addi	r3,r1,STACK_FRAME_OVERHEAD
	bl	.do_IRQ
	b	.ret_from_except_lite		/* loop back and handle more */

4:	stb	r5,PACAPROCENABLED(r13)
#endif

	ld	r3,_MSR(r1)
	andi.	r0,r3,MSR_RI
	beq-	unrecov_restore

	andi.	r0,r3,MSR_PR

	/*
	 * r13 is our per cpu area, only restore it if we are returning to
	 * userspace
	 */
	beq	1f
	REST_GPR(13, r1)
1:
	ld	r3,_CTR(r1)
	ld	r0,_LINK(r1)
	mtctr	r3
	mtlr	r0
	ld	r3,_XER(r1)
	mtspr	SPRN_XER,r3

	REST_8GPRS(5, r1)

	stdcx.	r0,0,r1		/* to clear the reservation */

	mfmsr	r0
	li	r2, MSR_RI
	andc	r0,r0,r2
	mtmsrd	r0,1

	ld	r0,_MSR(r1)
	mtspr	SPRN_SRR1,r0

	ld	r2,_CCR(r1)
	mtcrf	0xFF,r2
	ld	r2,_NIP(r1)
	mtspr	SPRN_SRR0,r2

	ld	r0,GPR0(r1)
	ld	r2,GPR2(r1)
	ld	r3,GPR3(r1)
	ld	r4,GPR4(r1)
	ld	r1,GPR1(r1)

	rfid
	b	.	/* prevent speculative execution */

/* Note: this must change if we start using the TIF_NOTIFY_RESUME bit */
do_work:
#ifdef CONFIG_PREEMPT
	andi.	r0,r3,MSR_PR	/* Returning to user mode? */
	bne	user_work
	/* Check that preempt_count() == 0 and interrupts are enabled */
	lwz	r8,TI_PREEMPT(r9)
	cmpwi	cr1,r8,0
#ifdef CONFIG_PPC_ISERIES
	ld	r0,SOFTE(r1)
	cmpdi	r0,0
#else
	andi.	r0,r3,MSR_EE
#endif
	crandc	eq,cr1*4+eq,eq
	bne	restore
	/* here we are preempting the current task */
1:
#ifdef CONFIG_PPC_ISERIES
	li	r0,1
	stb	r0,PACAPROCENABLED(r13)
#endif
	ori	r10,r10,MSR_EE
	mtmsrd	r10,1		/* reenable interrupts */
	bl	.preempt_schedule
	mfmsr	r10
	clrrdi	r9,r1,THREAD_SHIFT
	rldicl	r10,r10,48,1	/* disable interrupts again */
	rotldi	r10,r10,16
	mtmsrd	r10,1
	ld	r4,TI_FLAGS(r9)
	andi.	r0,r4,_TIF_NEED_RESCHED
	bne	1b
	b	restore

user_work:
#endif
	/* Enable interrupts */
	ori	r10,r10,MSR_EE
	mtmsrd	r10,1

	andi.	r0,r4,_TIF_NEED_RESCHED
	beq	1f
	bl	.schedule
	b	.ret_from_except_lite

1:	bl	.save_nvgprs
	li	r3,0
	addi	r4,r1,STACK_FRAME_OVERHEAD
	bl	.do_signal
	b	.ret_from_except

unrecov_restore:
	addi	r3,r1,STACK_FRAME_OVERHEAD
	bl	.unrecoverable_exception
	b	unrecov_restore

#ifdef CONFIG_PPC_RTAS
/*
 * On CHRP, the Run-Time Abstraction Services (RTAS) have to be
 * called with the MMU off.
 *
 * In addition, we need to be in 32b mode, at least for now.
 * 
 * Note: r3 is an input parameter to rtas, so don't trash it...
 */
_GLOBAL(enter_rtas)
	mflr	r0
	std	r0,16(r1)
        stdu	r1,-RTAS_FRAME_SIZE(r1)	/* Save SP and create stack space. */

	/* Because RTAS is running in 32b mode, it clobbers the high order half
	 * of all registers that it saves.  We therefore save those registers
	 * RTAS might touch to the stack.  (r0, r3-r13 are caller saved)
   	 */
	SAVE_GPR(2, r1)			/* Save the TOC */
	SAVE_GPR(13, r1)		/* Save paca */
	SAVE_8GPRS(14, r1)		/* Save the non-volatiles */
	SAVE_10GPRS(22, r1)		/* ditto */

	mfcr	r4
	std	r4,_CCR(r1)
	mfctr	r5
	std	r5,_CTR(r1)
	mfspr	r6,SPRN_XER
	std	r6,_XER(r1)
	mfdar	r7
	std	r7,_DAR(r1)
	mfdsisr	r8
	std	r8,_DSISR(r1)
	mfsrr0	r9
	std	r9,_SRR0(r1)
	mfsrr1	r10
	std	r10,_SRR1(r1)

	/* There is no way it is acceptable to get here with interrupts enabled,
	 * check it with the asm equivalent of WARN_ON
	 */
	mfmsr	r6
	andi.	r0,r6,MSR_EE
1:	tdnei	r0,0
.section __bug_table,"a"
	.llong	1b,__LINE__ + 0x1000000, 1f, 2f
.previous
.section .rodata,"a"
1:	.asciz	__FILE__
2:	.asciz "enter_rtas"
.previous
	
	/* Unfortunately, the stack pointer and the MSR are also clobbered,
	 * so they are saved in the PACA which allows us to restore
	 * our original state after RTAS returns.
         */
	std	r1,PACAR1(r13)
        std	r6,PACASAVEDMSR(r13)

	/* Setup our real return addr */	
	LOAD_REG_ADDR(r4,.rtas_return_loc)
	clrldi	r4,r4,2			/* convert to realmode address */
       	mtlr	r4

	li	r0,0
	ori	r0,r0,MSR_EE|MSR_SE|MSR_BE|MSR_RI
	andc	r0,r6,r0
	
        li      r9,1
        rldicr  r9,r9,MSR_SF_LG,(63-MSR_SF_LG)
	ori	r9,r9,MSR_IR|MSR_DR|MSR_FE0|MSR_FE1|MSR_FP
	andc	r6,r0,r9
	ori	r6,r6,MSR_RI
	sync				/* disable interrupts so SRR0/1 */
	mtmsrd	r0			/* don't get trashed */

	LOAD_REG_ADDR(r4, rtas)
	ld	r5,RTASENTRY(r4)	/* get the rtas->entry value */
	ld	r4,RTASBASE(r4)		/* get the rtas->base value */
	
	mtspr	SPRN_SRR0,r5
	mtspr	SPRN_SRR1,r6
	rfid
	b	.	/* prevent speculative execution */

_STATIC(rtas_return_loc)
	/* relocation is off at this point */
	mfspr	r4,SPRN_SPRG3	        /* Get PACA */
	clrldi	r4,r4,2			/* convert to realmode address */

	mfmsr   r6
	li	r0,MSR_RI
	andc	r6,r6,r0
	sync	
	mtmsrd  r6
        
        ld	r1,PACAR1(r4)           /* Restore our SP */
	LOAD_REG_IMMEDIATE(r3,.rtas_restore_regs)
        ld	r4,PACASAVEDMSR(r4)     /* Restore our MSR */

	mtspr	SPRN_SRR0,r3
	mtspr	SPRN_SRR1,r4
	rfid
	b	.	/* prevent speculative execution */

_STATIC(rtas_restore_regs)
	/* relocation is on at this point */
	REST_GPR(2, r1)			/* Restore the TOC */
	REST_GPR(13, r1)		/* Restore paca */
	REST_8GPRS(14, r1)		/* Restore the non-volatiles */
	REST_10GPRS(22, r1)		/* ditto */

	mfspr	r13,SPRN_SPRG3

	ld	r4,_CCR(r1)
	mtcr	r4
	ld	r5,_CTR(r1)
	mtctr	r5
	ld	r6,_XER(r1)
	mtspr	SPRN_XER,r6
	ld	r7,_DAR(r1)
	mtdar	r7
	ld	r8,_DSISR(r1)
	mtdsisr	r8
	ld	r9,_SRR0(r1)
	mtsrr0	r9
	ld	r10,_SRR1(r1)
	mtsrr1	r10

        addi	r1,r1,RTAS_FRAME_SIZE	/* Unstack our frame */
	ld	r0,16(r1)		/* get return address */

	mtlr    r0
        blr				/* return to caller */

#endif /* CONFIG_PPC_RTAS */

#ifdef CONFIG_PPC_MULTIPLATFORM

_GLOBAL(enter_prom)
	mflr	r0
	std	r0,16(r1)
        stdu	r1,-PROM_FRAME_SIZE(r1)	/* Save SP and create stack space */

	/* Because PROM is running in 32b mode, it clobbers the high order half
	 * of all registers that it saves.  We therefore save those registers
	 * PROM might touch to the stack.  (r0, r3-r13 are caller saved)
   	 */
	SAVE_8GPRS(2, r1)
	SAVE_GPR(13, r1)
	SAVE_8GPRS(14, r1)
	SAVE_10GPRS(22, r1)
	mfcr	r4
	std	r4,_CCR(r1)
	mfctr	r5
	std	r5,_CTR(r1)
	mfspr	r6,SPRN_XER
	std	r6,_XER(r1)
	mfdar	r7
	std	r7,_DAR(r1)
	mfdsisr	r8
	std	r8,_DSISR(r1)
	mfsrr0	r9
	std	r9,_SRR0(r1)
	mfsrr1	r10
	std	r10,_SRR1(r1)
	mfmsr	r11
	std	r11,_MSR(r1)

	/* Get the PROM entrypoint */
	ld	r0,GPR4(r1)
	mtlr	r0

	/* Switch MSR to 32 bits mode
	 */
        mfmsr   r11
        li      r12,1
        rldicr  r12,r12,MSR_SF_LG,(63-MSR_SF_LG)
        andc    r11,r11,r12
        li      r12,1
        rldicr  r12,r12,MSR_ISF_LG,(63-MSR_ISF_LG)
        andc    r11,r11,r12
        mtmsrd  r11
        isync

	/* Restore arguments & enter PROM here... */
	ld	r3,GPR3(r1)
	blrl

	/* Just make sure that r1 top 32 bits didn't get
	 * corrupt by OF
	 */
	rldicl	r1,r1,0,32

	/* Restore the MSR (back to 64 bits) */
	ld	r0,_MSR(r1)
	mtmsrd	r0
        isync

	/* Restore other registers */
	REST_GPR(2, r1)
	REST_GPR(13, r1)
	REST_8GPRS(14, r1)
	REST_10GPRS(22, r1)
	ld	r4,_CCR(r1)
	mtcr	r4
	ld	r5,_CTR(r1)
	mtctr	r5
	ld	r6,_XER(r1)
	mtspr	SPRN_XER,r6
	ld	r7,_DAR(r1)
	mtdar	r7
	ld	r8,_DSISR(r1)
	mtdsisr	r8
	ld	r9,_SRR0(r1)
	mtsrr0	r9
	ld	r10,_SRR1(r1)
	mtsrr1	r10
	
        addi	r1,r1,PROM_FRAME_SIZE
	ld	r0,16(r1)
	mtlr    r0
        blr
	
#endif	/* CONFIG_PPC_MULTIPLATFORM */
