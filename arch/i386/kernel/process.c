/*
 *  linux/arch/i386/kernel/process.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *
 *  Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 */

/*
 * This file handles the architecture-dependent parts of process handling..
 */

#include <stdarg.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/elfcore.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/stddef.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/interrupt.h>
#include <linux/config.h>
#include <linux/utsname.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/mc146818rtc.h>
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/ptrace.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/ldt.h>
#include <asm/processor.h>
#include <asm/i387.h>
#include <asm/irq.h>
#include <asm/desc.h>
#ifdef CONFIG_MATH_EMULATION
#include <asm/math_emu.h>
#endif

#include <linux/irq.h>
#include <linux/err.h>

asmlinkage void ret_from_fork(void) __asm__("ret_from_fork");

int hlt_counter;

unsigned long boot_option_idle_override = 0;
EXPORT_SYMBOL(boot_option_idle_override);

/*
 * Return saved PC of a blocked thread.
 */
unsigned long thread_saved_pc(struct task_struct *tsk)
{
	return ((unsigned long *)tsk->thread.esp)[3];
}

/*
 * Powermanagement idle function, if any..
 */
void (*pm_idle)(void);
static cpumask_t cpu_idle_map;

void disable_hlt(void)
{
	hlt_counter++;
}

EXPORT_SYMBOL(disable_hlt);

void enable_hlt(void)
{
	hlt_counter--;
}

EXPORT_SYMBOL(enable_hlt);

/*
 * We use this if we don't have any better
 * idle routine..
 */
void default_idle(void)
{
	if (!hlt_counter && boot_cpu_data.hlt_works_ok) {
		local_irq_disable();
		if (!need_resched())
			safe_halt();
		else
			local_irq_enable();
	} else {
		cpu_relax();
	}
}

/*
 * On SMP it's slightly faster (but much more power-consuming!)
 * to poll the ->work.need_resched flag instead of waiting for the
 * cross-CPU IPI to arrive. Use this option with caution.
 */
static void poll_idle (void)
{
	int oldval;

	local_irq_enable();

	/*
	 * Deal with another CPU just having chosen a thread to
	 * run here:
	 */
	oldval = test_and_clear_thread_flag(TIF_NEED_RESCHED);

	if (!oldval) {
		set_thread_flag(TIF_POLLING_NRFLAG);
		asm volatile(
			"2:"
			"testl %0, %1;"
			"rep; nop;"
			"je 2b;"
			: : "i"(_TIF_NEED_RESCHED), "m" (current_thread_info()->flags));

		clear_thread_flag(TIF_POLLING_NRFLAG);
	} else {
		set_need_resched();
	}
}

/*
 * The idle thread. There's no useful work to be
 * done, so just try to conserve power and have a
 * low exit latency (ie sit in a loop waiting for
 * somebody to say that they'd like to reschedule)
 */
void cpu_idle (void)
{
	int cpu = _smp_processor_id();

	/* endless idle loop with no priority at all */
	while (1) {
		while (!need_resched()) {
			void (*idle)(void);

			if (cpu_isset(cpu, cpu_idle_map))
				cpu_clear(cpu, cpu_idle_map);
			rmb();
			idle = pm_idle;

			if (!idle)
				idle = default_idle;

			irq_stat[cpu].idle_timestamp = jiffies;
			idle();
		}
		schedule();
	}
}

void cpu_idle_wait(void)
{
	int cpu;
	cpumask_t map;

	for_each_online_cpu(cpu)
		cpu_set(cpu, cpu_idle_map);

	wmb();
	do {
		ssleep(1);
		cpus_and(map, cpu_idle_map, cpu_online_map);
	} while (!cpus_empty(map));
}
EXPORT_SYMBOL_GPL(cpu_idle_wait);

/*
 * This uses new MONITOR/MWAIT instructions on P4 processors with PNI,
 * which can obviate IPI to trigger checking of need_resched.
 * We execute MONITOR against need_resched and enter optimized wait state
 * through MWAIT. Whenever someone changes need_resched, we would be woken
 * up from MWAIT (without an IPI).
 */
static void mwait_idle(void)
{
	local_irq_enable();

	if (!need_resched()) {
		set_thread_flag(TIF_POLLING_NRFLAG);
		do {
			__monitor((void *)&current_thread_info()->flags, 0, 0);
			if (need_resched())
				break;
			__mwait(0, 0);
		} while (!need_resched());
		clear_thread_flag(TIF_POLLING_NRFLAG);
	}
}

void __init select_idle_routine(const struct cpuinfo_x86 *c)
{
	if (cpu_has(c, X86_FEATURE_MWAIT)) {
		printk("monitor/mwait feature present.\n");
		/*
		 * Skip, if setup has overridden idle.
		 * One CPU supports mwait => All CPUs supports mwait
		 */
		if (!pm_idle) {
			printk("using mwait in idle threads.\n");
			pm_idle = mwait_idle;
		}
	}
}

static int __init idle_setup (char *str)
{
	if (!strncmp(str, "poll", 4)) {
		printk("using polling idle threads.\n");
		pm_idle = poll_idle;
#ifdef CONFIG_X86_SMP
		if (smp_num_siblings > 1)
			printk("WARNING: polling idle and HT enabled, performance may degrade.\n");
#endif
	} else if (!strncmp(str, "halt", 4)) {
		printk("using halt in idle threads.\n");
		pm_idle = default_idle;
	}

	boot_option_idle_override = 1;
	return 1;
}

__setup("idle=", idle_setup);

void show_regs(struct pt_regs * regs)
{
	unsigned long cr0 = 0L, cr2 = 0L, cr3 = 0L, cr4 = 0L;

	printk("\n");
	printk("Pid: %d, comm: %20s\n", current->pid, current->comm);
	printk("EIP: %04x:[<%08lx>] CPU: %d\n",0xffff & regs->xcs,regs->eip, smp_processor_id());
	print_symbol("EIP is at %s\n", regs->eip);

	if (regs->xcs & 3)
		printk(" ESP: %04x:%08lx",0xffff & regs->xss,regs->esp);
	printk(" EFLAGS: %08lx    %s  (%s)\n",
	       regs->eflags, print_tainted(), system_utsname.release);
	printk("EAX: %08lx EBX: %08lx ECX: %08lx EDX: %08lx\n",
		regs->eax,regs->ebx,regs->ecx,regs->edx);
	printk("ESI: %08lx EDI: %08lx EBP: %08lx",
		regs->esi, regs->edi, regs->ebp);
	printk(" DS: %04x ES: %04x\n",
		0xffff & regs->xds,0xffff & regs->xes);

	__asm__("movl %%cr0, %0": "=r" (cr0));
	__asm__("movl %%cr2, %0": "=r" (cr2));
	__asm__("movl %%cr3, %0": "=r" (cr3));
	/* This could fault if %cr4 does not exist */
	__asm__("1: movl %%cr4, %0		\n"
		"2:				\n"
		".section __ex_table,\"a\"	\n"
		".long 1b,2b			\n"
		".previous			\n"
		: "=r" (cr4): "0" (0));
	printk("CR0: %08lx CR2: %08lx CR3: %08lx CR4: %08lx\n", cr0, cr2, cr3, cr4);
	show_trace(NULL, &regs->esp);
}

/*
 * This gets run with %ebx containing the
 * function to call, and %edx containing
 * the "args".
 */
extern void kernel_thread_helper(void);
__asm__(".section .text\n"
	".align 4\n"
	"kernel_thread_helper:\n\t"
	"movl %edx,%eax\n\t"
	"pushl %edx\n\t"
	"call *%ebx\n\t"
	"pushl %eax\n\t"
	"call do_exit\n"
	".previous");

/*
 * Create a kernel thread
 */
/**
 * 创建一个新的内核线程
 * fn-要执行的内核函数的地址。
 * arg-要传递给函数的参数
 * flags-一组clone标志
 */
int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags)
{
	struct pt_regs regs;

	memset(&regs, 0, sizeof(regs));

	/**
	 * 内核栈地址，为其赋初值。
	 * do_fork将从这里取值来为新线程初始化CPU。
	 */
	regs.ebx = (unsigned long) fn;
	regs.edx = (unsigned long) arg;

	regs.xds = __USER_DS;
	regs.xes = __USER_DS;
	regs.orig_eax = -1;
	/**
	 * 把eip设置成kernel_thread_helper，这样，新线程将执行fn函数。如果函数结束，将执行do_exit
	 * fn的返回值作为do_exit的参数。
	 */
	regs.eip = (unsigned long) kernel_thread_helper;
	regs.xcs = __KERNEL_CS;
	regs.eflags = X86_EFLAGS_IF | X86_EFLAGS_SF | X86_EFLAGS_PF | 0x2;

	/* Ok, create the new process.. */
	/**
	 * CLONE_VM避免复制调用进程的页表。由于新的内核线程无论如何都不会访问用户态地址空间。
	 * 所以复制只会造成时间和空间的浪费。
	 * CLONE_UNTRACED标志保证内核线程不会被跟踪，即使调用进程被跟踪。
	 */
	return do_fork(flags | CLONE_VM | CLONE_UNTRACED, 0, &regs, 0, NULL, NULL);
}

/*
 * Free current thread data structures etc..
 */
/**
 * 从进程描述符中分离出与线程相关的数据结构。主要是IO权限位图。
 */
void exit_thread(void)
{
	struct task_struct *tsk = current;
	struct thread_struct *t = &tsk->thread;

	/* The process may have allocated an io port bitmap... nuke it. */
	if (unlikely(NULL != t->io_bitmap_ptr)) {
		int cpu = get_cpu();
		struct tss_struct *tss = &per_cpu(init_tss, cpu);

		kfree(t->io_bitmap_ptr);
		t->io_bitmap_ptr = NULL;
		/*
		 * Careful, clear this in the TSS too:
		 */
		memset(tss->io_bitmap, 0xff, tss->io_bitmap_max);
		t->io_bitmap_max = 0;
		tss->io_bitmap_owner = NULL;
		tss->io_bitmap_max = 0;
		tss->io_bitmap_base = INVALID_IO_BITMAP_OFFSET;
		put_cpu();
	}
}

void flush_thread(void)
{
	struct task_struct *tsk = current;

	memset(tsk->thread.debugreg, 0, sizeof(unsigned long)*8);
	memset(tsk->thread.tls_array, 0, sizeof(tsk->thread.tls_array));	
	/*
	 * Forget coprocessor state..
	 */
	clear_fpu(tsk);
	clear_used_math();
}

void release_thread(struct task_struct *dead_task)
{
	if (dead_task->mm) {
		// temporary debugging check
		if (dead_task->mm->context.size) {
			printk("WARNING: dead process %8s still has LDT? <%p/%d>\n",
					dead_task->comm,
					dead_task->mm->context.ldt,
					dead_task->mm->context.size);
			BUG();
		}
	}

	release_vm86_irqs(dead_task);
}

/*
 * This gets called before we allocate a new thread and copy
 * the current task into it.
 */
void prepare_to_copy(struct task_struct *tsk)
{
	unlazy_fpu(tsk);
}

int copy_thread(int nr, unsigned long clone_flags, unsigned long esp,
	unsigned long unused,
	struct task_struct * p, struct pt_regs * regs)
{
	struct pt_regs * childregs;
	struct task_struct *tsk;
	int err;

	childregs = ((struct pt_regs *) (THREAD_SIZE + (unsigned long) p->thread_info)) - 1;
	*childregs = *regs;
	childregs->eax = 0;
	childregs->esp = esp;

	p->thread.esp = (unsigned long) childregs;
	p->thread.esp0 = (unsigned long) (childregs+1);

	p->thread.eip = (unsigned long) ret_from_fork;

	savesegment(fs,p->thread.fs);
	savesegment(gs,p->thread.gs);

	tsk = current;
	if (unlikely(NULL != tsk->thread.io_bitmap_ptr)) {
		p->thread.io_bitmap_ptr = kmalloc(IO_BITMAP_BYTES, GFP_KERNEL);
		if (!p->thread.io_bitmap_ptr) {
			p->thread.io_bitmap_max = 0;
			return -ENOMEM;
		}
		memcpy(p->thread.io_bitmap_ptr, tsk->thread.io_bitmap_ptr,
			IO_BITMAP_BYTES);
	}

	/*
	 * Set a new TLS for the child thread?
	 */
	if (clone_flags & CLONE_SETTLS) {
		struct desc_struct *desc;
		struct user_desc info;
		int idx;

		err = -EFAULT;
		if (copy_from_user(&info, (void __user *)childregs->esi, sizeof(info)))
			goto out;
		err = -EINVAL;
		if (LDT_empty(&info))
			goto out;

		idx = info.entry_number;
		if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
			goto out;

		desc = p->thread.tls_array + idx - GDT_ENTRY_TLS_MIN;
		desc->a = LDT_entry_a(&info);
		desc->b = LDT_entry_b(&info);
	}

	err = 0;
 out:
	if (err && p->thread.io_bitmap_ptr) {
		kfree(p->thread.io_bitmap_ptr);
		p->thread.io_bitmap_max = 0;
	}
	return err;
}

/*
 * fill in the user structure for a core dump..
 */
void dump_thread(struct pt_regs * regs, struct user * dump)
{
	int i;

/* changed the size calculations - should hopefully work better. lbt */
	dump->magic = CMAGIC;
	dump->start_code = 0;
	dump->start_stack = regs->esp & ~(PAGE_SIZE - 1);
	dump->u_tsize = ((unsigned long) current->mm->end_code) >> PAGE_SHIFT;
	dump->u_dsize = ((unsigned long) (current->mm->brk + (PAGE_SIZE-1))) >> PAGE_SHIFT;
	dump->u_dsize -= dump->u_tsize;
	dump->u_ssize = 0;
	for (i = 0; i < 8; i++)
		dump->u_debugreg[i] = current->thread.debugreg[i];  

	if (dump->start_stack < TASK_SIZE)
		dump->u_ssize = ((unsigned long) (TASK_SIZE - dump->start_stack)) >> PAGE_SHIFT;

	dump->regs.ebx = regs->ebx;
	dump->regs.ecx = regs->ecx;
	dump->regs.edx = regs->edx;
	dump->regs.esi = regs->esi;
	dump->regs.edi = regs->edi;
	dump->regs.ebp = regs->ebp;
	dump->regs.eax = regs->eax;
	dump->regs.ds = regs->xds;
	dump->regs.es = regs->xes;
	savesegment(fs,dump->regs.fs);
	savesegment(gs,dump->regs.gs);
	dump->regs.orig_eax = regs->orig_eax;
	dump->regs.eip = regs->eip;
	dump->regs.cs = regs->xcs;
	dump->regs.eflags = regs->eflags;
	dump->regs.esp = regs->esp;
	dump->regs.ss = regs->xss;

	dump->u_fpvalid = dump_fpu (regs, &dump->i387);
}

/* 
 * Capture the user space registers if the task is not running (in user space)
 */
int dump_task_regs(struct task_struct *tsk, elf_gregset_t *regs)
{
	struct pt_regs ptregs;
	
	ptregs = *(struct pt_regs *)
		((unsigned long)tsk->thread_info+THREAD_SIZE - sizeof(ptregs));
	ptregs.xcs &= 0xffff;
	ptregs.xds &= 0xffff;
	ptregs.xes &= 0xffff;
	ptregs.xss &= 0xffff;

	elf_core_copy_regs(regs, &ptregs);

	return 1;
}

static inline void
handle_io_bitmap(struct thread_struct *next, struct tss_struct *tss)
{
	if (!next->io_bitmap_ptr) {
		/*
		 * Disable the bitmap via an invalid offset. We still cache
		 * the previous bitmap owner and the IO bitmap contents:
		 */
		tss->io_bitmap_base = INVALID_IO_BITMAP_OFFSET;
		return;
	}
	if (likely(next == tss->io_bitmap_owner)) {
		/*
		 * Previous owner of the bitmap (hence the bitmap content)
		 * matches the next task, we dont have to do anything but
		 * to set a valid offset in the TSS:
		 */
		tss->io_bitmap_base = IO_BITMAP_OFFSET;
		return;
	}
	/*
	 * Lazy TSS's I/O bitmap copy. We set an invalid offset here
	 * and we let the task to get a GPF in case an I/O instruction
	 * is performed.  The handler of the GPF will verify that the
	 * faulting task has a valid I/O bitmap and, it true, does the
	 * real copy and restart the instruction.  This will save us
	 * redundant copies when the currently switched task does not
	 * perform any I/O during its timeslice.
	 */
	tss->io_bitmap_base = INVALID_IO_BITMAP_OFFSET_LAZY;
}
/*
 * This special macro can be used to load a debugging register
 */
#define loaddebug(thread,register) \
		__asm__("movl %0,%%db" #register  \
			: /* no output */ \
			:"r" (thread->debugreg[register]))

/*
 *	switch_to(x,yn) should switch tasks from x to y.
 *
 * We fsave/fwait so that an exception goes off at the right time
 * (as a call from the fsave or fwait in effect) rather than to
 * the wrong process. Lazy FP saving no longer makes any sense
 * with modern CPU's, and this simplifies a lot of things (SMP
 * and UP become the same).
 *
 * NOTE! We used to use the x86 hardware context switching. The
 * reason for not using it any more becomes apparent when you
 * try to recover gracefully from saved state that is no longer
 * valid (stale segment register values in particular). With the
 * hardware task-switch, there is no way to fix up bad state in
 * a reasonable manner.
 *
 * The fact that Intel documents the hardware task-switching to
 * be slow is a fairly red herring - this code is not noticeably
 * faster. However, there _is_ some room for improvement here,
 * so the performance issues may eventually be a valid point.
 * More important, however, is the fact that this allows us much
 * more flexibility.
 *
 * The return value (in %eax) will be the "prev" task after
 * the task-switch, and shows up in ret_from_fork in entry.S,
 * for example.
 */
/**
 * __switch_to函数执行大多数进程切换的工作。
 * 进程切换的工作开始于switch_to宏，但是它的主要工作还是由__switch_to完成。
 * 这个函数是寄存器传参的函数。在switch_to宏中，参数已经保存在eax和edx中了.
 */
struct task_struct fastcall * __switch_to(struct task_struct *prev_p, struct task_struct *next_p)
{
	struct thread_struct *prev = &prev_p->thread,
				 *next = &next_p->thread;
	/**
	 * 通过读取current_thread_info()->cpu,获得当前进程在哪个CPU上运行.
	 * 因为在schedule函数中已经调用了禁用抢占,所以这里可以直接使用smp_processor_id()
	 */
	int cpu = smp_processor_id();
	struct tss_struct *tss = &per_cpu(init_tss, cpu);

	/* never put a printk in __switch_to... printk() calls wake_up*() indirectly */
	/**
	 * __unlazy_fpu宏有选择的保存FPU\MMX\XMM寄存器的内容.
	 * 它可能会延后保存这些寄存器的内容.
	 */
	__unlazy_fpu(prev_p);

	/*
	 * Reload esp0, LDT and the page table pointer:
	 */
	/**
	 * 把next_p->thread.esp0装入本地CPU的TSS的esp0字段.
	 * 任何由sysenter汇编指令产生的从用户态到内核态的特权级转换将把这个地址复制到esp寄存器.
	 */
	load_esp0(tss, next);

	/*
	 * Load the per-thread Thread-Local Storage descriptor.
	 */
	/**
	 * 将next_p进程使用的线程局部存储(TLS)段装入本地CPU的全局描述符表.
	 */
	load_TLS(next, cpu);

	/*
	 * Save away %fs and %gs. No need to save %es and %ds, as
	 * those are always kernel segments while inside the kernel.
	 */
	/**
	 * 把fs和gs段寄存器的内容分别存放在prev_p->thread.fs和prev_p->thread.gs中.
	 */
	asm volatile("movl %%fs,%0":"=m" (*(int *)&prev->fs));
	asm volatile("movl %%gs,%0":"=m" (*(int *)&prev->gs));

	/*
	 * Restore %fs and %gs if needed.
	 */
	/**
	 * 不管是prev还是next,只要他们使用了fs和gs,那么,都需要将next中的fs,gs更新到段寄存器.
	 * 即使next并不使用fs,但是只要prev使用了,也需要更新.这样可以防止next通过fs,gs访问prev的数据.
	 */
	if (unlikely(prev->fs | prev->gs | next->fs | next->gs)) {
		/**
		 * loadsegment可能会装载一个无效的段寄存器.CPU可能会产生一个异常.
		 * 但是loadsegment会采用代码修正技术来处理这种情况.
		 */
		loadsegment(fs, next->fs);
		loadsegment(gs, next->gs);
	}

	/*
	 * Now maybe reload the debug registers
	 */
	/**
	 * 用debugreg数组的内容dr0..dr7中的6个调试寄存器.这允许定义四个断点区域.
	 */
	if (unlikely(next->debugreg[7])) {
		loaddebug(next, 0);
		loaddebug(next, 1);
		loaddebug(next, 2);
		loaddebug(next, 3);
		/* no 4 and 5 */
		loaddebug(next, 6);
		loaddebug(next, 7);
	}

	/**
	 * 如果必要,更新TSS中的IO位图.当next或者prev有其自己的定制IO权限位图时必须这么做.
	 */
	if (unlikely(prev->io_bitmap_ptr || next->io_bitmap_ptr))
		/**
		 * handle_io_bitmap并不立即更新位图,而是采用一种懒模式的方法.
		 */
		handle_io_bitmap(next, tss);

	/**
	 * return产生的汇编指令是movl %edi, %eax,ret.
	 * 这里有保护eax和返回地址的问题.请仔细理解.
	 * 除了需要理解switch_to宏中的jmp指令外,对于没有产生切换,而是第一次开始执行的进程.
	 * 它并不会跳回switch_to,而是找到ret_from_fork函数的超始地址.
	 */
	return prev_p;
}

asmlinkage int sys_fork(struct pt_regs regs)
{
	return do_fork(SIGCHLD, regs.esp, &regs, 0, NULL, NULL);
}

asmlinkage int sys_clone(struct pt_regs regs)
{
	unsigned long clone_flags;
	unsigned long newsp;
	int __user *parent_tidptr, *child_tidptr;

	clone_flags = regs.ebx;
	newsp = regs.ecx;
	parent_tidptr = (int __user *)regs.edx;
	child_tidptr = (int __user *)regs.edi;
	if (!newsp)
		newsp = regs.esp;
	return do_fork(clone_flags, newsp, &regs, 0, parent_tidptr, child_tidptr);
}

/*
 * This is trivial, and on the face of it looks like it
 * could equally well be done in user mode.
 *
 * Not so, for quite unobvious reasons - register pressure.
 * In user mode vfork() cannot have a stack frame, and if
 * done by calling the "clone()" system call directly, you
 * do not have enough call-clobbered registers to hold all
 * the information you need.
 * 
 * sys_vfork 实际调用的是 do_fork，但传参不同，父进程被阻塞，直到子进程退出或者执行 exec()
 * 不复制父进程的页表项，不会向地址空间写数据
 * 在并未使用写时复制机制之前，vfork 比较有意义，但若 fork 支持父进程页表copy选项，那么vfork就没有用武之地了
 */
asmlinkage int sys_vfork(struct pt_regs regs)
{
	return do_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, regs.esp, &regs, 0, NULL, NULL);
}

/*
 * sys_execve() executes a new program.
 */
asmlinkage int sys_execve(struct pt_regs regs)
{
	int error;
	char * filename;

	filename = getname((char __user *) regs.ebx);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	error = do_execve(filename,
			(char __user * __user *) regs.ecx,
			(char __user * __user *) regs.edx,
			&regs);
	if (error == 0) {
		task_lock(current);
		current->ptrace &= ~PT_DTRACE;
		task_unlock(current);
		/* Make sure we don't return using sysenter.. */
		set_thread_flag(TIF_IRET);
	}
	putname(filename);
out:
	return error;
}

#define top_esp                (THREAD_SIZE - sizeof(unsigned long))
#define top_ebp                (THREAD_SIZE - 2*sizeof(unsigned long))

unsigned long get_wchan(struct task_struct *p)
{
	unsigned long ebp, esp, eip;
	unsigned long stack_page;
	int count = 0;
	if (!p || p == current || p->state == TASK_RUNNING)
		return 0;
	stack_page = (unsigned long)p->thread_info;
	esp = p->thread.esp;
	if (!stack_page || esp < stack_page || esp > top_esp+stack_page)
		return 0;
	/* include/asm-i386/system.h:switch_to() pushes ebp last. */
	ebp = *(unsigned long *) esp;
	do {
		if (ebp < stack_page || ebp > top_ebp+stack_page)
			return 0;
		eip = *(unsigned long *) (ebp+4);
		if (!in_sched_functions(eip))
			return eip;
		ebp = *(unsigned long *) ebp;
	} while (count++ < 16);
	return 0;
}

/*
 * sys_alloc_thread_area: get a yet unused TLS descriptor index.
 */
static int get_free_idx(void)
{
	struct thread_struct *t = &current->thread;
	int idx;

	for (idx = 0; idx < GDT_ENTRY_TLS_ENTRIES; idx++)
		if (desc_empty(t->tls_array + idx))
			return idx + GDT_ENTRY_TLS_MIN;
	return -ESRCH;
}

/*
 * Set a given TLS descriptor:
 */
asmlinkage int sys_set_thread_area(struct user_desc __user *u_info)
{
	struct thread_struct *t = &current->thread;
	struct user_desc info;
	struct desc_struct *desc;
	int cpu, idx;

	if (copy_from_user(&info, u_info, sizeof(info)))
		return -EFAULT;
	idx = info.entry_number;

	/*
	 * index -1 means the kernel should try to find and
	 * allocate an empty descriptor:
	 */
	if (idx == -1) {
		idx = get_free_idx();
		if (idx < 0)
			return idx;
		if (put_user(idx, &u_info->entry_number))
			return -EFAULT;
	}

	if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
		return -EINVAL;

	desc = t->tls_array + idx - GDT_ENTRY_TLS_MIN;

	/*
	 * We must not get preempted while modifying the TLS.
	 */
	cpu = get_cpu();

	if (LDT_empty(&info)) {
		desc->a = 0;
		desc->b = 0;
	} else {
		desc->a = LDT_entry_a(&info);
		desc->b = LDT_entry_b(&info);
	}
	load_TLS(t, cpu);

	put_cpu();

	return 0;
}

/*
 * Get the current Thread-Local Storage area:
 */

#define GET_BASE(desc) ( \
	(((desc)->a >> 16) & 0x0000ffff) | \
	(((desc)->b << 16) & 0x00ff0000) | \
	( (desc)->b        & 0xff000000)   )

#define GET_LIMIT(desc) ( \
	((desc)->a & 0x0ffff) | \
	 ((desc)->b & 0xf0000) )
	
#define GET_32BIT(desc)		(((desc)->b >> 22) & 1)
#define GET_CONTENTS(desc)	(((desc)->b >> 10) & 3)
#define GET_WRITABLE(desc)	(((desc)->b >>  9) & 1)
#define GET_LIMIT_PAGES(desc)	(((desc)->b >> 23) & 1)
#define GET_PRESENT(desc)	(((desc)->b >> 15) & 1)
#define GET_USEABLE(desc)	(((desc)->b >> 20) & 1)

asmlinkage int sys_get_thread_area(struct user_desc __user *u_info)
{
	struct user_desc info;
	struct desc_struct *desc;
	int idx;

	if (get_user(idx, &u_info->entry_number))
		return -EFAULT;
	if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
		return -EINVAL;

	desc = current->thread.tls_array + idx - GDT_ENTRY_TLS_MIN;

	info.entry_number = idx;
	info.base_addr = GET_BASE(desc);
	info.limit = GET_LIMIT(desc);
	info.seg_32bit = GET_32BIT(desc);
	info.contents = GET_CONTENTS(desc);
	info.read_exec_only = !GET_WRITABLE(desc);
	info.limit_in_pages = GET_LIMIT_PAGES(desc);
	info.seg_not_present = !GET_PRESENT(desc);
	info.useable = GET_USEABLE(desc);

	if (copy_to_user(u_info, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

