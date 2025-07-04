/*
 *  linux/arch/x86_64/entry.S
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2000, 2001, 2002  Andi Kleen SuSE Labs
 *  Copyright (C) 2000  Pavel Machek <pavel@suse.cz>
 * 
 *  $Id$
 */

/*
 * entry.S contains the system-call and fault low-level handling routines.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after an interrupt and after each system call.
 * 
 * Normal syscalls and interrupts don't save a full stack frame, this is 
 * only done for syscall tracing, signals or fork/exec et.al.
 * 
 * A note on terminology:	 
 * - top of stack: Architecture defined interrupt frame from SS to RIP 
 * at the top of the kernel process stack.	
 * - partial stack frame: partially saved registers upto R11.
 * - full stack frame: Like partial stack frame, but all register saved. 
 *	
 * TODO:	 
 * - schedule it carefully for the final hardware.
 */

#define ASSEMBLY 1
#include <linux/config.h>
#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/smp.h>
#include <asm/cache.h>
#include <asm/errno.h>
#include <asm/dwarf2.h>
#include <asm/calling.h>
#include <asm/asm-offsets.h>
#include <asm/msr.h>
#include <asm/unistd.h>
#include <asm/thread_info.h>
#include <asm/hw_irq.h>
#include <asm/page.h>

	.code64

#ifndef CONFIG_PREEMPT
#define retint_kernel retint_restore_args
#endif	
	
/*
 * C code is not supposed to know about undefined top of stack. Every time 
 * a C function with an pt_regs argument is called from the SYSCALL based 
 * fast path FIXUP_TOP_OF_STACK is needed.
 * RESTORE_TOP_OF_STACK syncs the syscall state after any possible ptregs
 * manipulation.
 */        	
		
	/* %rsp:at FRAMEEND */ 
	.macro FIXUP_TOP_OF_STACK tmp
	movq	%gs:pda_oldrsp,\tmp
	movq  	\tmp,RSP(%rsp)
	movq    $__USER_DS,SS(%rsp)
	movq    $__USER_CS,CS(%rsp)
	movq 	$-1,RCX(%rsp)
	movq	R11(%rsp),\tmp  /* get eflags */
	movq	\tmp,EFLAGS(%rsp)
	.endm

	.macro RESTORE_TOP_OF_STACK tmp,offset=0
	movq   RSP-\offset(%rsp),\tmp
	movq   \tmp,%gs:pda_oldrsp
	movq   EFLAGS-\offset(%rsp),\tmp
	movq   \tmp,R11-\offset(%rsp)
	.endm

	.macro FAKE_STACK_FRAME child_rip
	/* push in order ss, rsp, eflags, cs, rip */
	xorl %eax, %eax
	pushq %rax /* ss */
	CFI_ADJUST_CFA_OFFSET	8
	/*CFI_REL_OFFSET	ss,0*/
	pushq %rax /* rsp */
	CFI_ADJUST_CFA_OFFSET	8
	CFI_REL_OFFSET	rsp,0
	pushq $(1<<9) /* eflags - interrupts on */
	CFI_ADJUST_CFA_OFFSET	8
	/*CFI_REL_OFFSET	rflags,0*/
	pushq $__KERNEL_CS /* cs */
	CFI_ADJUST_CFA_OFFSET	8
	/*CFI_REL_OFFSET	cs,0*/
	pushq \child_rip /* rip */
	CFI_ADJUST_CFA_OFFSET	8
	CFI_REL_OFFSET	rip,0
	pushq	%rax /* orig rax */
	CFI_ADJUST_CFA_OFFSET	8
	.endm

	.macro UNFAKE_STACK_FRAME
	addq $8*6, %rsp
	CFI_ADJUST_CFA_OFFSET	-(6*8)
	.endm

	.macro	CFI_DEFAULT_STACK start=1
	.if \start
	CFI_STARTPROC	simple
	CFI_DEF_CFA	rsp,SS+8
	.else
	CFI_DEF_CFA_OFFSET SS+8
	.endif
	CFI_REL_OFFSET	r15,R15
	CFI_REL_OFFSET	r14,R14
	CFI_REL_OFFSET	r13,R13
	CFI_REL_OFFSET	r12,R12
	CFI_REL_OFFSET	rbp,RBP
	CFI_REL_OFFSET	rbx,RBX
	CFI_REL_OFFSET	r11,R11
	CFI_REL_OFFSET	r10,R10
	CFI_REL_OFFSET	r9,R9
	CFI_REL_OFFSET	r8,R8
	CFI_REL_OFFSET	rax,RAX
	CFI_REL_OFFSET	rcx,RCX
	CFI_REL_OFFSET	rdx,RDX
	CFI_REL_OFFSET	rsi,RSI
	CFI_REL_OFFSET	rdi,RDI
	CFI_REL_OFFSET	rip,RIP
	/*CFI_REL_OFFSET	cs,CS*/
	/*CFI_REL_OFFSET	rflags,EFLAGS*/
	CFI_REL_OFFSET	rsp,RSP
	/*CFI_REL_OFFSET	ss,SS*/
	.endm
/*
 * A newly forked process directly context switches into this.
 */ 	
/* rdi:	prev */	
ENTRY(ret_from_fork)
	CFI_DEFAULT_STACK
	call schedule_tail
	GET_THREAD_INFO(%rcx)
	testl $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT),threadinfo_flags(%rcx)
	jnz rff_trace
rff_action:	
	RESTORE_REST
	testl $3,CS-ARGOFFSET(%rsp)	# from kernel_thread?
	je   int_ret_from_sys_call
	testl $_TIF_IA32,threadinfo_flags(%rcx)
	jnz  int_ret_from_sys_call
	RESTORE_TOP_OF_STACK %rdi,ARGOFFSET
	jmp ret_from_sys_call
rff_trace:
	movq %rsp,%rdi
	call syscall_trace_leave
	GET_THREAD_INFO(%rcx)	
	jmp rff_action
	CFI_ENDPROC

/*
 * System call entry. Upto 6 arguments in registers are supported.
 *
 * SYSCALL does not save anything on the stack and does not change the
 * stack pointer.
 */
		
/*
 * Register setup:	
 * rax  system call number
 * rdi  arg0
 * rcx  return address for syscall/sysret, C arg3 
 * rsi  arg1
 * rdx  arg2	
 * r10  arg3 	(--> moved to rcx for C)
 * r8   arg4
 * r9   arg5
 * r11  eflags for syscall/sysret, temporary for C
 * r12-r15,rbp,rbx saved by C code, not touched. 		
 * 
 * Interrupts are off on entry.
 * Only called from user space.
 *
 * XXX	if we had a free scratch register we could save the RSP into the stack frame
 *      and report it properly in ps. Unfortunately we haven't.
 */ 			 		

ENTRY(system_call)
	CFI_STARTPROC	simple
	CFI_DEF_CFA	rsp,0
	CFI_REGISTER	rip,rcx
	/*CFI_REGISTER	rflags,r11*/
	swapgs
	movq	%rsp,%gs:pda_oldrsp 
	movq	%gs:pda_kernelstack,%rsp
	sti					
	SAVE_ARGS 8,1
	movq  %rax,ORIG_RAX-ARGOFFSET(%rsp) 
	movq  %rcx,RIP-ARGOFFSET(%rsp)
	CFI_REL_OFFSET rip,RIP-ARGOFFSET
	GET_THREAD_INFO(%rcx)
	testl $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|_TIF_SECCOMP),threadinfo_flags(%rcx)
	CFI_REMEMBER_STATE
	jnz tracesys
	cmpq $__NR_syscall_max,%rax
	ja badsys
	movq %r10,%rcx
	call *sys_call_table(,%rax,8)  # XXX:	 rip relative
	movq %rax,RAX-ARGOFFSET(%rsp)
/*
 * Syscall return path ending with SYSRET (fast path)
 * Has incomplete stack frame and undefined top of stack. 
 */		
	.globl ret_from_sys_call
ret_from_sys_call:
	movl $_TIF_ALLWORK_MASK,%edi
	/* edi:	flagmask */
sysret_check:		
	GET_THREAD_INFO(%rcx)
	cli
	movl threadinfo_flags(%rcx),%edx
	andl %edi,%edx
	CFI_REMEMBER_STATE
	jnz  sysret_careful 
	movq RIP-ARGOFFSET(%rsp),%rcx
	CFI_REGISTER	rip,rcx
	RESTORE_ARGS 0,-ARG_SKIP,1
	/*CFI_REGISTER	rflags,r11*/
	movq	%gs:pda_oldrsp,%rsp
	swapgs
	sysretq

	/* Handle reschedules */
	/* edx:	work, edi: workmask */	
sysret_careful:
	CFI_RESTORE_STATE
	bt $TIF_NEED_RESCHED,%edx
	jnc sysret_signal
	sti
	pushq %rdi
	CFI_ADJUST_CFA_OFFSET 8
	call schedule
	popq  %rdi
	CFI_ADJUST_CFA_OFFSET -8
	jmp sysret_check

	/* Handle a signal */ 
sysret_signal:
	sti
	testl $(_TIF_SIGPENDING|_TIF_NOTIFY_RESUME|_TIF_SINGLESTEP),%edx
	jz    1f

	/* Really a signal */
	/* edx:	work flags (arg3) */
	leaq do_notify_resume(%rip),%rax
	leaq -ARGOFFSET(%rsp),%rdi # &pt_regs -> arg1
	xorl %esi,%esi # oldset -> arg2
	call ptregscall_common
1:	movl $_TIF_NEED_RESCHED,%edi
	jmp sysret_check
	
badsys:
	movq $-ENOSYS,RAX-ARGOFFSET(%rsp)
	jmp ret_from_sys_call

	/* Do syscall tracing */
tracesys:			 
	CFI_RESTORE_STATE
	SAVE_REST
	movq $-ENOSYS,RAX(%rsp)
	FIXUP_TOP_OF_STACK %rdi
	movq %rsp,%rdi
	call syscall_trace_enter
	LOAD_ARGS ARGOFFSET  /* reload args from stack in case ptrace changed it */
	RESTORE_REST
	cmpq $__NR_syscall_max,%rax
	ja  1f
	movq %r10,%rcx	/* fixup for C */
	call *sys_call_table(,%rax,8)
	movq %rax,RAX-ARGOFFSET(%rsp)
1:	SAVE_REST
	movq %rsp,%rdi
	call syscall_trace_leave
	RESTORE_TOP_OF_STACK %rbx
	RESTORE_REST
	jmp ret_from_sys_call
	CFI_ENDPROC
		
/* 
 * Syscall return path ending with IRET.
 * Has correct top of stack, but partial stack frame.
 */ 	
ENTRY(int_ret_from_sys_call)
	CFI_STARTPROC	simple
	CFI_DEF_CFA	rsp,SS+8-ARGOFFSET
	/*CFI_REL_OFFSET	ss,SS-ARGOFFSET*/
	CFI_REL_OFFSET	rsp,RSP-ARGOFFSET
	/*CFI_REL_OFFSET	rflags,EFLAGS-ARGOFFSET*/
	/*CFI_REL_OFFSET	cs,CS-ARGOFFSET*/
	CFI_REL_OFFSET	rip,RIP-ARGOFFSET
	CFI_REL_OFFSET	rdx,RDX-ARGOFFSET
	CFI_REL_OFFSET	rcx,RCX-ARGOFFSET
	CFI_REL_OFFSET	rax,RAX-ARGOFFSET
	CFI_REL_OFFSET	rdi,RDI-ARGOFFSET
	CFI_REL_OFFSET	rsi,RSI-ARGOFFSET
	CFI_REL_OFFSET	r8,R8-ARGOFFSET
	CFI_REL_OFFSET	r9,R9-ARGOFFSET
	CFI_REL_OFFSET	r10,R10-ARGOFFSET
	CFI_REL_OFFSET	r11,R11-ARGOFFSET
	cli
	testl $3,CS-ARGOFFSET(%rsp)
	je retint_restore_args
	movl $_TIF_ALLWORK_MASK,%edi
	/* edi:	mask to check */
int_with_check:
	GET_THREAD_INFO(%rcx)
	movl threadinfo_flags(%rcx),%edx
	andl %edi,%edx
	jnz   int_careful
	andl    $~TS_COMPAT,threadinfo_status(%rcx)
	jmp   retint_swapgs

	/* Either reschedule or signal or syscall exit tracking needed. */
	/* First do a reschedule test. */
	/* edx:	work, edi: workmask */
int_careful:
	bt $TIF_NEED_RESCHED,%edx
	jnc  int_very_careful
	sti
	pushq %rdi
	CFI_ADJUST_CFA_OFFSET 8
	call schedule
	popq %rdi
	CFI_ADJUST_CFA_OFFSET -8
	cli
	jmp int_with_check

	/* handle signals and tracing -- both require a full stack frame */
int_very_careful:
	sti
	SAVE_REST
	/* Check for syscall exit trace */	
	testl $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|_TIF_SINGLESTEP),%edx
	jz int_signal
	pushq %rdi
	CFI_ADJUST_CFA_OFFSET 8
	leaq 8(%rsp),%rdi	# &ptregs -> arg1	
	call syscall_trace_leave
	popq %rdi
	CFI_ADJUST_CFA_OFFSET -8
	andl $~(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|_TIF_SINGLESTEP),%edi
	cli
	jmp int_restore_rest
	
int_signal:
	testl $(_TIF_NOTIFY_RESUME|_TIF_SIGPENDING|_TIF_SINGLESTEP),%edx
	jz 1f
	movq %rsp,%rdi		# &ptregs -> arg1
	xorl %esi,%esi		# oldset -> arg2
	call do_notify_resume
1:	movl $_TIF_NEED_RESCHED,%edi	
int_restore_rest:
	RESTORE_REST
	cli
	jmp int_with_check
	CFI_ENDPROC
		
/* 
 * Certain special system calls that need to save a complete full stack frame.
 */ 								
	
	.macro PTREGSCALL label,func,arg
	.globl \label
\label:
	leaq	\func(%rip),%rax
	leaq    -ARGOFFSET+8(%rsp),\arg /* 8 for return address */
	jmp	ptregscall_common
	.endm

	CFI_STARTPROC

	PTREGSCALL stub_clone, sys_clone, %r8
	PTREGSCALL stub_fork, sys_fork, %rdi
	PTREGSCALL stub_vfork, sys_vfork, %rdi
	PTREGSCALL stub_rt_sigsuspend, sys_rt_sigsuspend, %rdx
	PTREGSCALL stub_sigaltstack, sys_sigaltstack, %rdx
	PTREGSCALL stub_iopl, sys_iopl, %rsi

ENTRY(ptregscall_common)
	popq %r11
	CFI_ADJUST_CFA_OFFSET -8
	CFI_REGISTER rip, r11
	SAVE_REST
	movq %r11, %r15
	CFI_REGISTER rip, r15
	FIXUP_TOP_OF_STACK %r11
	call *%rax
	RESTORE_TOP_OF_STACK %r11
	movq %r15, %r11
	CFI_REGISTER rip, r11
	RESTORE_REST
	pushq %r11
	CFI_ADJUST_CFA_OFFSET 8
	CFI_REL_OFFSET rip, 0
	ret
	CFI_ENDPROC
	
ENTRY(stub_execve)
	CFI_STARTPROC
	popq %r11
	CFI_ADJUST_CFA_OFFSET -8
	CFI_REGISTER rip, r11
	SAVE_REST
	movq %r11, %r15
	CFI_REGISTER rip, r15
	FIXUP_TOP_OF_STACK %r11
	call sys_execve
	GET_THREAD_INFO(%rcx)
	bt $TIF_IA32,threadinfo_flags(%rcx)
	CFI_REMEMBER_STATE
	jc exec_32bit
	RESTORE_TOP_OF_STACK %r11
	movq %r15, %r11
	CFI_REGISTER rip, r11
	RESTORE_REST
	pushq %r11
	CFI_ADJUST_CFA_OFFSET 8
	CFI_REL_OFFSET rip, 0
	ret

exec_32bit:
	CFI_RESTORE_STATE
	movq %rax,RAX(%rsp)
	RESTORE_REST
	jmp int_ret_from_sys_call
	CFI_ENDPROC
	
/*
 * sigreturn is special because it needs to restore all registers on return.
 * This cannot be done with SYSRET, so use the IRET return path instead.
 */                
ENTRY(stub_rt_sigreturn)
	CFI_STARTPROC
	addq $8, %rsp
	CFI_ADJUST_CFA_OFFSET	-8
	SAVE_REST
	movq %rsp,%rdi
	FIXUP_TOP_OF_STACK %r11
	call sys_rt_sigreturn
	movq %rax,RAX(%rsp) # fixme, this could be done at the higher layer
	RESTORE_REST
	jmp int_ret_from_sys_call
	CFI_ENDPROC

/*
 * initial frame state for interrupts and exceptions
 */
	.macro _frame ref
	CFI_STARTPROC simple
	CFI_DEF_CFA rsp,SS+8-\ref
	/*CFI_REL_OFFSET ss,SS-\ref*/
	CFI_REL_OFFSET rsp,RSP-\ref
	/*CFI_REL_OFFSET rflags,EFLAGS-\ref*/
	/*CFI_REL_OFFSET cs,CS-\ref*/
	CFI_REL_OFFSET rip,RIP-\ref
	.endm

/* initial frame state for interrupts (and exceptions without error code) */
#define INTR_FRAME _frame RIP
/* initial frame state for exceptions with error code (and interrupts with
   vector already pushed) */
#define XCPT_FRAME _frame ORIG_RAX

/* 
 * Interrupt entry/exit.
 *
 * Interrupt entry points save only callee clobbered registers in fast path.
 *	
 * Entry runs with interrupts off.	
 */ 

/* 0(%rsp): interrupt number */ 
	.macro interrupt func
	cld
#ifdef CONFIG_DEBUG_INFO
	SAVE_ALL	
	movq %rsp,%rdi
	/*
	 * Setup a stack frame pointer.  This allows gdb to trace
	 * back to the original stack.
	 */
	movq %rsp,%rbp
	CFI_DEF_CFA_REGISTER	rbp
#else		
	SAVE_ARGS
	leaq -ARGOFFSET(%rsp),%rdi	# arg1 for handler
#endif	
	testl $3,CS(%rdi)
	je 1f
	swapgs	
1:	incl	%gs:pda_irqcount	# RED-PEN should check preempt count
	movq %gs:pda_irqstackptr,%rax
	cmoveq %rax,%rsp /*todo This needs CFI annotation! */
	pushq %rdi			# save old stack	
	CFI_ADJUST_CFA_OFFSET	8
	call \func
	.endm

ENTRY(common_interrupt)
	XCPT_FRAME
	interrupt do_IRQ
	/* 0(%rsp): oldrsp-ARGOFFSET */
ret_from_intr:
	popq  %rdi
	CFI_ADJUST_CFA_OFFSET	-8
	cli	
	decl %gs:pda_irqcount
#ifdef CONFIG_DEBUG_INFO
	movq RBP(%rdi),%rbp
	CFI_DEF_CFA_REGISTER	rsp
#endif
	leaq ARGOFFSET(%rdi),%rsp /*todo This needs CFI annotation! */
exit_intr:
	GET_THREAD_INFO(%rcx)
	testl $3,CS-ARGOFFSET(%rsp)
	je retint_kernel
	
	/* Interrupt came from user space */
	/*
	 * Has a correct top of stack, but a partial stack frame
	 * %rcx: thread info. Interrupts off.
	 */		
retint_with_reschedule:
	movl $_TIF_WORK_MASK,%edi
retint_check:
	movl threadinfo_flags(%rcx),%edx
	andl %edi,%edx
	CFI_REMEMBER_STATE
	jnz  retint_careful
retint_swapgs:	 	
	swapgs 
retint_restore_args:				
	cli
	RESTORE_ARGS 0,8,0						
iret_label:	
	iretq

	.section __ex_table,"a"
	.quad iret_label,bad_iret	
	.previous
	.section .fixup,"ax"
	/* force a signal here? this matches i386 behaviour */
	/* running with kernel gs */
bad_iret:
	movq $-9999,%rdi	/* better code? */
	jmp do_exit			
	.previous	
	
	/* edi: workmask, edx: work */
retint_careful:
	CFI_RESTORE_STATE
	bt    $TIF_NEED_RESCHED,%edx
	jnc   retint_signal
	sti
	pushq %rdi
	CFI_ADJUST_CFA_OFFSET	8
	call  schedule
	popq %rdi		
	CFI_ADJUST_CFA_OFFSET	-8
	GET_THREAD_INFO(%rcx)
	cli
	jmp retint_check
	
retint_signal:
	testl $(_TIF_SIGPENDING|_TIF_NOTIFY_RESUME|_TIF_SINGLESTEP),%edx
	jz    retint_swapgs
	sti
	SAVE_REST
	movq $-1,ORIG_RAX(%rsp) 			
	xorl %esi,%esi		# oldset
	movq %rsp,%rdi		# &pt_regs
	call do_notify_resume
	RESTORE_REST
	cli
	movl $_TIF_NEED_RESCHED,%edi
	GET_THREAD_INFO(%rcx)
	jmp retint_check

#ifdef CONFIG_PREEMPT
	/* Returning to kernel space. Check if we need preemption */
	/* rcx:	 threadinfo. interrupts off. */
	.p2align
retint_kernel:	
	cmpl $0,threadinfo_preempt_count(%rcx)
	jnz  retint_restore_args
	bt  $TIF_NEED_RESCHED,threadinfo_flags(%rcx)
	jnc  retint_restore_args
	bt   $9,EFLAGS-ARGOFFSET(%rsp)	/* interrupts off? */
	jnc  retint_restore_args
	call preempt_schedule_irq
	jmp exit_intr
#endif	
	CFI_ENDPROC
	
/*
 * APIC interrupts.
 */		
	.macro apicinterrupt num,func
	INTR_FRAME
	pushq $\num-256
	CFI_ADJUST_CFA_OFFSET 8
	interrupt \func
	jmp ret_from_intr
	CFI_ENDPROC
	.endm

ENTRY(thermal_interrupt)
	apicinterrupt THERMAL_APIC_VECTOR,smp_thermal_interrupt

ENTRY(threshold_interrupt)
	apicinterrupt THRESHOLD_APIC_VECTOR,mce_threshold_interrupt

#ifdef CONFIG_SMP	
ENTRY(reschedule_interrupt)
	apicinterrupt RESCHEDULE_VECTOR,smp_reschedule_interrupt

	.macro INVALIDATE_ENTRY num
ENTRY(invalidate_interrupt\num)
	apicinterrupt INVALIDATE_TLB_VECTOR_START+\num,smp_invalidate_interrupt	
	.endm

	INVALIDATE_ENTRY 0
	INVALIDATE_ENTRY 1
	INVALIDATE_ENTRY 2
	INVALIDATE_ENTRY 3
	INVALIDATE_ENTRY 4
	INVALIDATE_ENTRY 5
	INVALIDATE_ENTRY 6
	INVALIDATE_ENTRY 7

ENTRY(call_function_interrupt)
	apicinterrupt CALL_FUNCTION_VECTOR,smp_call_function_interrupt
#endif

#ifdef CONFIG_X86_LOCAL_APIC	
ENTRY(apic_timer_interrupt)
	apicinterrupt LOCAL_TIMER_VECTOR,smp_apic_timer_interrupt

ENTRY(error_interrupt)
	apicinterrupt ERROR_APIC_VECTOR,smp_error_interrupt

ENTRY(spurious_interrupt)
	apicinterrupt SPURIOUS_APIC_VECTOR,smp_spurious_interrupt
#endif
				
/*
 * Exception entry points.
 */ 		
	.macro zeroentry sym
	INTR_FRAME
	pushq $0	/* push error code/oldrax */ 
	CFI_ADJUST_CFA_OFFSET 8
	pushq %rax	/* push real oldrax to the rdi slot */ 
	CFI_ADJUST_CFA_OFFSET 8
	leaq  \sym(%rip),%rax
	jmp error_entry
	CFI_ENDPROC
	.endm	

	.macro errorentry sym
	XCPT_FRAME
	pushq %rax
	CFI_ADJUST_CFA_OFFSET 8
	leaq  \sym(%rip),%rax
	jmp error_entry
	CFI_ENDPROC
	.endm

	/* error code is on the stack already */
	/* handle NMI like exceptions that can happen everywhere */
	.macro paranoidentry sym, ist=0
	SAVE_ALL
	cld
	movl $1,%ebx
	movl  $MSR_GS_BASE,%ecx
	rdmsr
	testl %edx,%edx
	js    1f
	swapgs
	xorl  %ebx,%ebx
1:
	.if \ist
	movq	%gs:pda_data_offset, %rbp
	.endif
	movq %rsp,%rdi
	movq ORIG_RAX(%rsp),%rsi
	movq $-1,ORIG_RAX(%rsp)
	.if \ist
	subq	$EXCEPTION_STKSZ, per_cpu__init_tss + TSS_ist + (\ist - 1) * 8(%rbp)
	.endif
	call \sym
	.if \ist
	addq	$EXCEPTION_STKSZ, per_cpu__init_tss + TSS_ist + (\ist - 1) * 8(%rbp)
	.endif
	cli
	.endm
	
/*
 * Exception entry point. This expects an error code/orig_rax on the stack
 * and the exception handler in %rax.	
 */ 		  				
ENTRY(error_entry)
	_frame RDI
	/* rdi slot contains rax, oldrax contains error code */
	cld	
	subq  $14*8,%rsp
	CFI_ADJUST_CFA_OFFSET	(14*8)
	movq %rsi,13*8(%rsp)
	CFI_REL_OFFSET	rsi,RSI
	movq 14*8(%rsp),%rsi	/* load rax from rdi slot */
	movq %rdx,12*8(%rsp)
	CFI_REL_OFFSET	rdx,RDX
	movq %rcx,11*8(%rsp)
	CFI_REL_OFFSET	rcx,RCX
	movq %rsi,10*8(%rsp)	/* store rax */ 
	CFI_REL_OFFSET	rax,RAX
	movq %r8, 9*8(%rsp)
	CFI_REL_OFFSET	r8,R8
	movq %r9, 8*8(%rsp)
	CFI_REL_OFFSET	r9,R9
	movq %r10,7*8(%rsp)
	CFI_REL_OFFSET	r10,R10
	movq %r11,6*8(%rsp)
	CFI_REL_OFFSET	r11,R11
	movq %rbx,5*8(%rsp) 
	CFI_REL_OFFSET	rbx,RBX
	movq %rbp,4*8(%rsp) 
	CFI_REL_OFFSET	rbp,RBP
	movq %r12,3*8(%rsp) 
	CFI_REL_OFFSET	r12,R12
	movq %r13,2*8(%rsp) 
	CFI_REL_OFFSET	r13,R13
	movq %r14,1*8(%rsp) 
	CFI_REL_OFFSET	r14,R14
	movq %r15,(%rsp) 
	CFI_REL_OFFSET	r15,R15
	xorl %ebx,%ebx	
	testl $3,CS(%rsp)
	je  error_kernelspace
error_swapgs:	
	swapgs
error_sti:	
	movq %rdi,RDI(%rsp) 	
	movq %rsp,%rdi
	movq ORIG_RAX(%rsp),%rsi	/* get error code */ 
	movq $-1,ORIG_RAX(%rsp)
	call *%rax
	/* ebx:	no swapgs flag (1: don't need swapgs, 0: need it) */	 
error_exit:		
	movl %ebx,%eax		
	RESTORE_REST
	cli
	GET_THREAD_INFO(%rcx)	
	testl %eax,%eax
	jne  retint_kernel
	movl  threadinfo_flags(%rcx),%edx
	movl  $_TIF_WORK_MASK,%edi
	andl  %edi,%edx
	jnz  retint_careful
	swapgs 
	RESTORE_ARGS 0,8,0						
	jmp iret_label
	CFI_ENDPROC

error_kernelspace:
	incl %ebx
       /* There are two places in the kernel that can potentially fault with
          usergs. Handle them here. The exception handlers after
	   iret run with kernel gs again, so don't set the user space flag.
	   B stepping K8s sometimes report an truncated RIP for IRET 
	   exceptions returning to compat mode. Check for these here too. */
	leaq iret_label(%rip),%rbp
	cmpq %rbp,RIP(%rsp) 
	je   error_swapgs
	movl %ebp,%ebp	/* zero extend */
	cmpq %rbp,RIP(%rsp) 
	je   error_swapgs
	cmpq $gs_change,RIP(%rsp)
        je   error_swapgs
	jmp  error_sti
	
       /* Reload gs selector with exception handling */
       /* edi:  new selector */ 
ENTRY(load_gs_index)
	CFI_STARTPROC
	pushf
	CFI_ADJUST_CFA_OFFSET 8
	cli
        swapgs
gs_change:     
        movl %edi,%gs   
2:	mfence		/* workaround */
	swapgs
        popf
	CFI_ADJUST_CFA_OFFSET -8
        ret
	CFI_ENDPROC
       
        .section __ex_table,"a"
        .align 8
        .quad gs_change,bad_gs
        .previous
        .section .fixup,"ax"
	/* running with kernelgs */
bad_gs: 
	swapgs			/* switch back to user gs */
	xorl %eax,%eax
        movl %eax,%gs
        jmp  2b
        .previous       
	
/*
 * Create a kernel thread.
 *
 * C extern interface:
 *	extern long kernel_thread(int (*fn)(void *), void * arg, unsigned long flags)
 *
 * asm input arguments:
 *	rdi: fn, rsi: arg, rdx: flags
 */
ENTRY(kernel_thread)
	CFI_STARTPROC
	FAKE_STACK_FRAME $child_rip
	SAVE_ALL

	# rdi: flags, rsi: usp, rdx: will be &pt_regs
	movq %rdx,%rdi
	orq  kernel_thread_flags(%rip),%rdi
	movq $-1, %rsi
	movq %rsp, %rdx

	xorl %r8d,%r8d
	xorl %r9d,%r9d
	
	# clone now
	call do_fork
	movq %rax,RAX(%rsp)
	xorl %edi,%edi

	/*
	 * It isn't worth to check for reschedule here,
	 * so internally to the x86_64 port you can rely on kernel_thread()
	 * not to reschedule the child before returning, this avoids the need
	 * of hacks for example to fork off the per-CPU idle tasks.
         * [Hopefully no generic code relies on the reschedule -AK]	
	 */
	RESTORE_ALL
	UNFAKE_STACK_FRAME
	ret
	CFI_ENDPROC

	
child_rip:
	/*
	 * Here we are in the child and the registers are set as they were
	 * at kernel_thread() invocation in the parent.
	 */
	movq %rdi, %rax
	movq %rsi, %rdi
	call *%rax
	# exit
	xorl %edi, %edi
	call do_exit

/*
 * execve(). This function needs to use IRET, not SYSRET, to set up all state properly.
 *
 * C extern interface:
 *	 extern long execve(char *name, char **argv, char **envp)
 *
 * asm input arguments:
 *	rdi: name, rsi: argv, rdx: envp
 *
 * We want to fallback into:
 *	extern long sys_execve(char *name, char **argv,char **envp, struct pt_regs regs)
 *
 * do_sys_execve asm fallback arguments:
 *	rdi: name, rsi: argv, rdx: envp, fake frame on the stack
 */
ENTRY(execve)
	CFI_STARTPROC
	FAKE_STACK_FRAME $0
	SAVE_ALL	
	call sys_execve
	movq %rax, RAX(%rsp)	
	RESTORE_REST
	testq %rax,%rax
	je int_ret_from_sys_call
	RESTORE_ARGS
	UNFAKE_STACK_FRAME
	ret
	CFI_ENDPROC

KPROBE_ENTRY(page_fault)
	errorentry do_page_fault
	.previous .text

ENTRY(coprocessor_error)
	zeroentry do_coprocessor_error

ENTRY(simd_coprocessor_error)
	zeroentry do_simd_coprocessor_error	

ENTRY(device_not_available)
	zeroentry math_state_restore

	/* runs on exception stack */
KPROBE_ENTRY(debug)
 	INTR_FRAME
	pushq $0
	CFI_ADJUST_CFA_OFFSET 8		
	paranoidentry do_debug, DEBUG_STACK
	jmp paranoid_exit
	CFI_ENDPROC
	.previous .text

	/* runs on exception stack */	
ENTRY(nmi)
	INTR_FRAME
	pushq $-1
	CFI_ADJUST_CFA_OFFSET 8
	paranoidentry do_nmi
	/*
 	 * "Paranoid" exit path from exception stack.
  	 * Paranoid because this is used by NMIs and cannot take
	 * any kernel state for granted.
	 * We don't do kernel preemption checks here, because only
	 * NMI should be common and it does not enable IRQs and
	 * cannot get reschedule ticks.
	 */
	/* ebx:	no swapgs flag */
paranoid_exit:
	testl %ebx,%ebx				/* swapgs needed? */
	jnz paranoid_restore
	testl $3,CS(%rsp)
	jnz   paranoid_userspace
paranoid_swapgs:	
	swapgs
paranoid_restore:	
	RESTORE_ALL 8
	iretq
paranoid_userspace:	
	GET_THREAD_INFO(%rcx)
	movl threadinfo_flags(%rcx),%ebx
	andl $_TIF_WORK_MASK,%ebx
	jz paranoid_swapgs
	movq %rsp,%rdi			/* &pt_regs */
	call sync_regs
	movq %rax,%rsp			/* switch stack for scheduling */
	testl $_TIF_NEED_RESCHED,%ebx
	jnz paranoid_schedule
	movl %ebx,%edx			/* arg3: thread flags */
	sti
	xorl %esi,%esi 			/* arg2: oldset */
	movq %rsp,%rdi 			/* arg1: &pt_regs */
	call do_notify_resume
	cli
	jmp paranoid_userspace
paranoid_schedule:
	sti
	call schedule
	cli
	jmp paranoid_userspace
	CFI_ENDPROC

KPROBE_ENTRY(int3)
 	INTR_FRAME
 	pushq $0
 	CFI_ADJUST_CFA_OFFSET 8
 	paranoidentry do_int3, DEBUG_STACK
 	jmp paranoid_exit
 	CFI_ENDPROC
	.previous .text

ENTRY(overflow)
	zeroentry do_overflow

ENTRY(bounds)
	zeroentry do_bounds

ENTRY(invalid_op)
	zeroentry do_invalid_op	

ENTRY(coprocessor_segment_overrun)
	zeroentry do_coprocessor_segment_overrun

ENTRY(reserved)
	zeroentry do_reserved

	/* runs on exception stack */
ENTRY(double_fault)
	XCPT_FRAME
	paranoidentry do_double_fault
	jmp paranoid_exit
	CFI_ENDPROC

ENTRY(invalid_TSS)
	errorentry do_invalid_TSS

ENTRY(segment_not_present)
	errorentry do_segment_not_present

	/* runs on exception stack */
ENTRY(stack_segment)
	XCPT_FRAME
	paranoidentry do_stack_segment
	jmp paranoid_exit
	CFI_ENDPROC

KPROBE_ENTRY(general_protection)
	errorentry do_general_protection
	.previous .text

ENTRY(alignment_check)
	errorentry do_alignment_check

ENTRY(divide_error)
	zeroentry do_divide_error

ENTRY(spurious_interrupt_bug)
	zeroentry do_spurious_interrupt_bug

#ifdef CONFIG_X86_MCE
	/* runs on exception stack */
ENTRY(machine_check)
	INTR_FRAME
	pushq $0
	CFI_ADJUST_CFA_OFFSET 8	
	paranoidentry do_machine_check
	jmp paranoid_exit
	CFI_ENDPROC
#endif

ENTRY(call_softirq)
	CFI_STARTPROC
	movq %gs:pda_irqstackptr,%rax
	movq %rsp,%rdx
	CFI_DEF_CFA_REGISTER	rdx
	incl %gs:pda_irqcount
	cmove %rax,%rsp
	pushq %rdx
	/*todo CFI_DEF_CFA_EXPRESSION ...*/
	call __do_softirq
	popq %rsp
	CFI_DEF_CFA_REGISTER	rsp
	decl %gs:pda_irqcount
	ret
	CFI_ENDPROC
