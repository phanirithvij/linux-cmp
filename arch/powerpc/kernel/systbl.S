/*
 * This file contains the table of syscall-handling functions.
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 * Largely rewritten by Cort Dougan (cort@cs.nmt.edu)
 * and Paul Mackerras.
 *
 * Adapted for iSeries by Mike Corrigan (mikejc@us.ibm.com)
 * PPC64 updates by Dave Engebretsen (engebret@us.ibm.com) 
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <asm/ppc_asm.h>

#ifdef CONFIG_PPC64
#define SYSCALL(func)		.llong	.sys_##func,.sys_##func
#define COMPAT_SYS(func)	.llong	.sys_##func,.compat_sys_##func
#define PPC_SYS(func)		.llong	.ppc_##func,.ppc_##func
#define OLDSYS(func)		.llong	.sys_ni_syscall,.sys_ni_syscall
#define SYS32ONLY(func)		.llong	.sys_ni_syscall,.compat_sys_##func
#define SYSX(f, f3264, f32)	.llong	.f,.f3264
#else
#define SYSCALL(func)		.long	sys_##func
#define COMPAT_SYS(func)	.long	sys_##func
#define PPC_SYS(func)		.long	ppc_##func
#define OLDSYS(func)		.long	sys_##func
#define SYS32ONLY(func)		.long	sys_##func
#define SYSX(f, f3264, f32)	.long	f32
#endif

#ifdef CONFIG_PPC64
#define sys_sigpending	sys_ni_syscall
#define sys_old_getrlimit sys_ni_syscall
#else
#define ppc_rtas	sys_ni_syscall
#endif

_GLOBAL(sys_call_table)
SYSCALL(restart_syscall)
SYSCALL(exit)
PPC_SYS(fork)
SYSCALL(read)
SYSCALL(write)
COMPAT_SYS(open)
SYSCALL(close)
COMPAT_SYS(waitpid)
COMPAT_SYS(creat)
SYSCALL(link)
SYSCALL(unlink)
COMPAT_SYS(execve)
SYSCALL(chdir)
COMPAT_SYS(time)
SYSCALL(mknod)
SYSCALL(chmod)
SYSCALL(lchown)
SYSCALL(ni_syscall)
OLDSYS(stat)
SYSX(sys_lseek,ppc32_lseek,sys_lseek)
SYSCALL(getpid)
COMPAT_SYS(mount)
SYSX(sys_ni_syscall,sys_oldumount,sys_oldumount)
SYSCALL(setuid)
SYSCALL(getuid)
COMPAT_SYS(stime)
COMPAT_SYS(ptrace)
SYSCALL(alarm)
OLDSYS(fstat)
COMPAT_SYS(pause)
COMPAT_SYS(utime)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
COMPAT_SYS(access)
COMPAT_SYS(nice)
SYSCALL(ni_syscall)
SYSCALL(sync)
COMPAT_SYS(kill)
SYSCALL(rename)
COMPAT_SYS(mkdir)
SYSCALL(rmdir)
SYSCALL(dup)
SYSCALL(pipe)
COMPAT_SYS(times)
SYSCALL(ni_syscall)
SYSCALL(brk)
SYSCALL(setgid)
SYSCALL(getgid)
SYSCALL(signal)
SYSCALL(geteuid)
SYSCALL(getegid)
SYSCALL(acct)
SYSCALL(umount)
SYSCALL(ni_syscall)
COMPAT_SYS(ioctl)
COMPAT_SYS(fcntl)
SYSCALL(ni_syscall)
COMPAT_SYS(setpgid)
SYSCALL(ni_syscall)
SYSX(sys_ni_syscall,sys_olduname, sys_olduname)
COMPAT_SYS(umask)
SYSCALL(chroot)
SYSCALL(ustat)
SYSCALL(dup2)
SYSCALL(getppid)
SYSCALL(getpgrp)
SYSCALL(setsid)
SYS32ONLY(sigaction)
SYSCALL(sgetmask)
COMPAT_SYS(ssetmask)
SYSCALL(setreuid)
SYSCALL(setregid)
SYS32ONLY(sigsuspend)
COMPAT_SYS(sigpending)
COMPAT_SYS(sethostname)
COMPAT_SYS(setrlimit)
COMPAT_SYS(old_getrlimit)
COMPAT_SYS(getrusage)
COMPAT_SYS(gettimeofday)
COMPAT_SYS(settimeofday)
COMPAT_SYS(getgroups)
COMPAT_SYS(setgroups)
SYSX(sys_ni_syscall,sys_ni_syscall,ppc_select)
SYSCALL(symlink)
OLDSYS(lstat)
COMPAT_SYS(readlink)
SYSCALL(uselib)
SYSCALL(swapon)
SYSCALL(reboot)
SYSX(sys_ni_syscall,old32_readdir,old_readdir)
SYSCALL(mmap)
SYSCALL(munmap)
SYSCALL(truncate)
SYSCALL(ftruncate)
SYSCALL(fchmod)
SYSCALL(fchown)
COMPAT_SYS(getpriority)
COMPAT_SYS(setpriority)
SYSCALL(ni_syscall)
COMPAT_SYS(statfs)
COMPAT_SYS(fstatfs)
SYSCALL(ni_syscall)
COMPAT_SYS(socketcall)
COMPAT_SYS(syslog)
COMPAT_SYS(setitimer)
COMPAT_SYS(getitimer)
COMPAT_SYS(newstat)
COMPAT_SYS(newlstat)
COMPAT_SYS(newfstat)
SYSX(sys_ni_syscall,sys_uname,sys_uname)
SYSCALL(ni_syscall)
SYSCALL(vhangup)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
COMPAT_SYS(wait4)
SYSCALL(swapoff)
COMPAT_SYS(sysinfo)
COMPAT_SYS(ipc)
SYSCALL(fsync)
SYS32ONLY(sigreturn)
PPC_SYS(clone)
COMPAT_SYS(setdomainname)
PPC_SYS(newuname)
SYSCALL(ni_syscall)
COMPAT_SYS(adjtimex)
SYSCALL(mprotect)
SYSX(sys_ni_syscall,compat_sys_sigprocmask,sys_sigprocmask)
SYSCALL(ni_syscall)
SYSCALL(init_module)
SYSCALL(delete_module)
SYSCALL(ni_syscall)
SYSCALL(quotactl)
COMPAT_SYS(getpgid)
SYSCALL(fchdir)
SYSCALL(bdflush)
COMPAT_SYS(sysfs)
SYSX(ppc64_personality,ppc64_personality,sys_personality)
SYSCALL(ni_syscall)
SYSCALL(setfsuid)
SYSCALL(setfsgid)
SYSCALL(llseek)
COMPAT_SYS(getdents)
SYSX(sys_select,ppc32_select,ppc_select)
SYSCALL(flock)
SYSCALL(msync)
COMPAT_SYS(readv)
COMPAT_SYS(writev)
COMPAT_SYS(getsid)
SYSCALL(fdatasync)
COMPAT_SYS(sysctl)
SYSCALL(mlock)
SYSCALL(munlock)
SYSCALL(mlockall)
SYSCALL(munlockall)
COMPAT_SYS(sched_setparam)
COMPAT_SYS(sched_getparam)
COMPAT_SYS(sched_setscheduler)
COMPAT_SYS(sched_getscheduler)
SYSCALL(sched_yield)
COMPAT_SYS(sched_get_priority_max)
COMPAT_SYS(sched_get_priority_min)
COMPAT_SYS(sched_rr_get_interval)
COMPAT_SYS(nanosleep)
SYSCALL(mremap)
SYSCALL(setresuid)
SYSCALL(getresuid)
SYSCALL(ni_syscall)
SYSCALL(poll)
COMPAT_SYS(nfsservctl)
SYSCALL(setresgid)
SYSCALL(getresgid)
COMPAT_SYS(prctl)
COMPAT_SYS(rt_sigreturn)
COMPAT_SYS(rt_sigaction)
COMPAT_SYS(rt_sigprocmask)
COMPAT_SYS(rt_sigpending)
COMPAT_SYS(rt_sigtimedwait)
COMPAT_SYS(rt_sigqueueinfo)
COMPAT_SYS(rt_sigsuspend)
COMPAT_SYS(pread64)
COMPAT_SYS(pwrite64)
SYSCALL(chown)
SYSCALL(getcwd)
SYSCALL(capget)
SYSCALL(capset)
COMPAT_SYS(sigaltstack)
SYSX(sys_sendfile64,compat_sys_sendfile,sys_sendfile)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
PPC_SYS(vfork)
COMPAT_SYS(getrlimit)
COMPAT_SYS(readahead)
SYS32ONLY(mmap2)
SYS32ONLY(truncate64)
SYS32ONLY(ftruncate64)
SYSX(sys_ni_syscall,sys_stat64,sys_stat64)
SYSX(sys_ni_syscall,sys_lstat64,sys_lstat64)
SYSX(sys_ni_syscall,sys_fstat64,sys_fstat64)
SYSCALL(pciconfig_read)
SYSCALL(pciconfig_write)
SYSCALL(pciconfig_iobase)
SYSCALL(ni_syscall)
SYSCALL(getdents64)
SYSCALL(pivot_root)
SYSX(sys_ni_syscall,compat_sys_fcntl64,sys_fcntl64)
SYSCALL(madvise)
SYSCALL(mincore)
SYSCALL(gettid)
SYSCALL(tkill)
SYSCALL(setxattr)
SYSCALL(lsetxattr)
SYSCALL(fsetxattr)
SYSCALL(getxattr)
SYSCALL(lgetxattr)
SYSCALL(fgetxattr)
SYSCALL(listxattr)
SYSCALL(llistxattr)
SYSCALL(flistxattr)
SYSCALL(removexattr)
SYSCALL(lremovexattr)
SYSCALL(fremovexattr)
COMPAT_SYS(futex)
COMPAT_SYS(sched_setaffinity)
COMPAT_SYS(sched_getaffinity)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
SYS32ONLY(sendfile64)
COMPAT_SYS(io_setup)
SYSCALL(io_destroy)
COMPAT_SYS(io_getevents)
COMPAT_SYS(io_submit)
SYSCALL(io_cancel)
SYSCALL(set_tid_address)
SYSX(sys_fadvise64,ppc32_fadvise64,sys_fadvise64)
SYSCALL(exit_group)
SYSX(sys_lookup_dcookie,ppc32_lookup_dcookie,sys_lookup_dcookie)
SYSCALL(epoll_create)
SYSCALL(epoll_ctl)
SYSCALL(epoll_wait)
SYSCALL(remap_file_pages)
SYSX(sys_timer_create,compat_sys_timer_create,sys_timer_create)
COMPAT_SYS(timer_settime)
COMPAT_SYS(timer_gettime)
SYSCALL(timer_getoverrun)
SYSCALL(timer_delete)
COMPAT_SYS(clock_settime)
COMPAT_SYS(clock_gettime)
COMPAT_SYS(clock_getres)
COMPAT_SYS(clock_nanosleep)
COMPAT_SYS(swapcontext)
COMPAT_SYS(tgkill)
COMPAT_SYS(utimes)
COMPAT_SYS(statfs64)
COMPAT_SYS(fstatfs64)
SYSX(sys_ni_syscall, ppc_fadvise64_64, ppc_fadvise64_64)
PPC_SYS(rtas)
OLDSYS(debug_setcontext)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
COMPAT_SYS(mbind)
COMPAT_SYS(get_mempolicy)
COMPAT_SYS(set_mempolicy)
COMPAT_SYS(mq_open)
SYSCALL(mq_unlink)
COMPAT_SYS(mq_timedsend)
COMPAT_SYS(mq_timedreceive)
COMPAT_SYS(mq_notify)
COMPAT_SYS(mq_getsetattr)
COMPAT_SYS(kexec_load)
COMPAT_SYS(add_key)
COMPAT_SYS(request_key)
COMPAT_SYS(keyctl)
COMPAT_SYS(waitid)
COMPAT_SYS(ioprio_set)
COMPAT_SYS(ioprio_get)
SYSCALL(inotify_init)
SYSCALL(inotify_add_watch)
SYSCALL(inotify_rm_watch)
SYSCALL(spu_run)
SYSCALL(spu_create)
COMPAT_SYS(pselect6)
COMPAT_SYS(ppoll)
