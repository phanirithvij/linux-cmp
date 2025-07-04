/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __OS_H__
#define __OS_H__

#include "uml-config.h"
#include "asm/types.h"
#include "../os/include/file.h"
#include "sysdep/ptrace.h"
#include "kern_util.h"
#include "skas/mm_id.h"

#define OS_TYPE_FILE 1 
#define OS_TYPE_DIR 2 
#define OS_TYPE_SYMLINK 3 
#define OS_TYPE_CHARDEV 4
#define OS_TYPE_BLOCKDEV 5
#define OS_TYPE_FIFO 6
#define OS_TYPE_SOCK 7

/* os_access() flags */
#define OS_ACC_F_OK    0       /* Test for existence.  */
#define OS_ACC_X_OK    1       /* Test for execute permission.  */
#define OS_ACC_W_OK    2       /* Test for write permission.  */
#define OS_ACC_R_OK    4       /* Test for read permission.  */
#define OS_ACC_RW_OK   (OS_ACC_W_OK | OS_ACC_R_OK) /* Test for RW permission */

/*
 * types taken from stat_file() in hostfs_user.c
 * (if they are wrong here, they are wrong there...).
 */
struct uml_stat {
	int                ust_dev;        /* device */
	unsigned long long ust_ino;        /* inode */
	int                ust_mode;       /* protection */
	int                ust_nlink;      /* number of hard links */
	int                ust_uid;        /* user ID of owner */
	int                ust_gid;        /* group ID of owner */
	unsigned long long ust_size;       /* total size, in bytes */
	int                ust_blksize;    /* blocksize for filesystem I/O */
	unsigned long long ust_blocks;     /* number of blocks allocated */
	unsigned long      ust_atime;      /* time of last access */
	unsigned long      ust_mtime;      /* time of last modification */
	unsigned long      ust_ctime;      /* time of last change */
};

struct openflags {
	unsigned int r : 1;
	unsigned int w : 1;
	unsigned int s : 1;	/* O_SYNC */
	unsigned int c : 1;	/* O_CREAT */
	unsigned int t : 1;	/* O_TRUNC */
	unsigned int a : 1;	/* O_APPEND */
	unsigned int e : 1;	/* O_EXCL */
	unsigned int cl : 1;    /* FD_CLOEXEC */
};

#define OPENFLAGS() ((struct openflags) { .r = 0, .w = 0, .s = 0, .c = 0, \
 					  .t = 0, .a = 0, .e = 0, .cl = 0 })

static inline struct openflags of_read(struct openflags flags)
{
	flags.r = 1; 
	return(flags);
}

static inline struct openflags of_write(struct openflags flags)
{
	flags.w = 1; 
	return(flags); 
}

static inline struct openflags of_rdwr(struct openflags flags)
{
	return(of_read(of_write(flags)));
}

static inline struct openflags of_set_rw(struct openflags flags, int r, int w)
{
	flags.r = r;
	flags.w = w;
	return(flags);
}

static inline struct openflags of_sync(struct openflags flags)
{ 
	flags.s = 1; 
	return(flags); 
}

static inline struct openflags of_create(struct openflags flags)
{ 
	flags.c = 1; 
	return(flags); 
}
 
static inline struct openflags of_trunc(struct openflags flags)
{ 
	flags.t = 1; 
	return(flags); 
}
 
static inline struct openflags of_append(struct openflags flags)
{ 
	flags.a = 1; 
	return(flags); 
}
 
static inline struct openflags of_excl(struct openflags flags)
{ 
	flags.e = 1; 
	return(flags); 
}

static inline struct openflags of_cloexec(struct openflags flags)
{ 
	flags.cl = 1; 
	return(flags); 
}
  
extern int os_stat_file(const char *file_name, struct uml_stat *buf);
extern int os_stat_fd(const int fd, struct uml_stat *buf);
extern int os_access(const char *file, int mode);
extern void os_print_error(int error, const char* str);
extern int os_get_exec_close(int fd, int *close_on_exec);
extern int os_set_exec_close(int fd, int close_on_exec);
extern int os_ioctl_generic(int fd, unsigned int cmd, unsigned long arg);
extern int os_window_size(int fd, int *rows, int *cols);
extern int os_new_tty_pgrp(int fd, int pid);
extern int os_get_ifname(int fd, char *namebuf);
extern int os_set_slip(int fd);
extern int os_set_owner(int fd, int pid);
extern int os_sigio_async(int master, int slave);
extern int os_mode_fd(int fd, int mode);

extern int os_seek_file(int fd, __u64 offset);
extern int os_open_file(char *file, struct openflags flags, int mode);
extern int os_read_file(int fd, void *buf, int len);
extern int os_write_file(int fd, const void *buf, int count);
extern int os_file_size(char *file, unsigned long long *size_out);
extern int os_file_modtime(char *file, unsigned long *modtime);
extern int os_pipe(int *fd, int stream, int close_on_exec);
extern int os_set_fd_async(int fd, int owner);
extern int os_clear_fd_async(int fd);
extern int os_set_fd_block(int fd, int blocking);
extern int os_accept_connection(int fd);
extern int os_create_unix_socket(char *file, int len, int close_on_exec);
extern int os_shutdown_socket(int fd, int r, int w);
extern void os_close_file(int fd);
extern int os_rcv_fd(int fd, int *helper_pid_out);
extern int create_unix_socket(char *file, int len, int close_on_exec);
extern int os_connect_socket(char *name);
extern int os_file_type(char *file);
extern int os_file_mode(char *file, struct openflags *mode_out);
extern int os_lock_file(int fd, int excl);

/* start_up.c */
extern void os_early_checks(void);
extern int can_do_skas(void);

/* Make sure they are clear when running in TT mode. Required by
 * SEGV_MAYBE_FIXABLE */
#ifdef UML_CONFIG_MODE_SKAS
#define clear_can_do_skas() do { ptrace_faultinfo = proc_mm = 0; } while (0)
#else
#define clear_can_do_skas() do {} while (0)
#endif

/* mem.c */
extern int create_mem_file(unsigned long long len);

/* process.c */
extern unsigned long os_process_pc(int pid);
extern int os_process_parent(int pid);
extern void os_stop_process(int pid);
extern void os_kill_process(int pid, int reap_child);
extern void os_kill_ptraced_process(int pid, int reap_child);
extern void os_usr1_process(int pid);
extern int os_getpid(void);
extern int os_getpgrp(void);
extern void init_new_thread_stack(void *sig_stack, void (*usr1_handler)(int));
extern void init_new_thread_signals(int altstack);
extern int run_kernel_thread(int (*fn)(void *), void *arg, void **jmp_ptr);

extern int os_map_memory(void *virt, int fd, unsigned long long off,
			 unsigned long len, int r, int w, int x);
extern int os_protect_memory(void *addr, unsigned long len, 
			     int r, int w, int x);
extern int os_unmap_memory(void *addr, int len);
extern void os_flush_stdout(void);

/* tt.c
 * for tt mode only (will be deleted in future...)
 */
extern void stop(void);
extern int wait_for_stop(int pid, int sig, int cont_type, void *relay);
extern int protect_memory(unsigned long addr, unsigned long len,
			  int r, int w, int x, int must_succeed);
extern void forward_pending_sigio(int target);
extern int start_fork_tramp(void *arg, unsigned long temp_stack,
			    int clone_flags, int (*tramp)(void *));

/* uaccess.c */
extern unsigned long __do_user_copy(void *to, const void *from, int n,
				    void **fault_addr, void **fault_catcher,
				    void (*op)(void *to, const void *from,
					       int n), int *faulted_out);

/* helper.c */
extern int run_helper(void (*pre_exec)(void *), void *pre_data, char **argv,
		      unsigned long *stack_out);
extern int run_helper_thread(int (*proc)(void *), void *arg,
			     unsigned int flags, unsigned long *stack_out,
			     int stack_order);
extern int helper_wait(int pid);

/* umid.c */

extern int umid_file_name(char *name, char *buf, int len);
extern int set_umid(char *name);
extern char *get_umid(void);

/* signal.c */
extern void set_sigstack(void *sig_stack, int size);
extern void remove_sigstack(void);
extern void set_handler(int sig, void (*handler)(int), int flags, ...);
extern int change_sig(int signal, int on);
extern void block_signals(void);
extern void unblock_signals(void);
extern int get_signals(void);
extern int set_signals(int enable);
extern void os_usr1_signal(int on);

/* trap.c */
extern void os_fill_handlinfo(struct kern_handlers h);
extern void do_longjmp(void *p, int val);

/* util.c */
extern void stack_protections(unsigned long address);
extern void task_protections(unsigned long address);
extern int raw(int fd);
extern void setup_machinename(char *machine_out);
extern void setup_hostinfo(void);
extern int setjmp_wrapper(void (*proc)(void *, void *), ...);

/* time.c */
#define BILLION (1000 * 1000 * 1000)

extern void switch_timers(int to_real);
extern void idle_sleep(int secs);
extern void enable_timer(void);
extern void disable_timer(void);
extern void user_time_init(void);
extern void uml_idle_timer(void);
extern unsigned long long os_nsecs(void);

/* skas/mem.c */
extern long run_syscall_stub(struct mm_id * mm_idp,
			     int syscall, unsigned long *args, long expected,
			     void **addr, int done);
extern long syscall_stub_data(struct mm_id * mm_idp,
			      unsigned long *data, int data_count,
			      void **addr, void **stub_addr);
extern int map(struct mm_id * mm_idp, unsigned long virt,
	       unsigned long len, int r, int w, int x, int phys_fd,
	       unsigned long long offset, int done, void **data);
extern int unmap(struct mm_id * mm_idp, void *addr, unsigned long len,
		 int done, void **data);
extern int protect(struct mm_id * mm_idp, unsigned long addr,
		   unsigned long len, int r, int w, int x, int done,
		   void **data);

/* skas/process.c */
extern int is_skas_winch(int pid, int fd, void *data);
extern int start_userspace(unsigned long stub_stack);
extern int copy_context_skas0(unsigned long stack, int pid);
extern void userspace(union uml_pt_regs *regs);
extern void map_stub_pages(int fd, unsigned long code,
			   unsigned long data, unsigned long stack);
extern void new_thread(void *stack, void **switch_buf_ptr,
			 void **fork_buf_ptr, void (*handler)(int));
extern void thread_wait(void *sw, void *fb);
extern void switch_threads(void *me, void *next);
extern int start_idle_thread(void *stack, void *switch_buf_ptr,
			     void **fork_buf_ptr);
extern void initial_thread_cb_skas(void (*proc)(void *),
				 void *arg);
extern void halt_skas(void);
extern void reboot_skas(void);

#endif
