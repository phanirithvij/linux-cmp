#
# Makefile for the Linux/SuperH kernel.
#

extra-y	:= head.o init_task.o vmlinux.lds

obj-y	:= process.o signal.o entry.o traps.o irq.o \
	ptrace.o setup.o time.o sys_sh.o semaphore.o \
	io.o io_generic.o sh_ksyms.o

obj-y				+= cpu/

obj-$(CONFIG_SMP)		+= smp.o
obj-$(CONFIG_CF_ENABLER)	+= cf-enabler.o
obj-$(CONFIG_SH_STANDARD_BIOS)	+= sh_bios.o
obj-$(CONFIG_SH_KGDB)		+= kgdb_stub.o kgdb_jmp.o
obj-$(CONFIG_SH_CPU_FREQ)	+= cpufreq.o
obj-$(CONFIG_MODULES)		+= module.o
obj-$(CONFIG_EARLY_PRINTK)	+= early_printk.o
obj-$(CONFIG_KEXEC)		+= machine_kexec.o relocate_kernel.o
