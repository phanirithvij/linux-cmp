/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 *
 *  2003-06-02  Jim Houston - Concurrent Computer Corp.
 *		Changes to use preallocated sigqueue structures
 *		to allow signals to be sent reliably.
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/binfmts.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/ptrace.h>
#include <linux/posix-timers.h>
#include <linux/signal.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <asm/param.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/siginfo.h>

/*
 * SLAB caches for signal bits.
 */

static kmem_cache_t *sigqueue_cachep;

/*
 * In POSIX a signal is sent either to a specific thread (Linux task)
 * or to the process as a whole (Linux thread group).  How the signal
 * is sent determines whether it's to one thread or the whole group,
 * which determines which signal mask(s) are involved in blocking it
 * from being delivered until later.  When the signal is delivered,
 * either it's caught or ignored by a user handler or it has a default
 * effect that applies to the whole thread group (POSIX process).
 *
 * The possible effects an unblocked signal set to SIG_DFL can have are:
 *   ignore	- Nothing Happens
 *   terminate	- kill the process, i.e. all threads in the group,
 * 		  similar to exit_group.  The group leader (only) reports
 *		  WIFSIGNALED status to its parent.
 *   coredump	- write a core dump file describing all threads using
 *		  the same mm and then kill all those threads
 *   stop 	- stop all the threads in the group, i.e. TASK_STOPPED state
 *
 * SIGKILL and SIGSTOP cannot be caught, blocked, or ignored.
 * Other signals when not blocked and set to SIG_DFL behaves as follows.
 * The job control signals also have other special effects.
 *
 *	+--------------------+------------------+
 *	|  POSIX signal      |  default action  |
 *	+--------------------+------------------+
 *	|  SIGHUP            |  terminate	|
 *	|  SIGINT            |	terminate	|
 *	|  SIGQUIT           |	coredump 	|
 *	|  SIGILL            |	coredump 	|
 *	|  SIGTRAP           |	coredump 	|
 *	|  SIGABRT/SIGIOT    |	coredump 	|
 *	|  SIGBUS            |	coredump 	|
 *	|  SIGFPE            |	coredump 	|
 *	|  SIGKILL           |	terminate(+)	|
 *	|  SIGUSR1           |	terminate	|
 *	|  SIGSEGV           |	coredump 	|
 *	|  SIGUSR2           |	terminate	|
 *	|  SIGPIPE           |	terminate	|
 *	|  SIGALRM           |	terminate	|
 *	|  SIGTERM           |	terminate	|
 *	|  SIGCHLD           |	ignore   	|
 *	|  SIGCONT           |	ignore(*)	|
 *	|  SIGSTOP           |	stop(*)(+)  	|
 *	|  SIGTSTP           |	stop(*)  	|
 *	|  SIGTTIN           |	stop(*)  	|
 *	|  SIGTTOU           |	stop(*)  	|
 *	|  SIGURG            |	ignore   	|
 *	|  SIGXCPU           |	coredump 	|
 *	|  SIGXFSZ           |	coredump 	|
 *	|  SIGVTALRM         |	terminate	|
 *	|  SIGPROF           |	terminate	|
 *	|  SIGPOLL/SIGIO     |	terminate	|
 *	|  SIGSYS/SIGUNUSED  |	coredump 	|
 *	|  SIGSTKFLT         |	terminate	|
 *	|  SIGWINCH          |	ignore   	|
 *	|  SIGPWR            |	terminate	|
 *	|  SIGRTMIN-SIGRTMAX |	terminate       |
 *	+--------------------+------------------+
 *	|  non-POSIX signal  |  default action  |
 *	+--------------------+------------------+
 *	|  SIGEMT            |  coredump	|
 *	+--------------------+------------------+
 *
 * (+) For SIGKILL and SIGSTOP the action is "always", not just "default".
 * (*) Special job control effects:
 * When SIGCONT is sent, it resumes the process (all threads in the group)
 * from TASK_STOPPED state and also clears any pending/queued stop signals
 * (any of those marked with "stop(*)").  This happens regardless of blocking,
 * catching, or ignoring SIGCONT.  When any stop signal is sent, it clears
 * any pending/queued SIGCONT signals; this happens regardless of blocking,
 * catching, or ignored the stop signal, though (except for SIGSTOP) the
 * default action of stopping the process may happen later or never.
 */

#ifdef SIGEMT
#define M_SIGEMT	M(SIGEMT)
#else
#define M_SIGEMT	0
#endif

#if SIGRTMIN > BITS_PER_LONG
#define M(sig) (1ULL << ((sig)-1))
#else
#define M(sig) (1UL << ((sig)-1))
#endif
#define T(sig, mask) (M(sig) & (mask))

#define SIG_KERNEL_ONLY_MASK (\
	M(SIGKILL)   |  M(SIGSTOP)                                   )

#define SIG_KERNEL_STOP_MASK (\
	M(SIGSTOP)   |  M(SIGTSTP)   |  M(SIGTTIN)   |  M(SIGTTOU)   )

#define SIG_KERNEL_COREDUMP_MASK (\
        M(SIGQUIT)   |  M(SIGILL)    |  M(SIGTRAP)   |  M(SIGABRT)   | \
        M(SIGFPE)    |  M(SIGSEGV)   |  M(SIGBUS)    |  M(SIGSYS)    | \
        M(SIGXCPU)   |  M(SIGXFSZ)   |  M_SIGEMT                     )

#define SIG_KERNEL_IGNORE_MASK (\
        M(SIGCONT)   |  M(SIGCHLD)   |  M(SIGWINCH)  |  M(SIGURG)    )

#define sig_kernel_only(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_ONLY_MASK))
#define sig_kernel_coredump(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_COREDUMP_MASK))
#define sig_kernel_ignore(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_IGNORE_MASK))
#define sig_kernel_stop(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_STOP_MASK))

#define sig_user_defined(t, signr) \
	(((t)->sighand->action[(signr)-1].sa.sa_handler != SIG_DFL) &&	\
	 ((t)->sighand->action[(signr)-1].sa.sa_handler != SIG_IGN))

#define sig_fatal(t, signr) \
	(!T(signr, SIG_KERNEL_IGNORE_MASK|SIG_KERNEL_STOP_MASK) && \
	 (t)->sighand->action[(signr)-1].sa.sa_handler == SIG_DFL)

static int sig_ignored(struct task_struct *t, int sig)
{
	void __user * handler;

	/*
	 * Tracers always want to know about signals..
	 */
	if (t->ptrace & PT_PTRACED)
		return 0;

	/*
	 * Blocked signals are never ignored, since the
	 * signal handler may change by the time it is
	 * unblocked.
	 */
	if (sigismember(&t->blocked, sig))
		return 0;

	/* Is it explicitly or implicitly ignored? */
	handler = t->sighand->action[sig-1].sa.sa_handler;
	return   handler == SIG_IGN ||
		(handler == SIG_DFL && sig_kernel_ignore(sig));
}

/*
 * Re-calculate pending state from the set of locally pending
 * signals, globally pending signals, and blocked signals.
 */
static inline int has_pending_signals(sigset_t *signal, sigset_t *blocked)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= signal->sig[i] &~ blocked->sig[i];
		break;

	case 4: ready  = signal->sig[3] &~ blocked->sig[3];
		ready |= signal->sig[2] &~ blocked->sig[2];
		ready |= signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 2: ready  = signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 1: ready  = signal->sig[0] &~ blocked->sig[0];
	}
	return ready !=	0;
}

#define PENDING(p,b) has_pending_signals(&(p)->signal, (b))

fastcall void recalc_sigpending_tsk(struct task_struct *t)
{
	if (t->signal->group_stop_count > 0 ||
	    (freezing(t)) ||
	    PENDING(&t->pending, &t->blocked) ||
	    PENDING(&t->signal->shared_pending, &t->blocked))
		set_tsk_thread_flag(t, TIF_SIGPENDING);
	else
		clear_tsk_thread_flag(t, TIF_SIGPENDING);
}

void recalc_sigpending(void)
{
	recalc_sigpending_tsk(current);
}

/* Given the mask, find the first available signal that should be serviced. */

static int
next_signal(struct sigpending *pending, sigset_t *mask)
{
	unsigned long i, *s, *m, x;
	int sig = 0;
	
	s = pending->signal.sig;
	m = mask->sig;
	switch (_NSIG_WORDS) {
	default:
		for (i = 0; i < _NSIG_WORDS; ++i, ++s, ++m)
			if ((x = *s &~ *m) != 0) {
				sig = ffz(~x) + i*_NSIG_BPW + 1;
				break;
			}
		break;

	case 2: if ((x = s[0] &~ m[0]) != 0)
			sig = 1;
		else if ((x = s[1] &~ m[1]) != 0)
			sig = _NSIG_BPW + 1;
		else
			break;
		sig += ffz(~x);
		break;

	case 1: if ((x = *s &~ *m) != 0)
			sig = ffz(~x) + 1;
		break;
	}
	
	return sig;
}

static struct sigqueue *__sigqueue_alloc(struct task_struct *t, gfp_t flags,
					 int override_rlimit)
{
	struct sigqueue *q = NULL;

	atomic_inc(&t->user->sigpending);
	if (override_rlimit ||
	    atomic_read(&t->user->sigpending) <=
			t->signal->rlim[RLIMIT_SIGPENDING].rlim_cur)
		q = kmem_cache_alloc(sigqueue_cachep, flags);
	if (unlikely(q == NULL)) {
		atomic_dec(&t->user->sigpending);
	} else {
		INIT_LIST_HEAD(&q->list);
		q->flags = 0;
		q->user = get_uid(t->user);
	}
	return(q);
}

static inline void __sigqueue_free(struct sigqueue *q)
{
	if (q->flags & SIGQUEUE_PREALLOC)
		return;
	atomic_dec(&q->user->sigpending);
	free_uid(q->user);
	kmem_cache_free(sigqueue_cachep, q);
}

static void flush_sigqueue(struct sigpending *queue)
{
	struct sigqueue *q;

	sigemptyset(&queue->signal);
	while (!list_empty(&queue->list)) {
		q = list_entry(queue->list.next, struct sigqueue , list);
		list_del_init(&q->list);
		__sigqueue_free(q);
	}
}

/*
 * Flush all pending signals for a task.
 */

void
flush_signals(struct task_struct *t)
{
	unsigned long flags;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	clear_tsk_thread_flag(t,TIF_SIGPENDING);
	flush_sigqueue(&t->pending);
	flush_sigqueue(&t->signal->shared_pending);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);
}

/*
 * This function expects the tasklist_lock write-locked.
 */
void __exit_sighand(struct task_struct *tsk)
{
	struct sighand_struct * sighand = tsk->sighand;

	/* Ok, we're done with the signal handlers */
	tsk->sighand = NULL;
	if (atomic_dec_and_test(&sighand->count))
		sighand_free(sighand);
}

void exit_sighand(struct task_struct *tsk)
{
	write_lock_irq(&tasklist_lock);
	rcu_read_lock();
	if (tsk->sighand != NULL) {
		struct sighand_struct *sighand = rcu_dereference(tsk->sighand);
		spin_lock(&sighand->siglock);
		__exit_sighand(tsk);
		spin_unlock(&sighand->siglock);
	}
	rcu_read_unlock();
	write_unlock_irq(&tasklist_lock);
}

/*
 * This function expects the tasklist_lock write-locked.
 */
void __exit_signal(struct task_struct *tsk)
{
	struct signal_struct * sig = tsk->signal;
	struct sighand_struct * sighand;

	if (!sig)
		BUG();
	if (!atomic_read(&sig->count))
		BUG();
	rcu_read_lock();
	sighand = rcu_dereference(tsk->sighand);
	spin_lock(&sighand->siglock);
	posix_cpu_timers_exit(tsk);
	if (atomic_dec_and_test(&sig->count)) {
		posix_cpu_timers_exit_group(tsk);
		tsk->signal = NULL;
		__exit_sighand(tsk);
		spin_unlock(&sighand->siglock);
		flush_sigqueue(&sig->shared_pending);
	} else {
		/*
		 * If there is any task waiting for the group exit
		 * then notify it:
		 */
		if (sig->group_exit_task && atomic_read(&sig->count) == sig->notify_count) {
			wake_up_process(sig->group_exit_task);
			sig->group_exit_task = NULL;
		}
		if (tsk == sig->curr_target)
			sig->curr_target = next_thread(tsk);
		tsk->signal = NULL;
		/*
		 * Accumulate here the counters for all threads but the
		 * group leader as they die, so they can be added into
		 * the process-wide totals when those are taken.
		 * The group leader stays around as a zombie as long
		 * as there are other threads.  When it gets reaped,
		 * the exit.c code will add its counts into these totals.
		 * We won't ever get here for the group leader, since it
		 * will have been the last reference on the signal_struct.
		 */
		sig->utime = cputime_add(sig->utime, tsk->utime);
		sig->stime = cputime_add(sig->stime, tsk->stime);
		sig->min_flt += tsk->min_flt;
		sig->maj_flt += tsk->maj_flt;
		sig->nvcsw += tsk->nvcsw;
		sig->nivcsw += tsk->nivcsw;
		sig->sched_time += tsk->sched_time;
		__exit_sighand(tsk);
		spin_unlock(&sighand->siglock);
		sig = NULL;	/* Marker for below.  */
	}
	rcu_read_unlock();
	clear_tsk_thread_flag(tsk,TIF_SIGPENDING);
	flush_sigqueue(&tsk->pending);
	if (sig) {
		/*
		 * We are cleaning up the signal_struct here.
		 */
		exit_thread_group_keys(sig);
		kmem_cache_free(signal_cachep, sig);
	}
}

void exit_signal(struct task_struct *tsk)
{
	atomic_dec(&tsk->signal->live);

	write_lock_irq(&tasklist_lock);
	__exit_signal(tsk);
	write_unlock_irq(&tasklist_lock);
}

/*
 * Flush all handlers for a task.
 */

void
flush_signal_handlers(struct task_struct *t, int force_default)
{
	int i;
	struct k_sigaction *ka = &t->sighand->action[0];
	for (i = _NSIG ; i != 0 ; i--) {
		if (force_default || ka->sa.sa_handler != SIG_IGN)
			ka->sa.sa_handler = SIG_DFL;
		ka->sa.sa_flags = 0;
		sigemptyset(&ka->sa.sa_mask);
		ka++;
	}
}


/* Notify the system that a driver wants to block all signals for this
 * process, and wants to be notified if any signals at all were to be
 * sent/acted upon.  If the notifier routine returns non-zero, then the
 * signal will be acted upon after all.  If the notifier routine returns 0,
 * then then signal will be blocked.  Only one block per process is
 * allowed.  priv is a pointer to private data that the notifier routine
 * can use to determine if the signal should be blocked or not.  */

void
block_all_signals(int (*notifier)(void *priv), void *priv, sigset_t *mask)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	current->notifier_mask = mask;
	current->notifier_data = priv;
	current->notifier = notifier;
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}

/* Notify the system that blocking has ended. */

void
unblock_all_signals(void)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	current->notifier = NULL;
	current->notifier_data = NULL;
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}

static int collect_signal(int sig, struct sigpending *list, siginfo_t *info)
{
	struct sigqueue *q, *first = NULL;
	int still_pending = 0;

	if (unlikely(!sigismember(&list->signal, sig)))
		return 0;

	/*
	 * Collect the siginfo appropriate to this signal.  Check if
	 * there is another siginfo for the same signal.
	*/
	list_for_each_entry(q, &list->list, list) {
		if (q->info.si_signo == sig) {
			if (first) {
				still_pending = 1;
				break;
			}
			first = q;
		}
	}
	if (first) {
		list_del_init(&first->list);
		copy_siginfo(info, &first->info);
		__sigqueue_free(first);
		if (!still_pending)
			sigdelset(&list->signal, sig);
	} else {

		/* Ok, it wasn't in the queue.  This must be
		   a fast-pathed signal or we must have been
		   out of queue space.  So zero out the info.
		 */
		sigdelset(&list->signal, sig);
		info->si_signo = sig;
		info->si_errno = 0;
		info->si_code = 0;
		info->si_pid = 0;
		info->si_uid = 0;
	}
	return 1;
}

static int __dequeue_signal(struct sigpending *pending, sigset_t *mask,
			siginfo_t *info)
{
	int sig = 0;

	sig = next_signal(pending, mask);
	if (sig) {
		if (current->notifier) {
			if (sigismember(current->notifier_mask, sig)) {
				if (!(current->notifier)(current->notifier_data)) {
					clear_thread_flag(TIF_SIGPENDING);
					return 0;
				}
			}
		}

		if (!collect_signal(sig, pending, info))
			sig = 0;
				
	}
	recalc_sigpending();

	return sig;
}

/*
 * Dequeue a signal and return the element to the caller, which is 
 * expected to free it.
 *
 * All callers have to hold the siglock.
 */
int dequeue_signal(struct task_struct *tsk, sigset_t *mask, siginfo_t *info)
{
	int signr = __dequeue_signal(&tsk->pending, mask, info);
	if (!signr)
		signr = __dequeue_signal(&tsk->signal->shared_pending,
					 mask, info);
 	if (signr && unlikely(sig_kernel_stop(signr))) {
 		/*
 		 * Set a marker that we have dequeued a stop signal.  Our
 		 * caller might release the siglock and then the pending
 		 * stop signal it is about to process is no longer in the
 		 * pending bitmasks, but must still be cleared by a SIGCONT
 		 * (and overruled by a SIGKILL).  So those cases clear this
 		 * shared flag after we've set it.  Note that this flag may
 		 * remain set after the signal we return is ignored or
 		 * handled.  That doesn't matter because its only purpose
 		 * is to alert stop-signal processing code when another
 		 * processor has come along and cleared the flag.
 		 */
 		if (!(tsk->signal->flags & SIGNAL_GROUP_EXIT))
 			tsk->signal->flags |= SIGNAL_STOP_DEQUEUED;
 	}
	if ( signr &&
	     ((info->si_code & __SI_MASK) == __SI_TIMER) &&
	     info->si_sys_private){
		/*
		 * Release the siglock to ensure proper locking order
		 * of timer locks outside of siglocks.  Note, we leave
		 * irqs disabled here, since the posix-timers code is
		 * about to disable them again anyway.
		 */
		spin_unlock(&tsk->sighand->siglock);
		do_schedule_next_timer(info);
		spin_lock(&tsk->sighand->siglock);
	}
	return signr;
}

/*
 * Tell a process that it has a new active signal..
 *
 * NOTE! we rely on the previous spin_lock to
 * lock interrupts for us! We can only be called with
 * "siglock" held, and the local interrupt must
 * have been disabled when that got acquired!
 *
 * No need to set need_resched since signal event passing
 * goes through ->blocked
 */
void signal_wake_up(struct task_struct *t, int resume)
{
	unsigned int mask;

	set_tsk_thread_flag(t, TIF_SIGPENDING);

	/*
	 * For SIGKILL, we want to wake it up in the stopped/traced case.
	 * We don't check t->state here because there is a race with it
	 * executing another processor and just now entering stopped state.
	 * By using wake_up_state, we ensure the process will wake up and
	 * handle its death signal.
	 */
	mask = TASK_INTERRUPTIBLE;
	if (resume)
		mask |= TASK_STOPPED | TASK_TRACED;
	if (!wake_up_state(t, mask))
		kick_process(t);
}

/*
 * Remove signals in mask from the pending set and queue.
 * Returns 1 if any signals were found.
 *
 * All callers must be holding the siglock.
 *
 * This version takes a sigset mask and looks at all signals,
 * not just those in the first mask word.
 */
static int rm_from_queue_full(sigset_t *mask, struct sigpending *s)
{
	struct sigqueue *q, *n;
	sigset_t m;

	sigandsets(&m, mask, &s->signal);
	if (sigisemptyset(&m))
		return 0;

	signandsets(&s->signal, &s->signal, mask);
	list_for_each_entry_safe(q, n, &s->list, list) {
		if (sigismember(mask, q->info.si_signo)) {
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}
	return 1;
}
/*
 * Remove signals in mask from the pending set and queue.
 * Returns 1 if any signals were found.
 *
 * All callers must be holding the siglock.
 */
static int rm_from_queue(unsigned long mask, struct sigpending *s)
{
	struct sigqueue *q, *n;

	if (!sigtestsetmask(&s->signal, mask))
		return 0;

	sigdelsetmask(&s->signal, mask);
	list_for_each_entry_safe(q, n, &s->list, list) {
		if (q->info.si_signo < SIGRTMIN &&
		    (mask & sigmask(q->info.si_signo))) {
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}
	return 1;
}

/*
 * Bad permissions for sending the signal
 */
static int check_kill_permission(int sig, struct siginfo *info,
				 struct task_struct *t)
{
	int error = -EINVAL;
	if (!valid_signal(sig))
		return error;
	error = -EPERM;
	if ((info == SEND_SIG_NOINFO || (!is_si_special(info) && SI_FROMUSER(info)))
	    && ((sig != SIGCONT) ||
		(current->signal->session != t->signal->session))
	    && (current->euid ^ t->suid) && (current->euid ^ t->uid)
	    && (current->uid ^ t->suid) && (current->uid ^ t->uid)
	    && !capable(CAP_KILL))
		return error;

	error = security_task_kill(t, info, sig);
	if (!error)
		audit_signal_info(sig, t); /* Let audit system see the signal */
	return error;
}

/* forward decl */
static void do_notify_parent_cldstop(struct task_struct *tsk,
				     int to_self,
				     int why);

/*
 * Handle magic process-wide effects of stop/continue signals.
 * Unlike the signal actions, these happen immediately at signal-generation
 * time regardless of blocking, ignoring, or handling.  This does the
 * actual continuing for SIGCONT, but not the actual stopping for stop
 * signals.  The process stop is done as a signal action for SIG_DFL.
 */
static void handle_stop_signal(int sig, struct task_struct *p)
{
	struct task_struct *t;

	if (p->signal->flags & SIGNAL_GROUP_EXIT)
		/*
		 * The process is in the middle of dying already.
		 */
		return;

	if (sig_kernel_stop(sig)) {
		/*
		 * This is a stop signal.  Remove SIGCONT from all queues.
		 */
		rm_from_queue(sigmask(SIGCONT), &p->signal->shared_pending);
		t = p;
		do {
			rm_from_queue(sigmask(SIGCONT), &t->pending);
			t = next_thread(t);
		} while (t != p);
	} else if (sig == SIGCONT) {
		/*
		 * Remove all stop signals from all queues,
		 * and wake all threads.
		 */
		if (unlikely(p->signal->group_stop_count > 0)) {
			/*
			 * There was a group stop in progress.  We'll
			 * pretend it finished before we got here.  We are
			 * obliged to report it to the parent: if the
			 * SIGSTOP happened "after" this SIGCONT, then it
			 * would have cleared this pending SIGCONT.  If it
			 * happened "before" this SIGCONT, then the parent
			 * got the SIGCHLD about the stop finishing before
			 * the continue happened.  We do the notification
			 * now, and it's as if the stop had finished and
			 * the SIGCHLD was pending on entry to this kill.
			 */
			p->signal->group_stop_count = 0;
			p->signal->flags = SIGNAL_STOP_CONTINUED;
			spin_unlock(&p->sighand->siglock);
			do_notify_parent_cldstop(p, (p->ptrace & PT_PTRACED), CLD_STOPPED);
			spin_lock(&p->sighand->siglock);
		}
		rm_from_queue(SIG_KERNEL_STOP_MASK, &p->signal->shared_pending);
		t = p;
		do {
			unsigned int state;
			rm_from_queue(SIG_KERNEL_STOP_MASK, &t->pending);
			
			/*
			 * If there is a handler for SIGCONT, we must make
			 * sure that no thread returns to user mode before
			 * we post the signal, in case it was the only
			 * thread eligible to run the signal handler--then
			 * it must not do anything between resuming and
			 * running the handler.  With the TIF_SIGPENDING
			 * flag set, the thread will pause and acquire the
			 * siglock that we hold now and until we've queued
			 * the pending signal. 
			 *
			 * Wake up the stopped thread _after_ setting
			 * TIF_SIGPENDING
			 */
			state = TASK_STOPPED;
			if (sig_user_defined(t, SIGCONT) && !sigismember(&t->blocked, SIGCONT)) {
				set_tsk_thread_flag(t, TIF_SIGPENDING);
				state |= TASK_INTERRUPTIBLE;
			}
			wake_up_state(t, state);

			t = next_thread(t);
		} while (t != p);

		if (p->signal->flags & SIGNAL_STOP_STOPPED) {
			/*
			 * We were in fact stopped, and are now continued.
			 * Notify the parent with CLD_CONTINUED.
			 */
			p->signal->flags = SIGNAL_STOP_CONTINUED;
			p->signal->group_exit_code = 0;
			spin_unlock(&p->sighand->siglock);
			do_notify_parent_cldstop(p, (p->ptrace & PT_PTRACED), CLD_CONTINUED);
			spin_lock(&p->sighand->siglock);
		} else {
			/*
			 * We are not stopped, but there could be a stop
			 * signal in the middle of being processed after
			 * being removed from the queue.  Clear that too.
			 */
			p->signal->flags = 0;
		}
	} else if (sig == SIGKILL) {
		/*
		 * Make sure that any pending stop signal already dequeued
		 * is undone by the wakeup for SIGKILL.
		 */
		p->signal->flags = 0;
	}
}

static int send_signal(int sig, struct siginfo *info, struct task_struct *t,
			struct sigpending *signals)
{
	struct sigqueue * q = NULL;
	int ret = 0;

	/*
	 * fast-pathed signals for kernel-internal things like SIGSTOP
	 * or SIGKILL.
	 */
	if (info == SEND_SIG_FORCED)
		goto out_set;

	/* Real-time signals must be queued if sent by sigqueue, or
	   some other real-time mechanism.  It is implementation
	   defined whether kill() does so.  We attempt to do so, on
	   the principle of least surprise, but since kill is not
	   allowed to fail with EAGAIN when low on memory we just
	   make sure at least one signal gets delivered and don't
	   pass on the info struct.  */

	q = __sigqueue_alloc(t, GFP_ATOMIC, (sig < SIGRTMIN &&
					     (is_si_special(info) ||
					      info->si_code >= 0)));
	if (q) {
		list_add_tail(&q->list, &signals->list);
		switch ((unsigned long) info) {
		case (unsigned long) SEND_SIG_NOINFO:
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_USER;
			q->info.si_pid = current->pid;
			q->info.si_uid = current->uid;
			break;
		case (unsigned long) SEND_SIG_PRIV:
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_KERNEL;
			q->info.si_pid = 0;
			q->info.si_uid = 0;
			break;
		default:
			copy_siginfo(&q->info, info);
			break;
		}
	} else if (!is_si_special(info)) {
		if (sig >= SIGRTMIN && info->si_code != SI_USER)
		/*
		 * Queue overflow, abort.  We may abort if the signal was rt
		 * and sent by user using something other than kill().
		 */
			return -EAGAIN;
	}

out_set:
	sigaddset(&signals->signal, sig);
	return ret;
}

#define LEGACY_QUEUE(sigptr, sig) \
	(((sig) < SIGRTMIN) && sigismember(&(sigptr)->signal, (sig)))


static int
specific_send_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	int ret = 0;

	if (!irqs_disabled())
		BUG();
	assert_spin_locked(&t->sighand->siglock);

	/* Short-circuit ignored signals.  */
	if (sig_ignored(t, sig))
		goto out;

	/* Support queueing exactly one non-rt signal, so that we
	   can get more detailed information about the cause of
	   the signal. */
	if (LEGACY_QUEUE(&t->pending, sig))
		goto out;

	ret = send_signal(sig, info, t, &t->pending);
	if (!ret && !sigismember(&t->blocked, sig))
		signal_wake_up(t, sig == SIGKILL);
out:
	return ret;
}

/*
 * Force a signal that the process can't ignore: if necessary
 * we unblock the signal and change any SIG_IGN to SIG_DFL.
 */

int
force_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	unsigned long int flags;
	int ret;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	if (t->sighand->action[sig-1].sa.sa_handler == SIG_IGN) {
		t->sighand->action[sig-1].sa.sa_handler = SIG_DFL;
	}
	if (sigismember(&t->blocked, sig)) {
		sigdelset(&t->blocked, sig);
	}
	recalc_sigpending_tsk(t);
	ret = specific_send_sig_info(sig, info, t);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);

	return ret;
}

void
force_sig_specific(int sig, struct task_struct *t)
{
	force_sig_info(sig, SEND_SIG_FORCED, t);
}

/*
 * Test if P wants to take SIG.  After we've checked all threads with this,
 * it's equivalent to finding no threads not blocking SIG.  Any threads not
 * blocking SIG were ruled out because they are not running and already
 * have pending signals.  Such threads will dequeue from the shared queue
 * as soon as they're available, so putting the signal on the shared queue
 * will be equivalent to sending it to one such thread.
 */
static inline int wants_signal(int sig, struct task_struct *p)
{
	if (sigismember(&p->blocked, sig))
		return 0;
	if (p->flags & PF_EXITING)
		return 0;
	if (sig == SIGKILL)
		return 1;
	if (p->state & (TASK_STOPPED | TASK_TRACED))
		return 0;
	return task_curr(p) || !signal_pending(p);
}

static void
__group_complete_signal(int sig, struct task_struct *p)
{
	struct task_struct *t;

	/*
	 * Now find a thread we can wake up to take the signal off the queue.
	 *
	 * If the main thread wants the signal, it gets first crack.
	 * Probably the least surprising to the average bear.
	 */
	if (wants_signal(sig, p))
		t = p;
	else if (thread_group_empty(p))
		/*
		 * There is just one thread and it does not need to be woken.
		 * It will dequeue unblocked signals before it runs again.
		 */
		return;
	else {
		/*
		 * Otherwise try to find a suitable thread.
		 */
		t = p->signal->curr_target;
		if (t == NULL)
			/* restart balancing at this thread */
			t = p->signal->curr_target = p;
		BUG_ON(t->tgid != p->tgid);

		while (!wants_signal(sig, t)) {
			t = next_thread(t);
			if (t == p->signal->curr_target)
				/*
				 * No thread needs to be woken.
				 * Any eligible threads will see
				 * the signal in the queue soon.
				 */
				return;
		}
		p->signal->curr_target = t;
	}

	/*
	 * Found a killable thread.  If the signal will be fatal,
	 * then start taking the whole group down immediately.
	 */
	if (sig_fatal(p, sig) && !(p->signal->flags & SIGNAL_GROUP_EXIT) &&
	    !sigismember(&t->real_blocked, sig) &&
	    (sig == SIGKILL || !(t->ptrace & PT_PTRACED))) {
		/*
		 * This signal will be fatal to the whole group.
		 */
		if (!sig_kernel_coredump(sig)) {
			/*
			 * Start a group exit and wake everybody up.
			 * This way we don't have other threads
			 * running and doing things after a slower
			 * thread has the fatal signal pending.
			 */
			p->signal->flags = SIGNAL_GROUP_EXIT;
			p->signal->group_exit_code = sig;
			p->signal->group_stop_count = 0;
			t = p;
			do {
				sigaddset(&t->pending.signal, SIGKILL);
				signal_wake_up(t, 1);
				t = next_thread(t);
			} while (t != p);
			return;
		}

		/*
		 * There will be a core dump.  We make all threads other
		 * than the chosen one go into a group stop so that nothing
		 * happens until it gets scheduled, takes the signal off
		 * the shared queue, and does the core dump.  This is a
		 * little more complicated than strictly necessary, but it
		 * keeps the signal state that winds up in the core dump
		 * unchanged from the death state, e.g. which thread had
		 * the core-dump signal unblocked.
		 */
		rm_from_queue(SIG_KERNEL_STOP_MASK, &t->pending);
		rm_from_queue(SIG_KERNEL_STOP_MASK, &p->signal->shared_pending);
		p->signal->group_stop_count = 0;
		p->signal->group_exit_task = t;
		t = p;
		do {
			p->signal->group_stop_count++;
			signal_wake_up(t, 0);
			t = next_thread(t);
		} while (t != p);
		wake_up_process(p->signal->group_exit_task);
		return;
	}

	/*
	 * The signal is already in the shared-pending queue.
	 * Tell the chosen thread to wake up and dequeue it.
	 */
	signal_wake_up(t, sig == SIGKILL);
	return;
}

int
__group_send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret = 0;

	assert_spin_locked(&p->sighand->siglock);
	handle_stop_signal(sig, p);

	/* Short-circuit ignored signals.  */
	if (sig_ignored(p, sig))
		return ret;

	if (LEGACY_QUEUE(&p->signal->shared_pending, sig))
		/* This is a non-RT signal and we already have one queued.  */
		return ret;

	/*
	 * Put this signal on the shared-pending queue, or fail with EAGAIN.
	 * We always use the shared queue for process-wide signals,
	 * to avoid several races.
	 */
	ret = send_signal(sig, info, p, &p->signal->shared_pending);
	if (unlikely(ret))
		return ret;

	__group_complete_signal(sig, p);
	return 0;
}

/*
 * Nuke all other threads in the group.
 */
void zap_other_threads(struct task_struct *p)
{
	struct task_struct *t;

	p->signal->flags = SIGNAL_GROUP_EXIT;
	p->signal->group_stop_count = 0;

	if (thread_group_empty(p))
		return;

	for (t = next_thread(p); t != p; t = next_thread(t)) {
		/*
		 * Don't bother with already dead threads
		 */
		if (t->exit_state)
			continue;

		/*
		 * We don't want to notify the parent, since we are
		 * killed as part of a thread group due to another
		 * thread doing an execve() or similar. So set the
		 * exit signal to -1 to allow immediate reaping of
		 * the process.  But don't detach the thread group
		 * leader.
		 */
		if (t != p->group_leader)
			t->exit_signal = -1;

		/* SIGKILL will be handled before any pending SIGSTOP */
		sigaddset(&t->pending.signal, SIGKILL);
		signal_wake_up(t, 1);
	}
}

/*
 * Must be called under rcu_read_lock() or with tasklist_lock read-held.
 */
int group_send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	unsigned long flags;
	struct sighand_struct *sp;
	int ret;

retry:
	ret = check_kill_permission(sig, info, p);
	if (!ret && sig && (sp = rcu_dereference(p->sighand))) {
		spin_lock_irqsave(&sp->siglock, flags);
		if (p->sighand != sp) {
			spin_unlock_irqrestore(&sp->siglock, flags);
			goto retry;
		}
		if ((atomic_read(&sp->count) == 0) ||
				(atomic_read(&p->usage) == 0)) {
			spin_unlock_irqrestore(&sp->siglock, flags);
			return -ESRCH;
		}
		ret = __group_send_sig_info(sig, info, p);
		spin_unlock_irqrestore(&sp->siglock, flags);
	}

	return ret;
}

/*
 * kill_pg_info() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */

int __kill_pg_info(int sig, struct siginfo *info, pid_t pgrp)
{
	struct task_struct *p = NULL;
	int retval, success;

	if (pgrp <= 0)
		return -EINVAL;

	success = 0;
	retval = -ESRCH;
	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		int err = group_send_sig_info(sig, info, p);
		success |= !err;
		retval = err;
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return success ? 0 : retval;
}

int
kill_pg_info(int sig, struct siginfo *info, pid_t pgrp)
{
	int retval;

	read_lock(&tasklist_lock);
	retval = __kill_pg_info(sig, info, pgrp);
	read_unlock(&tasklist_lock);

	return retval;
}

int
kill_proc_info(int sig, struct siginfo *info, pid_t pid)
{
	int error;
	int acquired_tasklist_lock = 0;
	struct task_struct *p;

	rcu_read_lock();
	if (unlikely(sig_kernel_stop(sig) || sig == SIGCONT)) {
		read_lock(&tasklist_lock);
		acquired_tasklist_lock = 1;
	}
	p = find_task_by_pid(pid);
	error = -ESRCH;
	if (p)
		error = group_send_sig_info(sig, info, p);
	if (unlikely(acquired_tasklist_lock))
		read_unlock(&tasklist_lock);
	rcu_read_unlock();
	return error;
}

/* like kill_proc_info(), but doesn't use uid/euid of "current" */
int kill_proc_info_as_uid(int sig, struct siginfo *info, pid_t pid,
		      uid_t uid, uid_t euid)
{
	int ret = -EINVAL;
	struct task_struct *p;

	if (!valid_signal(sig))
		return ret;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	if (!p) {
		ret = -ESRCH;
		goto out_unlock;
	}
	if ((info == SEND_SIG_NOINFO || (!is_si_special(info) && SI_FROMUSER(info)))
	    && (euid != p->suid) && (euid != p->uid)
	    && (uid != p->suid) && (uid != p->uid)) {
		ret = -EPERM;
		goto out_unlock;
	}
	if (sig && p->sighand) {
		unsigned long flags;
		spin_lock_irqsave(&p->sighand->siglock, flags);
		ret = __group_send_sig_info(sig, info, p);
		spin_unlock_irqrestore(&p->sighand->siglock, flags);
	}
out_unlock:
	read_unlock(&tasklist_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kill_proc_info_as_uid);

/*
 * kill_something_info() interprets pid in interesting ways just like kill(2).
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */

static int kill_something_info(int sig, struct siginfo *info, int pid)
{
	if (!pid) {
		return kill_pg_info(sig, info, process_group(current));
	} else if (pid == -1) {
		int retval = 0, count = 0;
		struct task_struct * p;

		read_lock(&tasklist_lock);
		for_each_process(p) {
			if (p->pid > 1 && p->tgid != current->tgid) {
				int err = group_send_sig_info(sig, info, p);
				++count;
				if (err != -EPERM)
					retval = err;
			}
		}
		read_unlock(&tasklist_lock);
		return count ? retval : -ESRCH;
	} else if (pid < 0) {
		return kill_pg_info(sig, info, -pid);
	} else {
		return kill_proc_info(sig, info, pid);
	}
}

/*
 * These are for backward compatibility with the rest of the kernel source.
 */

/*
 * These two are the most common entry points.  They send a signal
 * just to the specific thread.
 */
int
send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret;
	unsigned long flags;

	/*
	 * Make sure legacy kernel users don't send in bad values
	 * (normal paths check this in check_kill_permission).
	 */
	if (!valid_signal(sig))
		return -EINVAL;

	/*
	 * We need the tasklist lock even for the specific
	 * thread case (when we don't need to follow the group
	 * lists) in order to avoid races with "p->sighand"
	 * going away or changing from under us.
	 */
	read_lock(&tasklist_lock);  
	spin_lock_irqsave(&p->sighand->siglock, flags);
	ret = specific_send_sig_info(sig, info, p);
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
	read_unlock(&tasklist_lock);
	return ret;
}

#define __si_special(priv) \
	((priv) ? SEND_SIG_PRIV : SEND_SIG_NOINFO)

int
send_sig(int sig, struct task_struct *p, int priv)
{
	return send_sig_info(sig, __si_special(priv), p);
}

/*
 * This is the entry point for "process-wide" signals.
 * They will go to an appropriate thread in the thread group.
 */
int
send_group_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret;
	read_lock(&tasklist_lock);
	ret = group_send_sig_info(sig, info, p);
	read_unlock(&tasklist_lock);
	return ret;
}

void
force_sig(int sig, struct task_struct *p)
{
	force_sig_info(sig, SEND_SIG_PRIV, p);
}

/*
 * When things go south during signal handling, we
 * will force a SIGSEGV. And if the signal that caused
 * the problem was already a SIGSEGV, we'll want to
 * make sure we don't even try to deliver the signal..
 */
int
force_sigsegv(int sig, struct task_struct *p)
{
	if (sig == SIGSEGV) {
		unsigned long flags;
		spin_lock_irqsave(&p->sighand->siglock, flags);
		p->sighand->action[sig - 1].sa.sa_handler = SIG_DFL;
		spin_unlock_irqrestore(&p->sighand->siglock, flags);
	}
	force_sig(SIGSEGV, p);
	return 0;
}

int
kill_pg(pid_t pgrp, int sig, int priv)
{
	return kill_pg_info(sig, __si_special(priv), pgrp);
}

int
kill_proc(pid_t pid, int sig, int priv)
{
	return kill_proc_info(sig, __si_special(priv), pid);
}

/*
 * These functions support sending signals using preallocated sigqueue
 * structures.  This is needed "because realtime applications cannot
 * afford to lose notifications of asynchronous events, like timer
 * expirations or I/O completions".  In the case of Posix Timers 
 * we allocate the sigqueue structure from the timer_create.  If this
 * allocation fails we are able to report the failure to the application
 * with an EAGAIN error.
 */
 
struct sigqueue *sigqueue_alloc(void)
{
	struct sigqueue *q;

	if ((q = __sigqueue_alloc(current, GFP_KERNEL, 0)))
		q->flags |= SIGQUEUE_PREALLOC;
	return(q);
}

void sigqueue_free(struct sigqueue *q)
{
	unsigned long flags;
	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));
	/*
	 * If the signal is still pending remove it from the
	 * pending queue.
	 */
	if (unlikely(!list_empty(&q->list))) {
		spinlock_t *lock = &current->sighand->siglock;
		read_lock(&tasklist_lock);
		spin_lock_irqsave(lock, flags);
		if (!list_empty(&q->list))
			list_del_init(&q->list);
		spin_unlock_irqrestore(lock, flags);
		read_unlock(&tasklist_lock);
	}
	q->flags &= ~SIGQUEUE_PREALLOC;
	__sigqueue_free(q);
}

int
send_sigqueue(int sig, struct sigqueue *q, struct task_struct *p)
{
	unsigned long flags;
	int ret = 0;
	struct sighand_struct *sh;

	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));

	/*
	 * The rcu based delayed sighand destroy makes it possible to
	 * run this without tasklist lock held. The task struct itself
	 * cannot go away as create_timer did get_task_struct().
	 *
	 * We return -1, when the task is marked exiting, so
	 * posix_timer_event can redirect it to the group leader
	 */
	rcu_read_lock();

	if (unlikely(p->flags & PF_EXITING)) {
		ret = -1;
		goto out_err;
	}

retry:
	sh = rcu_dereference(p->sighand);

	spin_lock_irqsave(&sh->siglock, flags);
	if (p->sighand != sh) {
		/* We raced with exec() in a multithreaded process... */
		spin_unlock_irqrestore(&sh->siglock, flags);
		goto retry;
	}

	/*
	 * We do the check here again to handle the following scenario:
	 *
	 * CPU 0		CPU 1
	 * send_sigqueue
	 * check PF_EXITING
	 * interrupt		exit code running
	 *			__exit_signal
	 *			lock sighand->siglock
	 *			unlock sighand->siglock
	 * lock sh->siglock
	 * add(tsk->pending) 	flush_sigqueue(tsk->pending)
	 *
	 */

	if (unlikely(p->flags & PF_EXITING)) {
		ret = -1;
		goto out;
	}

	if (unlikely(!list_empty(&q->list))) {
		/*
		 * If an SI_TIMER entry is already queue just increment
		 * the overrun count.
		 */
		if (q->info.si_code != SI_TIMER)
			BUG();
		q->info.si_overrun++;
		goto out;
	}
	/* Short-circuit ignored signals.  */
	if (sig_ignored(p, sig)) {
		ret = 1;
		goto out;
	}

	list_add_tail(&q->list, &p->pending.list);
	sigaddset(&p->pending.signal, sig);
	if (!sigismember(&p->blocked, sig))
		signal_wake_up(p, sig == SIGKILL);

out:
	spin_unlock_irqrestore(&sh->siglock, flags);
out_err:
	rcu_read_unlock();

	return ret;
}

int
send_group_sigqueue(int sig, struct sigqueue *q, struct task_struct *p)
{
	unsigned long flags;
	int ret = 0;

	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));

	read_lock(&tasklist_lock);
	/* Since it_lock is held, p->sighand cannot be NULL. */
	spin_lock_irqsave(&p->sighand->siglock, flags);
	handle_stop_signal(sig, p);

	/* Short-circuit ignored signals.  */
	if (sig_ignored(p, sig)) {
		ret = 1;
		goto out;
	}

	if (unlikely(!list_empty(&q->list))) {
		/*
		 * If an SI_TIMER entry is already queue just increment
		 * the overrun count.  Other uses should not try to
		 * send the signal multiple times.
		 */
		if (q->info.si_code != SI_TIMER)
			BUG();
		q->info.si_overrun++;
		goto out;
	} 

	/*
	 * Put this signal on the shared-pending queue.
	 * We always use the shared queue for process-wide signals,
	 * to avoid several races.
	 */
	list_add_tail(&q->list, &p->signal->shared_pending.list);
	sigaddset(&p->signal->shared_pending.signal, sig);

	__group_complete_signal(sig, p);
out:
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
	read_unlock(&tasklist_lock);
	return ret;
}

/*
 * Wake up any threads in the parent blocked in wait* syscalls.
 */
static inline void __wake_up_parent(struct task_struct *p,
				    struct task_struct *parent)
{
	wake_up_interruptible_sync(&parent->signal->wait_chldexit);
}

/*
 * Let a parent know about the death of a child.
 * For a stopped/continued status change, use do_notify_parent_cldstop instead.
 */

void do_notify_parent(struct task_struct *tsk, int sig)
{
	struct siginfo info;
	unsigned long flags;
	struct sighand_struct *psig;

	BUG_ON(sig == -1);

 	/* do_notify_parent_cldstop should have been called instead.  */
 	BUG_ON(tsk->state & (TASK_STOPPED|TASK_TRACED));

	BUG_ON(!tsk->ptrace &&
	       (tsk->group_leader != tsk || !thread_group_empty(tsk)));

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_pid = tsk->pid;
	info.si_uid = tsk->uid;

	/* FIXME: find out whether or not this is supposed to be c*time. */
	info.si_utime = cputime_to_jiffies(cputime_add(tsk->utime,
						       tsk->signal->utime));
	info.si_stime = cputime_to_jiffies(cputime_add(tsk->stime,
						       tsk->signal->stime));

	info.si_status = tsk->exit_code & 0x7f;
	if (tsk->exit_code & 0x80)
		info.si_code = CLD_DUMPED;
	else if (tsk->exit_code & 0x7f)
		info.si_code = CLD_KILLED;
	else {
		info.si_code = CLD_EXITED;
		info.si_status = tsk->exit_code >> 8;
	}

	psig = tsk->parent->sighand;
	spin_lock_irqsave(&psig->siglock, flags);
	if (!tsk->ptrace && sig == SIGCHLD &&
	    (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
	     (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
		/*
		 * We are exiting and our parent doesn't care.  POSIX.1
		 * defines special semantics for setting SIGCHLD to SIG_IGN
		 * or setting the SA_NOCLDWAIT flag: we should be reaped
		 * automatically and not left for our parent's wait4 call.
		 * Rather than having the parent do it as a magic kind of
		 * signal handler, we just set this to tell do_exit that we
		 * can be cleaned up without becoming a zombie.  Note that
		 * we still call __wake_up_parent in this case, because a
		 * blocked sys_wait4 might now return -ECHILD.
		 *
		 * Whether we send SIGCHLD or not for SA_NOCLDWAIT
		 * is implementation-defined: we do (if you don't want
		 * it, just use SIG_IGN instead).
		 */
		tsk->exit_signal = -1;
		if (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN)
			sig = 0;
	}
	if (valid_signal(sig) && sig > 0)
		__group_send_sig_info(sig, &info, tsk->parent);
	__wake_up_parent(tsk, tsk->parent);
	spin_unlock_irqrestore(&psig->siglock, flags);
}

static void do_notify_parent_cldstop(struct task_struct *tsk, int to_self, int why)
{
	struct siginfo info;
	unsigned long flags;
	struct task_struct *parent;
	struct sighand_struct *sighand;

	if (to_self)
		parent = tsk->parent;
	else {
		tsk = tsk->group_leader;
		parent = tsk->real_parent;
	}

	info.si_signo = SIGCHLD;
	info.si_errno = 0;
	info.si_pid = tsk->pid;
	info.si_uid = tsk->uid;

	/* FIXME: find out whether or not this is supposed to be c*time. */
	info.si_utime = cputime_to_jiffies(tsk->utime);
	info.si_stime = cputime_to_jiffies(tsk->stime);

 	info.si_code = why;
 	switch (why) {
 	case CLD_CONTINUED:
 		info.si_status = SIGCONT;
 		break;
 	case CLD_STOPPED:
 		info.si_status = tsk->signal->group_exit_code & 0x7f;
 		break;
 	case CLD_TRAPPED:
 		info.si_status = tsk->exit_code & 0x7f;
 		break;
 	default:
 		BUG();
 	}

	sighand = parent->sighand;
	spin_lock_irqsave(&sighand->siglock, flags);
	if (sighand->action[SIGCHLD-1].sa.sa_handler != SIG_IGN &&
	    !(sighand->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
		__group_send_sig_info(SIGCHLD, &info, parent);
	/*
	 * Even if SIGCHLD is not generated, we must wake up wait4 calls.
	 */
	__wake_up_parent(tsk, parent);
	spin_unlock_irqrestore(&sighand->siglock, flags);
}

/*
 * This must be called with current->sighand->siglock held.
 *
 * This should be the path for all ptrace stops.
 * We always set current->last_siginfo while stopped here.
 * That makes it a way to test a stopped process for
 * being ptrace-stopped vs being job-control-stopped.
 *
 * If we actually decide not to stop at all because the tracer is gone,
 * we leave nostop_code in current->exit_code.
 */
static void ptrace_stop(int exit_code, int nostop_code, siginfo_t *info)
{
	/*
	 * If there is a group stop in progress,
	 * we must participate in the bookkeeping.
	 */
	if (current->signal->group_stop_count > 0)
		--current->signal->group_stop_count;

	current->last_siginfo = info;
	current->exit_code = exit_code;

	/* Let the debugger run.  */
	set_current_state(TASK_TRACED);
	spin_unlock_irq(&current->sighand->siglock);
	read_lock(&tasklist_lock);
	if (likely(current->ptrace & PT_PTRACED) &&
	    likely(current->parent != current->real_parent ||
		   !(current->ptrace & PT_ATTACHED)) &&
	    (likely(current->parent->signal != current->signal) ||
	     !unlikely(current->signal->flags & SIGNAL_GROUP_EXIT))) {
		do_notify_parent_cldstop(current, 1, CLD_TRAPPED);
		read_unlock(&tasklist_lock);
		schedule();
	} else {
		/*
		 * By the time we got the lock, our tracer went away.
		 * Don't stop here.
		 */
		read_unlock(&tasklist_lock);
		set_current_state(TASK_RUNNING);
		current->exit_code = nostop_code;
	}

	/*
	 * We are back.  Now reacquire the siglock before touching
	 * last_siginfo, so that we are sure to have synchronized with
	 * any signal-sending on another CPU that wants to examine it.
	 */
	spin_lock_irq(&current->sighand->siglock);
	current->last_siginfo = NULL;

	/*
	 * Queued signals ignored us while we were stopped for tracing.
	 * So check for any that we should take before resuming user mode.
	 */
	recalc_sigpending();
}

void ptrace_notify(int exit_code)
{
	siginfo_t info;

	BUG_ON((exit_code & (0x7f | ~0xffff)) != SIGTRAP);

	memset(&info, 0, sizeof info);
	info.si_signo = SIGTRAP;
	info.si_code = exit_code;
	info.si_pid = current->pid;
	info.si_uid = current->uid;

	/* Let the debugger run.  */
	spin_lock_irq(&current->sighand->siglock);
	ptrace_stop(exit_code, 0, &info);
	spin_unlock_irq(&current->sighand->siglock);
}

static void
finish_stop(int stop_count)
{
	int to_self;

	/*
	 * If there are no other threads in the group, or if there is
	 * a group stop in progress and we are the last to stop,
	 * report to the parent.  When ptraced, every thread reports itself.
	 */
	if (stop_count < 0 || (current->ptrace & PT_PTRACED))
		to_self = 1;
	else if (stop_count == 0)
		to_self = 0;
	else
		goto out;

	read_lock(&tasklist_lock);
	do_notify_parent_cldstop(current, to_self, CLD_STOPPED);
	read_unlock(&tasklist_lock);

out:
	schedule();
	/*
	 * Now we don't run again until continued.
	 */
	current->exit_code = 0;
}

/*
 * This performs the stopping for SIGSTOP and other stop signals.
 * We have to stop all threads in the thread group.
 * Returns nonzero if we've actually stopped and released the siglock.
 * Returns zero if we didn't stop and still hold the siglock.
 */
static int
do_signal_stop(int signr)
{
	struct signal_struct *sig = current->signal;
	struct sighand_struct *sighand = current->sighand;
	int stop_count = -1;

	if (!likely(sig->flags & SIGNAL_STOP_DEQUEUED))
		return 0;

	if (sig->group_stop_count > 0) {
		/*
		 * There is a group stop in progress.  We don't need to
		 * start another one.
		 */
		signr = sig->group_exit_code;
		stop_count = --sig->group_stop_count;
		current->exit_code = signr;
		set_current_state(TASK_STOPPED);
		if (stop_count == 0)
			sig->flags = SIGNAL_STOP_STOPPED;
		spin_unlock_irq(&sighand->siglock);
	}
	else if (thread_group_empty(current)) {
		/*
		 * Lock must be held through transition to stopped state.
		 */
		current->exit_code = current->signal->group_exit_code = signr;
		set_current_state(TASK_STOPPED);
		sig->flags = SIGNAL_STOP_STOPPED;
		spin_unlock_irq(&sighand->siglock);
	}
	else {
		/*
		 * There is no group stop already in progress.
		 * We must initiate one now, but that requires
		 * dropping siglock to get both the tasklist lock
		 * and siglock again in the proper order.  Note that
		 * this allows an intervening SIGCONT to be posted.
		 * We need to check for that and bail out if necessary.
		 */
		struct task_struct *t;

		spin_unlock_irq(&sighand->siglock);

		/* signals can be posted during this window */

		read_lock(&tasklist_lock);
		spin_lock_irq(&sighand->siglock);

		if (!likely(sig->flags & SIGNAL_STOP_DEQUEUED)) {
			/*
			 * Another stop or continue happened while we
			 * didn't have the lock.  We can just swallow this
			 * signal now.  If we raced with a SIGCONT, that
			 * should have just cleared it now.  If we raced
			 * with another processor delivering a stop signal,
			 * then the SIGCONT that wakes us up should clear it.
			 */
			read_unlock(&tasklist_lock);
			return 0;
		}

		if (sig->group_stop_count == 0) {
			sig->group_exit_code = signr;
			stop_count = 0;
			for (t = next_thread(current); t != current;
			     t = next_thread(t))
				/*
				 * Setting state to TASK_STOPPED for a group
				 * stop is always done with the siglock held,
				 * so this check has no races.
				 */
				if (!t->exit_state &&
				    !(t->state & (TASK_STOPPED|TASK_TRACED))) {
					stop_count++;
					signal_wake_up(t, 0);
				}
			sig->group_stop_count = stop_count;
		}
		else {
			/* A race with another thread while unlocked.  */
			signr = sig->group_exit_code;
			stop_count = --sig->group_stop_count;
		}

		current->exit_code = signr;
		set_current_state(TASK_STOPPED);
		if (stop_count == 0)
			sig->flags = SIGNAL_STOP_STOPPED;

		spin_unlock_irq(&sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	finish_stop(stop_count);
	return 1;
}

/*
 * Do appropriate magic when group_stop_count > 0.
 * We return nonzero if we stopped, after releasing the siglock.
 * We return zero if we still hold the siglock and should look
 * for another signal without checking group_stop_count again.
 */
static int handle_group_stop(void)
{
	int stop_count;

	if (current->signal->group_exit_task == current) {
		/*
		 * Group stop is so we can do a core dump,
		 * We are the initiating thread, so get on with it.
		 */
		current->signal->group_exit_task = NULL;
		return 0;
	}

	if (current->signal->flags & SIGNAL_GROUP_EXIT)
		/*
		 * Group stop is so another thread can do a core dump,
		 * or else we are racing against a death signal.
		 * Just punt the stop so we can get the next signal.
		 */
		return 0;

	/*
	 * There is a group stop in progress.  We stop
	 * without any associated signal being in our queue.
	 */
	stop_count = --current->signal->group_stop_count;
	if (stop_count == 0)
		current->signal->flags = SIGNAL_STOP_STOPPED;
	current->exit_code = current->signal->group_exit_code;
	set_current_state(TASK_STOPPED);
	spin_unlock_irq(&current->sighand->siglock);
	finish_stop(stop_count);
	return 1;
}

int get_signal_to_deliver(siginfo_t *info, struct k_sigaction *return_ka,
			  struct pt_regs *regs, void *cookie)
{
	sigset_t *mask = &current->blocked;
	int signr = 0;

relock:
	spin_lock_irq(&current->sighand->siglock);
	for (;;) {
		struct k_sigaction *ka;

		if (unlikely(current->signal->group_stop_count > 0) &&
		    handle_group_stop())
			goto relock;

		signr = dequeue_signal(current, mask, info);

		if (!signr)
			break; /* will return 0 */

		if ((current->ptrace & PT_PTRACED) && signr != SIGKILL) {
			ptrace_signal_deliver(regs, cookie);

			/* Let the debugger run.  */
			ptrace_stop(signr, signr, info);

			/* We're back.  Did the debugger cancel the sig or group_exit? */
			signr = current->exit_code;
			if (signr == 0 || current->signal->flags & SIGNAL_GROUP_EXIT)
				continue;

			current->exit_code = 0;

			/* Update the siginfo structure if the signal has
			   changed.  If the debugger wanted something
			   specific in the siginfo structure then it should
			   have updated *info via PTRACE_SETSIGINFO.  */
			if (signr != info->si_signo) {
				info->si_signo = signr;
				info->si_errno = 0;
				info->si_code = SI_USER;
				info->si_pid = current->parent->pid;
				info->si_uid = current->parent->uid;
			}

			/* If the (new) signal is now blocked, requeue it.  */
			if (sigismember(&current->blocked, signr)) {
				specific_send_sig_info(signr, info, current);
				continue;
			}
		}

		ka = &current->sighand->action[signr-1];
		if (ka->sa.sa_handler == SIG_IGN) /* Do nothing.  */
			continue;
		if (ka->sa.sa_handler != SIG_DFL) {
			/* Run the handler.  */
			*return_ka = *ka;

			if (ka->sa.sa_flags & SA_ONESHOT)
				ka->sa.sa_handler = SIG_DFL;

			break; /* will return non-zero "signr" value */
		}

		/*
		 * Now we are doing the default action for this signal.
		 */
		if (sig_kernel_ignore(signr)) /* Default is nothing. */
			continue;

		/* Init gets no signals it doesn't want.  */
		if (current->pid == 1)
			continue;

		if (sig_kernel_stop(signr)) {
			/*
			 * The default action is to stop all threads in
			 * the thread group.  The job control signals
			 * do nothing in an orphaned pgrp, but SIGSTOP
			 * always works.  Note that siglock needs to be
			 * dropped during the call to is_orphaned_pgrp()
			 * because of lock ordering with tasklist_lock.
			 * This allows an intervening SIGCONT to be posted.
			 * We need to check for that and bail out if necessary.
			 */
			if (signr != SIGSTOP) {
				spin_unlock_irq(&current->sighand->siglock);

				/* signals can be posted during this window */

				if (is_orphaned_pgrp(process_group(current)))
					goto relock;

				spin_lock_irq(&current->sighand->siglock);
			}

			if (likely(do_signal_stop(signr))) {
				/* It released the siglock.  */
				goto relock;
			}

			/*
			 * We didn't actually stop, due to a race
			 * with SIGCONT or something like that.
			 */
			continue;
		}

		spin_unlock_irq(&current->sighand->siglock);

		/*
		 * Anything else is fatal, maybe with a core dump.
		 */
		current->flags |= PF_SIGNALED;
		if (sig_kernel_coredump(signr)) {
			/*
			 * If it was able to dump core, this kills all
			 * other threads in the group and synchronizes with
			 * their demise.  If we lost the race with another
			 * thread getting here, it set group_exit_code
			 * first and our do_group_exit call below will use
			 * that value and ignore the one we pass it.
			 */
			do_coredump((long)signr, signr, regs);
		}

		/*
		 * Death signals, no core dump.
		 */
		do_group_exit(signr);
		/* NOTREACHED */
	}
	spin_unlock_irq(&current->sighand->siglock);
	return signr;
}

EXPORT_SYMBOL(recalc_sigpending);
EXPORT_SYMBOL_GPL(dequeue_signal);
EXPORT_SYMBOL(flush_signals);
EXPORT_SYMBOL(force_sig);
EXPORT_SYMBOL(kill_pg);
EXPORT_SYMBOL(kill_proc);
EXPORT_SYMBOL(ptrace_notify);
EXPORT_SYMBOL(send_sig);
EXPORT_SYMBOL(send_sig_info);
EXPORT_SYMBOL(sigprocmask);
EXPORT_SYMBOL(block_all_signals);
EXPORT_SYMBOL(unblock_all_signals);


/*
 * System call entry points.
 */

asmlinkage long sys_restart_syscall(void)
{
	struct restart_block *restart = &current_thread_info()->restart_block;
	return restart->fn(restart);
}

long do_no_restart_syscall(struct restart_block *param)
{
	return -EINTR;
}

/*
 * We don't need to get the kernel lock - this is all local to this
 * particular thread.. (and that's good, because this is _heavily_
 * used by various programs)
 */

/*
 * This is also useful for kernel threads that want to temporarily
 * (or permanently) block certain signals.
 *
 * NOTE! Unlike the user-mode sys_sigprocmask(), the kernel
 * interface happily blocks "unblockable" signals like SIGKILL
 * and friends.
 */
int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
	int error;
	sigset_t old_block;

	spin_lock_irq(&current->sighand->siglock);
	old_block = current->blocked;
	error = 0;
	switch (how) {
	case SIG_BLOCK:
		sigorsets(&current->blocked, &current->blocked, set);
		break;
	case SIG_UNBLOCK:
		signandsets(&current->blocked, &current->blocked, set);
		break;
	case SIG_SETMASK:
		current->blocked = *set;
		break;
	default:
		error = -EINVAL;
	}
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	if (oldset)
		*oldset = old_block;
	return error;
}

asmlinkage long
sys_rt_sigprocmask(int how, sigset_t __user *set, sigset_t __user *oset, size_t sigsetsize)
{
	int error = -EINVAL;
	sigset_t old_set, new_set;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		error = sigprocmask(how, &new_set, &old_set);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		spin_lock_irq(&current->sighand->siglock);
		old_set = current->blocked;
		spin_unlock_irq(&current->sighand->siglock);

	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}

long do_sigpending(void __user *set, unsigned long sigsetsize)
{
	long error = -EINVAL;
	sigset_t pending;

	if (sigsetsize > sizeof(sigset_t))
		goto out;

	spin_lock_irq(&current->sighand->siglock);
	sigorsets(&pending, &current->pending.signal,
		  &current->signal->shared_pending.signal);
	spin_unlock_irq(&current->sighand->siglock);

	/* Outside the lock because only this thread touches it.  */
	sigandsets(&pending, &current->blocked, &pending);

	error = -EFAULT;
	if (!copy_to_user(set, &pending, sigsetsize))
		error = 0;

out:
	return error;
}	

asmlinkage long
sys_rt_sigpending(sigset_t __user *set, size_t sigsetsize)
{
	return do_sigpending(set, sigsetsize);
}

#ifndef HAVE_ARCH_COPY_SIGINFO_TO_USER

int copy_siginfo_to_user(siginfo_t __user *to, siginfo_t *from)
{
	int err;

	if (!access_ok (VERIFY_WRITE, to, sizeof(siginfo_t)))
		return -EFAULT;
	if (from->si_code < 0)
		return __copy_to_user(to, from, sizeof(siginfo_t))
			? -EFAULT : 0;
	/*
	 * If you change siginfo_t structure, please be sure
	 * this code is fixed accordingly.
	 * It should never copy any pad contained in the structure
	 * to avoid security leaks, but must copy the generic
	 * 3 ints plus the relevant union member.
	 */
	err = __put_user(from->si_signo, &to->si_signo);
	err |= __put_user(from->si_errno, &to->si_errno);
	err |= __put_user((short)from->si_code, &to->si_code);
	switch (from->si_code & __SI_MASK) {
	case __SI_KILL:
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		break;
	case __SI_TIMER:
		 err |= __put_user(from->si_tid, &to->si_tid);
		 err |= __put_user(from->si_overrun, &to->si_overrun);
		 err |= __put_user(from->si_ptr, &to->si_ptr);
		break;
	case __SI_POLL:
		err |= __put_user(from->si_band, &to->si_band);
		err |= __put_user(from->si_fd, &to->si_fd);
		break;
	case __SI_FAULT:
		err |= __put_user(from->si_addr, &to->si_addr);
#ifdef __ARCH_SI_TRAPNO
		err |= __put_user(from->si_trapno, &to->si_trapno);
#endif
		break;
	case __SI_CHLD:
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		err |= __put_user(from->si_status, &to->si_status);
		err |= __put_user(from->si_utime, &to->si_utime);
		err |= __put_user(from->si_stime, &to->si_stime);
		break;
	case __SI_RT: /* This is not generated by the kernel as of now. */
	case __SI_MESGQ: /* But this is */
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		err |= __put_user(from->si_ptr, &to->si_ptr);
		break;
	default: /* this is just in case for now ... */
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		break;
	}
	return err;
}

#endif

asmlinkage long
sys_rt_sigtimedwait(const sigset_t __user *uthese,
		    siginfo_t __user *uinfo,
		    const struct timespec __user *uts,
		    size_t sigsetsize)
{
	int ret, sig;
	sigset_t these;
	struct timespec ts;
	siginfo_t info;
	long timeout = 0;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;
		
	/*
	 * Invert the set of allowed signals to get those we
	 * want to block.
	 */
	sigdelsetmask(&these, sigmask(SIGKILL)|sigmask(SIGSTOP));
	signotset(&these);

	if (uts) {
		if (copy_from_user(&ts, uts, sizeof(ts)))
			return -EFAULT;
		if (ts.tv_nsec >= 1000000000L || ts.tv_nsec < 0
		    || ts.tv_sec < 0)
			return -EINVAL;
	}

	spin_lock_irq(&current->sighand->siglock);
	sig = dequeue_signal(current, &these, &info);
	if (!sig) {
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (uts)
			timeout = (timespec_to_jiffies(&ts)
				   + (ts.tv_sec || ts.tv_nsec));

		if (timeout) {
			/* None ready -- temporarily unblock those we're
			 * interested while we are sleeping in so that we'll
			 * be awakened when they arrive.  */
			current->real_blocked = current->blocked;
			sigandsets(&current->blocked, &current->blocked, &these);
			recalc_sigpending();
			spin_unlock_irq(&current->sighand->siglock);

			timeout = schedule_timeout_interruptible(timeout);

			try_to_freeze();
			spin_lock_irq(&current->sighand->siglock);
			sig = dequeue_signal(current, &these, &info);
			current->blocked = current->real_blocked;
			siginitset(&current->real_blocked, 0);
			recalc_sigpending();
		}
	}
	spin_unlock_irq(&current->sighand->siglock);

	if (sig) {
		ret = sig;
		if (uinfo) {
			if (copy_siginfo_to_user(uinfo, &info))
				ret = -EFAULT;
		}
	} else {
		ret = -EAGAIN;
		if (timeout)
			ret = -EINTR;
	}

	return ret;
}

asmlinkage long
sys_kill(int pid, int sig)
{
	struct siginfo info;

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_USER;
	info.si_pid = current->tgid;
	info.si_uid = current->uid;

	return kill_something_info(sig, &info, pid);
}

static int do_tkill(int tgid, int pid, int sig)
{
	int error;
	struct siginfo info;
	struct task_struct *p;

	error = -ESRCH;
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_TKILL;
	info.si_pid = current->tgid;
	info.si_uid = current->uid;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	if (p && (tgid <= 0 || p->tgid == tgid)) {
		error = check_kill_permission(sig, &info, p);
		/*
		 * The null signal is a permissions and process existence
		 * probe.  No signal is actually delivered.
		 */
		if (!error && sig && p->sighand) {
			spin_lock_irq(&p->sighand->siglock);
			handle_stop_signal(sig, p);
			error = specific_send_sig_info(sig, &info, p);
			spin_unlock_irq(&p->sighand->siglock);
		}
	}
	read_unlock(&tasklist_lock);

	return error;
}

/**
 *  sys_tgkill - send signal to one specific thread
 *  @tgid: the thread group ID of the thread
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *
 *  This syscall also checks the tgid and returns -ESRCH even if the PID
 *  exists but it's not belonging to the target process anymore. This
 *  method solves the problem of threads exiting and PIDs getting reused.
 */
asmlinkage long sys_tgkill(int tgid, int pid, int sig)
{
	/* This is only valid for single tasks */
	if (pid <= 0 || tgid <= 0)
		return -EINVAL;

	return do_tkill(tgid, pid, sig);
}

/*
 *  Send a signal to only one task, even if it's a CLONE_THREAD task.
 */
asmlinkage long
sys_tkill(int pid, int sig)
{
	/* This is only valid for single tasks */
	if (pid <= 0)
		return -EINVAL;

	return do_tkill(0, pid, sig);
}

asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t __user *uinfo)
{
	siginfo_t info;

	if (copy_from_user(&info, uinfo, sizeof(siginfo_t)))
		return -EFAULT;

	/* Not even root can pretend to send signals from the kernel.
	   Nor can they impersonate a kill(), which adds source info.  */
	if (info.si_code >= 0)
		return -EPERM;
	info.si_signo = sig;

	/* POSIX.1b doesn't mention process groups.  */
	return kill_proc_info(sig, &info, pid);
}

int
do_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *oact)
{
	struct k_sigaction *k;
	sigset_t mask;

	if (!valid_signal(sig) || sig < 1 || (act && sig_kernel_only(sig)))
		return -EINVAL;

	k = &current->sighand->action[sig-1];

	spin_lock_irq(&current->sighand->siglock);
	if (signal_pending(current)) {
		/*
		 * If there might be a fatal signal pending on multiple
		 * threads, make sure we take it before changing the action.
		 */
		spin_unlock_irq(&current->sighand->siglock);
		return -ERESTARTNOINTR;
	}

	if (oact)
		*oact = *k;

	if (act) {
		/*
		 * POSIX 3.3.1.3:
		 *  "Setting a signal action to SIG_IGN for a signal that is
		 *   pending shall cause the pending signal to be discarded,
		 *   whether or not it is blocked."
		 *
		 *  "Setting a signal action to SIG_DFL for a signal that is
		 *   pending and whose default action is to ignore the signal
		 *   (for example, SIGCHLD), shall cause the pending signal to
		 *   be discarded, whether or not it is blocked"
		 */
		if (act->sa.sa_handler == SIG_IGN ||
		    (act->sa.sa_handler == SIG_DFL &&
		     sig_kernel_ignore(sig))) {
			/*
			 * This is a fairly rare case, so we only take the
			 * tasklist_lock once we're sure we'll need it.
			 * Now we must do this little unlock and relock
			 * dance to maintain the lock hierarchy.
			 */
			struct task_struct *t = current;
			spin_unlock_irq(&t->sighand->siglock);
			read_lock(&tasklist_lock);
			spin_lock_irq(&t->sighand->siglock);
			*k = *act;
			sigdelsetmask(&k->sa.sa_mask,
				      sigmask(SIGKILL) | sigmask(SIGSTOP));
			sigemptyset(&mask);
			sigaddset(&mask, sig);
			rm_from_queue_full(&mask, &t->signal->shared_pending);
			do {
				rm_from_queue_full(&mask, &t->pending);
				recalc_sigpending_tsk(t);
				t = next_thread(t);
			} while (t != current);
			spin_unlock_irq(&current->sighand->siglock);
			read_unlock(&tasklist_lock);
			return 0;
		}

		*k = *act;
		sigdelsetmask(&k->sa.sa_mask,
			      sigmask(SIGKILL) | sigmask(SIGSTOP));
	}

	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

int 
do_sigaltstack (const stack_t __user *uss, stack_t __user *uoss, unsigned long sp)
{
	stack_t oss;
	int error;

	if (uoss) {
		oss.ss_sp = (void __user *) current->sas_ss_sp;
		oss.ss_size = current->sas_ss_size;
		oss.ss_flags = sas_ss_flags(sp);
	}

	if (uss) {
		void __user *ss_sp;
		size_t ss_size;
		int ss_flags;

		error = -EFAULT;
		if (!access_ok(VERIFY_READ, uss, sizeof(*uss))
		    || __get_user(ss_sp, &uss->ss_sp)
		    || __get_user(ss_flags, &uss->ss_flags)
		    || __get_user(ss_size, &uss->ss_size))
			goto out;

		error = -EPERM;
		if (on_sig_stack(sp))
			goto out;

		error = -EINVAL;
		/*
		 *
		 * Note - this code used to test ss_flags incorrectly
		 *  	  old code may have been written using ss_flags==0
		 *	  to mean ss_flags==SS_ONSTACK (as this was the only
		 *	  way that worked) - this fix preserves that older
		 *	  mechanism
		 */
		if (ss_flags != SS_DISABLE && ss_flags != SS_ONSTACK && ss_flags != 0)
			goto out;

		if (ss_flags == SS_DISABLE) {
			ss_size = 0;
			ss_sp = NULL;
		} else {
			error = -ENOMEM;
			if (ss_size < MINSIGSTKSZ)
				goto out;
		}

		current->sas_ss_sp = (unsigned long) ss_sp;
		current->sas_ss_size = ss_size;
	}

	if (uoss) {
		error = -EFAULT;
		if (copy_to_user(uoss, &oss, sizeof(oss)))
			goto out;
	}

	error = 0;
out:
	return error;
}

#ifdef __ARCH_WANT_SYS_SIGPENDING

asmlinkage long
sys_sigpending(old_sigset_t __user *set)
{
	return do_sigpending(set, sizeof(*set));
}

#endif

#ifdef __ARCH_WANT_SYS_SIGPROCMASK
/* Some platforms have their own version with special arguments others
   support only sys_rt_sigprocmask.  */

asmlinkage long
sys_sigprocmask(int how, old_sigset_t __user *set, old_sigset_t __user *oset)
{
	int error;
	old_sigset_t old_set, new_set;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		new_set &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

		spin_lock_irq(&current->sighand->siglock);
		old_set = current->blocked.sig[0];

		error = 0;
		switch (how) {
		default:
			error = -EINVAL;
			break;
		case SIG_BLOCK:
			sigaddsetmask(&current->blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&current->blocked, new_set);
			break;
		case SIG_SETMASK:
			current->blocked.sig[0] = new_set;
			break;
		}

		recalc_sigpending();
		spin_unlock_irq(&current->sighand->siglock);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		old_set = current->blocked.sig[0];
	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}
#endif /* __ARCH_WANT_SYS_SIGPROCMASK */

#ifdef __ARCH_WANT_SYS_RT_SIGACTION
asmlinkage long
sys_rt_sigaction(int sig,
		 const struct sigaction __user *act,
		 struct sigaction __user *oact,
		 size_t sigsetsize)
{
	struct k_sigaction new_sa, old_sa;
	int ret = -EINVAL;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (act) {
		if (copy_from_user(&new_sa.sa, act, sizeof(new_sa.sa)))
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);

	if (!ret && oact) {
		if (copy_to_user(oact, &old_sa.sa, sizeof(old_sa.sa)))
			return -EFAULT;
	}
out:
	return ret;
}
#endif /* __ARCH_WANT_SYS_RT_SIGACTION */

#ifdef __ARCH_WANT_SYS_SGETMASK

/*
 * For backwards compatibility.  Functionality superseded by sigprocmask.
 */
asmlinkage long
sys_sgetmask(void)
{
	/* SMP safe */
	return current->blocked.sig[0];
}

asmlinkage long
sys_ssetmask(int newmask)
{
	int old;

	spin_lock_irq(&current->sighand->siglock);
	old = current->blocked.sig[0];

	siginitset(&current->blocked, newmask & ~(sigmask(SIGKILL)|
						  sigmask(SIGSTOP)));
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	return old;
}
#endif /* __ARCH_WANT_SGETMASK */

#ifdef __ARCH_WANT_SYS_SIGNAL
/*
 * For backwards compatibility.  Functionality superseded by sigaction.
 */
asmlinkage unsigned long
sys_signal(int sig, __sighandler_t handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	new_sa.sa.sa_handler = handler;
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : (unsigned long)old_sa.sa.sa_handler;
}
#endif /* __ARCH_WANT_SYS_SIGNAL */

#ifdef __ARCH_WANT_SYS_PAUSE

asmlinkage long
sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return -ERESTARTNOHAND;
}

#endif

#ifdef __ARCH_WANT_SYS_RT_SIGSUSPEND
asmlinkage long sys_rt_sigsuspend(sigset_t __user *unewset, size_t sigsetsize)
{
	sigset_t newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	sigdelsetmask(&newset, sigmask(SIGKILL)|sigmask(SIGSTOP));

	spin_lock_irq(&current->sighand->siglock);
	current->saved_sigmask = current->blocked;
	current->blocked = newset;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	current->state = TASK_INTERRUPTIBLE;
	schedule();
	set_thread_flag(TIF_RESTORE_SIGMASK);
	return -ERESTARTNOHAND;
}
#endif /* __ARCH_WANT_SYS_RT_SIGSUSPEND */

void __init signals_init(void)
{
	sigqueue_cachep =
		kmem_cache_create("sigqueue",
				  sizeof(struct sigqueue),
				  __alignof__(struct sigqueue),
				  SLAB_PANIC, NULL, NULL);
}
