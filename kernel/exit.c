/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/tty.h>
#include <linux/namespace.h>
#include <linux/key.h>
#include <linux/security.h>
#include <linux/cpu.h>
#include <linux/acct.h>
#include <linux/file.h>
#include <linux/binfmts.h>
#include <linux/ptrace.h>
#include <linux/profile.h>
#include <linux/mount.h>
#include <linux/proc_fs.h>
#include <linux/mempolicy.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>

extern void sem_exit (void);
extern struct task_struct *child_reaper;

int getrusage(struct task_struct *, int, struct rusage __user *);

static void __unhash_process(struct task_struct *p)
{
	nr_threads--;
	detach_pid(p, PIDTYPE_PID);
	detach_pid(p, PIDTYPE_TGID);
	if (thread_group_leader(p)) {
		detach_pid(p, PIDTYPE_PGID);
		detach_pid(p, PIDTYPE_SID);
		if (p->pid)
			__get_cpu_var(process_counts)--;
	}

	REMOVE_LINKS(p);
}

/**
 * 释放进程描述符。如果进程已经是僵死状态，就会回收它占用的RAM。
 * 在执行了 do_exit 后，线程僵死不能执行，但是进程描述符还存在，父进程还可以获取到子进程的信息，
 * 当不再需要的时候，通过 release_task 释放。
 */
void release_task(struct task_struct * p)
{
	int zap_leader;
	task_t *leader;
	struct dentry *proc_dentry;

repeat: 
	/**
	 * 递减进程拥有者的进程个数。
	 */
	atomic_dec(&p->user->processes);
	spin_lock(&p->proc_lock);
	proc_dentry = proc_pid_unhash(p);
	write_lock_irq(&tasklist_lock);
	/**
	 * 如果进程正被跟踪，函数将它从调试程序的ptrace_children链表中删除，并让该进程重新属于初始的父进程。
	 */
	if (unlikely(p->ptrace))
		__ptrace_unlink(p);
	BUG_ON(!list_empty(&p->ptrace_list) || !list_empty(&p->ptrace_children));
	/**
	 * 删除所有的挂起信号并释放进程的signal_struct描述符。
	 * 如果该描述符不再被其他的轻量级进程使用，函数进一步删除这个数据结构。
	 * 它还会调用exit_itimers，删除所有POSIX时间间隔定时器。
	 */
	__exit_signal(p);
	/**
	 * 删除信号处理函数。
	 */
	__exit_sighand(p);
	/**
	 * __unhash_process将进程从各种hash表中摘除。它执行:
	 *    变量nr_threads减1
	 *    两次调用detach_pid，分别从PIDTYPE_PID和PIDTYPE_TGID类型的PID散列表中删除进程描述符。
	 *    如果进程是线程组的领头进程，那么再调用两次detach_pid，从PIDTYPE_PGID和PIDTYPE_SID类型的散列表中删除进程描述符。
	 *    用宏REMOVE_LINKS从进程链表中解除进程描述符的链接。
	 */
	__unhash_process(p);

	/*
	 * If we are the last non-leader member of the thread
	 * group, and the leader is zombie, then notify the
	 * group leader's parent process. (if it wants notification.)
	 */
	zap_leader = 0;
	leader = p->group_leader;
	/**
	 * 如果进程不是线程组的领头进程，领头进程处于僵死状态，并且进程是线程组的最后一个成员。
	 */
	if (leader != p && thread_group_empty(leader) && leader->exit_state == EXIT_ZOMBIE) {
		BUG_ON(leader->exit_signal == -1);
		/**
		 * 向领头进程的父进程发送一个信号，通知它:进程已经死亡。
		 */
		do_notify_parent(leader, leader->exit_signal);
		/*
		 * If we were the last child thread and the leader has
		 * exited already, and the leader's parent ignores SIGCHLD,
		 * then we are the one who should release the leader.
		 *
		 * do_notify_parent() will have marked it self-reaping in
		 * that case.
		 */
		zap_leader = (leader->exit_signal == -1);
	}

	/**
	 * sched_exit函数调整父进程的时间片。
	 */
	sched_exit(p);
	write_unlock_irq(&tasklist_lock);
	spin_unlock(&p->proc_lock);
	proc_pid_flush(proc_dentry);
	release_thread(p);
	/**
	 * 递减进程描述符的使用计数器。如果计数器变成0,则终止所有残留的对进程的引用。
	 *     递减进程所有者的user_struct数据结构的使用计数器。如果计数为0，就释放该结构。
	 *     释放进程描述符、thread_info描述符、内核态堆栈所占用的内存区域。
	 */
	put_task_struct(p);

	p = leader;
	if (unlikely(zap_leader))
		goto repeat;
}

/* we are using it only for SMP init */

void unhash_process(struct task_struct *p)
{
	struct dentry *proc_dentry;

	spin_lock(&p->proc_lock);
	proc_dentry = proc_pid_unhash(p);
	write_lock_irq(&tasklist_lock);
	__unhash_process(p);
	write_unlock_irq(&tasklist_lock);
	spin_unlock(&p->proc_lock);
	proc_pid_flush(proc_dentry);
}

/*
 * This checks not only the pgrp, but falls back on the pid if no
 * satisfactory pgrp is found. I dunno - gdb doesn't work correctly
 * without this...
 */
int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	int sid = -1;

	read_lock(&tasklist_lock);
	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p->signal->session > 0) {
			sid = p->signal->session;
			goto out;
		}
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	p = find_task_by_pid(pgrp);
	if (p)
		sid = p->signal->session;
out:
	read_unlock(&tasklist_lock);
	
	return sid;
}

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 */
static int will_become_orphaned_pgrp(int pgrp, task_t *ignored_task)
{
	struct task_struct *p;
	int ret = 1;

	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p == ignored_task
				|| p->exit_state
				|| p->real_parent->pid == 1)
			continue;
		if (process_group(p->real_parent) != pgrp
			    && p->real_parent->signal->session == p->signal->session) {
			ret = 0;
			break;
		}
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return ret;	/* (sighing) "Often!" */
}

int is_orphaned_pgrp(int pgrp)
{
	int retval;

	read_lock(&tasklist_lock);
	retval = will_become_orphaned_pgrp(pgrp, NULL);
	read_unlock(&tasklist_lock);

	return retval;
}

static inline int has_stopped_jobs(int pgrp)
{
	int retval = 0;
	struct task_struct *p;

	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p->state != TASK_STOPPED)
			continue;

		/* If p is stopped by a debugger on a signal that won't
		   stop it, then don't count p as stopped.  This isn't
		   perfect but it's a good approximation.  */
		if (unlikely (p->ptrace)
		    && p->exit_code != SIGSTOP
		    && p->exit_code != SIGTSTP
		    && p->exit_code != SIGTTOU
		    && p->exit_code != SIGTTIN)
			continue;

		retval = 1;
		break;
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return retval;
}

/**
 * reparent_to_init() - Reparent the calling kernel thread to the init task.
 *
 * If a kernel thread is launched as a result of a system call, or if
 * it ever exits, it should generally reparent itself to init so that
 * it is correctly cleaned up on exit.
 *
 * The various task state such as scheduling policy and priority may have
 * been inherited from a user process, so we reset them to sane values here.
 *
 * NOTE that reparent_to_init() gives the caller full capabilities.
 */
void reparent_to_init(void)
{
	write_lock_irq(&tasklist_lock);

	ptrace_unlink(current);
	/* Reparent to init */
	REMOVE_LINKS(current);
	current->parent = child_reaper;
	current->real_parent = child_reaper;
	SET_LINKS(current);

	/* Set the exit signal to SIGCHLD so we signal init on exit */
	current->exit_signal = SIGCHLD;

	if ((current->policy == SCHED_NORMAL) && (task_nice(current) < 0))
		set_user_nice(current, 0);
	/* cpus_allowed? */
	/* rt_priority? */
	/* signals? */
	security_task_reparent_to_init(current);
	memcpy(current->signal->rlim, init_task.signal->rlim,
	       sizeof(current->signal->rlim));
	atomic_inc(&(INIT_USER->__count));
	write_unlock_irq(&tasklist_lock);
	switch_uid(INIT_USER);
}

void __set_special_pids(pid_t session, pid_t pgrp)
{
	struct task_struct *curr = current;

	if (curr->signal->session != session) {
		detach_pid(curr, PIDTYPE_SID);
		curr->signal->session = session;
		attach_pid(curr, PIDTYPE_SID, session);
	}
	if (process_group(curr) != pgrp) {
		detach_pid(curr, PIDTYPE_PGID);
		curr->signal->pgrp = pgrp;
		attach_pid(curr, PIDTYPE_PGID, pgrp);
	}
}

void set_special_pids(pid_t session, pid_t pgrp)
{
	write_lock_irq(&tasklist_lock);
	__set_special_pids(session, pgrp);
	write_unlock_irq(&tasklist_lock);
}

/*
 * Let kernel threads use this to say that they
 * allow a certain signal (since daemonize() will
 * have disabled all of them by default).
 */
int allow_signal(int sig)
{
	if (sig < 1 || sig > _NSIG)
		return -EINVAL;

	spin_lock_irq(&current->sighand->siglock);
	sigdelset(&current->blocked, sig);
	if (!current->mm) {
		/* Kernel threads handle their own signals.
		   Let the signal code know it'll be handled, so
		   that they don't get converted to SIGKILL or
		   just silently dropped */
		current->sighand->action[(sig)-1].sa.sa_handler = (void __user *)2;
	}
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

EXPORT_SYMBOL(allow_signal);

int disallow_signal(int sig)
{
	if (sig < 1 || sig > _NSIG)
		return -EINVAL;

	spin_lock_irq(&current->sighand->siglock);
	sigaddset(&current->blocked, sig);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

EXPORT_SYMBOL(disallow_signal);

/*
 *	Put all the gunge required to become a kernel thread without
 *	attached user resources in one place where it belongs.
 */

void daemonize(const char *name, ...)
{
	va_list args;
	struct fs_struct *fs;
	sigset_t blocked;

	va_start(args, name);
	vsnprintf(current->comm, sizeof(current->comm), name, args);
	va_end(args);

	/*
	 * If we were started as result of loading a module, close all of the
	 * user space pages.  We don't need them, and if we didn't close them
	 * they would be locked into memory.
	 */
	exit_mm(current);

	set_special_pids(1, 1);
	down(&tty_sem);
	current->signal->tty = NULL;
	up(&tty_sem);

	/* Block and flush all signals */
	sigfillset(&blocked);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	flush_signals(current);

	/* Become as one with the init task */

	exit_fs(current);	/* current->fs->count--; */
	fs = init_task.fs;
	current->fs = fs;
	atomic_inc(&fs->count);
 	exit_files(current);
	current->files = init_task.files;
	atomic_inc(&current->files->count);

	reparent_to_init();
}

EXPORT_SYMBOL(daemonize);

static inline void close_files(struct files_struct * files)
{
	int i, j;

	j = 0;
	for (;;) {
		unsigned long set;
		i = j * __NFDBITS;
		if (i >= files->max_fdset || i >= files->max_fds)
			break;
		set = files->open_fds->fds_bits[j++];
		while (set) {
			if (set & 1) {
				struct file * file = xchg(&files->fd[i], NULL);
				if (file)
					filp_close(file, files);
			}
			i++;
			set >>= 1;
		}
	}
}

struct files_struct *get_files_struct(struct task_struct *task)
{
	struct files_struct *files;

	task_lock(task);
	files = task->files;
	if (files)
		atomic_inc(&files->count);
	task_unlock(task);

	return files;
}

void fastcall put_files_struct(struct files_struct *files)
{
	if (atomic_dec_and_test(&files->count)) {
		close_files(files);
		/*
		 * Free the fd and fdset arrays if we expanded them.
		 */
		if (files->fd != &files->fd_array[0])
			free_fd_array(files->fd, files->max_fds);
		if (files->max_fdset > __FD_SETSIZE) {
			free_fdset(files->open_fds, files->max_fdset);
			free_fdset(files->close_on_exec, files->max_fdset);
		}
		kmem_cache_free(files_cachep, files);
	}
}

EXPORT_SYMBOL(put_files_struct);

/**
 * 从进程描述符中分离出与打开文件相关的数据结构。
 * 如果没有其他进程共享该结构，还删除所有这些数据结构。
 */
static inline void __exit_files(struct task_struct *tsk)
{
	struct files_struct * files = tsk->files;

	if (files) {
		task_lock(tsk);
		tsk->files = NULL;
		task_unlock(tsk);
		put_files_struct(files);
	}
}

void exit_files(struct task_struct *tsk)
{
	__exit_files(tsk);
}

static inline void __put_fs_struct(struct fs_struct *fs)
{
	/* No need to hold fs->lock if we are killing it */
	if (atomic_dec_and_test(&fs->count)) {
		dput(fs->root);
		mntput(fs->rootmnt);
		dput(fs->pwd);
		mntput(fs->pwdmnt);
		if (fs->altroot) {
			dput(fs->altroot);
			mntput(fs->altrootmnt);
		}
		kmem_cache_free(fs_cachep, fs);
	}
}

void put_fs_struct(struct fs_struct *fs)
{
	__put_fs_struct(fs);
}

/**
 * 从进程描述符中分离出与文件系统相关的数据结构。
 * 如果没有其他进程共享该结构，还删除所有这些数据结构。
 */
static inline void __exit_fs(struct task_struct *tsk)
{
	struct fs_struct * fs = tsk->fs;

	if (fs) {
		task_lock(tsk);
		tsk->fs = NULL;
		task_unlock(tsk);
		__put_fs_struct(fs);
	}
}

void exit_fs(struct task_struct *tsk)
{
	__exit_fs(tsk);
}

EXPORT_SYMBOL_GPL(exit_fs);

/*
 * Turn us into a lazy TLB process if we
 * aren't already..
 */
/**
 * 当进程结束时，exit_mm函数释放进程的地址空间。
 */
void exit_mm(struct task_struct * tsk)
{
	struct mm_struct *mm = tsk->mm;

	/**
	 * mm_release函数唤醒在vfork_done上睡眠的进程。一般的，只有当进程是通过vfork创建的，这个队列上才会有进程等待。
	 * 一般的，是进程的父进程在等待。
	 */
	mm_release(tsk, mm);
	/**
	 * 内核线程为什么就不继续后续操作？？
	 * 因为内核线程没有自己的内存描述符和相关的数据结构。
	 * 而后面的操作都释放内存描述符和相关数据结构。
	 */
	if (!mm)
		return;
	/*
	 * Serialize with any possible pending coredump.
	 * We must hold mmap_sem around checking core_waiters
	 * and clearing tsk->mm.  The core-inducing thread
	 * will increment core_waiters for each thread in the
	 * group with ->mm != NULL.
	 */
	down_read(&mm->mmap_sem);
	/**
	 * 需要转储，先获得信号量，使转储串行化。
	 */
	if (mm->core_waiters) {
		up_read(&mm->mmap_sem);
		down_write(&mm->mmap_sem);
		if (!--mm->core_waiters)
			complete(mm->core_startup_done);
		up_write(&mm->mmap_sem);

		wait_for_completion(&mm->core_done);
		down_read(&mm->mmap_sem);
	}
	/**
	 * 增加内存描述符的主使用计数器。
	 */
	atomic_inc(&mm->mm_count);
	if (mm != tsk->active_mm) BUG();
	/* more a memory barrier than a real lock */
	task_lock(tsk);
	/**
	 * 重新设置进程描述符的mm字段。
	 */
	tsk->mm = NULL;
	up_read(&mm->mmap_sem);
	/**
	 * 使处理器牌tlb懒惰模式。
	 */
	enter_lazy_tlb(mm, current);
	task_unlock(tsk);
	/**
	 * 前面已经增加使用计数了，现在开始通过mmput释放局部描述符、线性区描述符和页表。
	 * 当然，因为使用计数不会变成0，mmput不会释放mm描述符的。
	 * 它会在finish_task_switch中释放。
	 */
	mmput(mm);
}

static inline void choose_new_parent(task_t *p, task_t *reaper, task_t *child_reaper)
{
	/*
	 * Make sure we're not reparenting to ourselves and that
	 * the parent is not a zombie.
	 */
	BUG_ON(p == reaper || reaper->exit_state >= EXIT_ZOMBIE);
	p->real_parent = reaper;
	if (p->parent == p->real_parent)
		BUG();
}

static inline void reparent_thread(task_t *p, task_t *father, int traced)
{
	/* We don't want people slaying init.  */
	if (p->exit_signal != -1)
		p->exit_signal = SIGCHLD;

	if (p->pdeath_signal)
		/* We already hold the tasklist_lock here.  */
		group_send_sig_info(p->pdeath_signal, (void *) 0, p);

	/* Move the child from its dying parent to the new one.  */
	if (unlikely(traced)) {
		/* Preserve ptrace links if someone else is tracing this child.  */
		list_del_init(&p->ptrace_list);
		if (p->parent != p->real_parent)
			list_add(&p->ptrace_list, &p->real_parent->ptrace_children);
	} else {
		/* If this child is being traced, then we're the one tracing it
		 * anyway, so let go of it.
		 */
		p->ptrace = 0;
		list_del_init(&p->sibling);
		p->parent = p->real_parent;
		list_add_tail(&p->sibling, &p->parent->children);

		/* If we'd notified the old parent about this child's death,
		 * also notify the new parent.
		 */
		if (p->exit_state == EXIT_ZOMBIE && p->exit_signal != -1 &&
		    thread_group_empty(p))
			do_notify_parent(p, p->exit_signal);
		else if (p->state == TASK_TRACED) {
			/*
			 * If it was at a trace stop, turn it into
			 * a normal stop since it's no longer being
			 * traced.
			 */
			ptrace_untrace(p);
		}
	}

	/*
	 * process group orphan check
	 * Case ii: Our child is in a different pgrp
	 * than we are, and it was the only connection
	 * outside, so the child pgrp is now orphaned.
	 */
	if ((process_group(p) != process_group(father)) &&
	    (p->signal->session == father->signal->session)) {
		int pgrp = process_group(p);

		if (will_become_orphaned_pgrp(pgrp, NULL) && has_stopped_jobs(pgrp)) {
			__kill_pg_info(SIGHUP, (void *)1, pgrp);
			__kill_pg_info(SIGCONT, (void *)1, pgrp);
		}
	}
}

/*
 * When we die, we re-parent all our children.
 * Try to give them to another thread in our thread
 * group, and if no such member exists, give it to
 * the global child reaper process (ie "init")
 * 
 * 自己即将被终结，则需为自己的子进程寻找新的父
 */
static inline void forget_original_parent(struct task_struct * father,
					  struct list_head *to_release)
{
	struct task_struct *p, *reaper = father;
	struct list_head *_p, *_n;

	do {
		reaper = next_thread(reaper);
		if (reaper == father) {
			reaper = child_reaper;
			break;
		}
	} while (reaper->exit_state);

	/*
	 * There are only two places where our children can be:
	 *
	 * - in our child list
	 * - in our ptraced child list
	 *
	 * Search them and reparent children.
	 * 
	 * 给每一个子进程设置一个新的父进程
	 */
	list_for_each_safe(_p, _n, &father->children) {
		int ptrace;
		p = list_entry(_p,struct task_struct,sibling);

		ptrace = p->ptrace;

		/* if father isn't the real parent, then ptrace must be enabled */
		BUG_ON(father != p->real_parent && !ptrace);

		if (father == p->real_parent) {
			/* reparent with a reaper, real father it's us */
			choose_new_parent(p, reaper, child_reaper);
			reparent_thread(p, father, 0);
		} else {
			/* reparent ptraced task to its real parent */
			__ptrace_unlink (p);
			if (p->exit_state == EXIT_ZOMBIE && p->exit_signal != -1 &&
			    thread_group_empty(p))
				do_notify_parent(p, p->exit_signal);
		}

		/*
		 * if the ptraced child is a zombie with exit_signal == -1
		 * we must collect it before we exit, or it will remain
		 * zombie forever since we prevented it from self-reap itself
		 * while it was being traced by us, to be able to see it in wait4.
		 */
		if (unlikely(ptrace && p->exit_state == EXIT_ZOMBIE && p->exit_signal == -1))
			list_add(&p->ptrace_list, to_release);
	}
	list_for_each_safe(_p, _n, &father->ptrace_children) {
		p = list_entry(_p,struct task_struct,ptrace_list);
		choose_new_parent(p, reaper, child_reaper);
		reparent_thread(p, father, 1);
	}
}

/*
 * Send signals to all our closest relatives so that they know
 * to properly mourn us..
 */
/**
 * 当进程退出时，通知其他相关进程
 */
static void exit_notify(struct task_struct *tsk)
{
	int state;
	struct task_struct *t;
	struct list_head ptrace_dead, *_p, *_n;

	if (signal_pending(tsk) && !(tsk->signal->flags & SIGNAL_GROUP_EXIT)
	    && !thread_group_empty(tsk)) {
		/*
		 * This occurs when there was a race between our exit
		 * syscall and a group signal choosing us as the one to
		 * wake up.  It could be that we are the only thread
		 * alerted to check for pending signals, but another thread
		 * should be woken now to take the signal since we will not.
		 * Now we'll wake all the threads in the group just to make
		 * sure someone gets all the pending signals.
		 */
		read_lock(&tasklist_lock);
		spin_lock_irq(&tsk->sighand->siglock);
		for (t = next_thread(tsk); t != tsk; t = next_thread(t))
			if (!signal_pending(t) && !(t->flags & PF_EXITING)) {
				recalc_sigpending_tsk(t);
				if (signal_pending(t))
					signal_wake_up(t, 0);
			}
		spin_unlock_irq(&tsk->sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	write_lock_irq(&tasklist_lock);

	/*
	 * This does two things:
	 *
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */

	INIT_LIST_HEAD(&ptrace_dead);
	/**
	 * 更新父进程和子进程的亲属关系
	 * 即如果自己有子进程，但自己即将被终结，此时通知子进程寻找其他父进程
	 * 如果线程组中还有运行的进程，就让其中子线程作父进程。所谓：长兄如父，是也。
	 * 如果线程组中没有其他兄弟进程，就让子进程成为init的子进程。
	 */
	forget_original_parent(tsk, &ptrace_dead);
	BUG_ON(!list_empty(&tsk->children));
	BUG_ON(!list_empty(&tsk->ptrace_children));

	/*
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
	 */
	 
	t = tsk->real_parent;
	
	if ((process_group(t) != process_group(tsk)) &&
	    (t->signal->session == tsk->signal->session) &&
	    will_become_orphaned_pgrp(process_group(tsk), tsk) &&
	    has_stopped_jobs(process_group(tsk))) {
		__kill_pg_info(SIGHUP, (void *)1, process_group(tsk));
		__kill_pg_info(SIGCONT, (void *)1, process_group(tsk));
	}

	/* Let father know we died 
	 *
	 * Thread signals are configurable, but you aren't going to use
	 * that to send signals to arbitary processes. 
	 * That stops right now.
	 *
	 * If the parent exec id doesn't match the exec id we saved
	 * when we started then we know the parent has changed security
	 * domain.
	 *
	 * If our self_exec id doesn't match our parent_exec_id then
	 * we have changed execution domain as these two values started
	 * the same after a fork.
	 *	
	 */

	if (tsk->exit_signal != SIGCHLD && tsk->exit_signal != -1 &&
	    ( tsk->parent_exec_id != t->self_exec_id  ||
	      tsk->self_exec_id != tsk->parent_exec_id)
	    && !capable(CAP_KILL))
		tsk->exit_signal = SIGCHLD;


	/* If something other than our normal parent is ptracing us, then
	 * send it a SIGCHLD instead of honoring exit_signal.  exit_signal
	 * only has special meaning to our real parent.
	 */
	/**
	 * 检查终止进程其进程描述符的exit_signal字段是否不等于-1，并检查进程是否是其所属进程组的最后一个成员。
	 * 一般来说，正常进程都有这些条件，请参数copy_process中的注释。
	 * 这种情况下，给父进程发送一个信号（通常是SIGCHLD），以通知父进程死亡。
	 */
	if (tsk->exit_signal != -1 && thread_group_empty(tsk)) {
		int signal = tsk->parent == tsk->real_parent ? tsk->exit_signal : SIGCHLD;
		do_notify_parent(tsk, signal);
	} else if (tsk->ptrace) {
		/**
		 * 否则exit_signal等于-1或者线程组中还有其他进程，那么判断进程是否正在被跟踪，如果是
		 * 就向父进程发送一个SIGCHLD信号。
		 */
		do_notify_parent(tsk, SIGCHLD);
	}

	state = EXIT_ZOMBIE;
	/**
	 * exit_signal等于-1，并且没有被跟踪（注：正常的线程退出）
	 * 就将exit_state设置为EXIT_DEAD
	 * 否则设置为EXIT_ZOMBIE
	 */
	if (tsk->exit_signal == -1 &&
	    (likely(tsk->ptrace == 0) ||
	     unlikely(tsk->parent->signal->flags & SIGNAL_GROUP_EXIT)))
		state = EXIT_DEAD;
	tsk->exit_state = state;

	/*
	 * Clear these here so that update_process_times() won't try to deliver
	 * itimer, profile or rlimit signals to this task while it is in late exit.
	 */
	tsk->it_virt_value = cputime_zero;
	tsk->it_prof_value = cputime_zero;

	write_unlock_irq(&tasklist_lock);

	list_for_each_safe(_p, _n, &ptrace_dead) {
		list_del_init(_p);
		t = list_entry(_p,struct task_struct,ptrace_list);
		/**
		 * release_task后，进程描述符的使用计数变为1（不是0）。
		 * 请参考copy_process的代码，对引用计数的赋值。
		 * 它此时不会被释放，但是也快被释放了。
		 */
		release_task(t);
	}

	/* If the process is dead, release it - nobody will wait for it */
	if (state == EXIT_DEAD)
		release_task(tsk);

	/* PF_DEAD causes final put_task_struct after we schedule. */
	preempt_disable();
	/**
	 * 有了PF_DEAD标志，schedule就不会调度它了。
	 */
	tsk->flags |= PF_DEAD;
}

/**
 * 所有进程的终止都是本函数处理的。
 * 它从内核数据结构中删除对终止进程的大部分引用（注：不是全部，进程描述符就不是）
 * 它接受进程的终止代码作为参数。
 */
fastcall NORET_TYPE void do_exit(long code)
{
	struct task_struct *tsk = current;
	int group_dead;

	profile_task_exit(tsk);

	if (unlikely(in_interrupt()))
		panic("Aiee, killing interrupt handler!");
	if (unlikely(!tsk->pid))
		panic("Attempted to kill the idle task!");
	if (unlikely(tsk->pid == 1))
		panic("Attempted to kill init!");
	if (tsk->io_context)
		exit_io_context();

	if (unlikely(current->ptrace & PT_TRACE_EXIT)) {
		current->ptrace_message = code;
		ptrace_notify((PTRACE_EVENT_EXIT << 8) | SIGTRAP);
	}

	/**
	 * PF_EXITING表示进程的状态：正在被删除。
	 */
	tsk->flags |= PF_EXITING;
	/**
	 * 从动态定时器队列中删除进程描述符。
	 * 删除任一内核定时器，根据返回结果，确保没有定时器排队，也没有定时处理程序运行。
	 */
	del_timer_sync(&tsk->real_timer);

	if (unlikely(in_atomic()))
		printk(KERN_INFO "note: %s[%d] exited with preempt_count %d\n",
				current->comm, current->pid,
				preempt_count());

	//输出记账信息 
	acct_update_integrals();
	update_mem_hiwater();
	group_dead = atomic_dec_and_test(&tsk->signal->live);
	if (group_dead)
		acct_process(code);

	/**
	 * exit_mm从进程描述符中分离出分页相关的描述符。
	 * 如果没有其他进程共享这些数据结构，就删除这些数据结构。
	 */
	exit_mm(tsk);

	/**
	 * exit_sem从进程描述符中分离出信号量相关的描述符
	 */
	exit_sem(tsk);
	/**
	 * __exit_files从进程描述符中分离出文件系统相关的描述符
	 */
	__exit_files(tsk);
	/**
	 * __exit_fs从进程描述符中分离出打开文件描述符相关的描述符
	 */
	__exit_fs(tsk);
	/**
	 * exit_namespace从进程描述符中分离出命名空间相关的描述符
	 */	
	exit_namespace(tsk);
	/**
	 * exit_thread从进程描述符中分离出IO权限位图相关的描述符
	 */		
	exit_thread();
	exit_keys(tsk);

	if (group_dead && tsk->signal->leader)
		disassociate_ctty(1);

	/**
	 * 如果实现了被杀死进程的执行域和可执行格式的内核函数在内核模块中
	 * 就递减它们的值。
	 * 注：这应该是为了防止意外的卸载模块。
	 */
	module_put(tsk->thread_info->exec_domain->module);
	if (tsk->binfmt)
		module_put(tsk->binfmt->module);

	/**
	 * 设置退出代码
	 */
	tsk->exit_code = code;
	/**
	 * exit_notify执行比较复杂的操作，更新了很多内核数据结构
	 * 通知相关进程
	 */
	exit_notify(tsk);
#ifdef CONFIG_NUMA
	mpol_free(tsk->mempolicy);
	tsk->mempolicy = NULL;
#endif

	BUG_ON(!(current->flags & PF_DEAD));
	/**
	 * 完了，让其他线程运行吧
	 * 因为schedule会忽略处于EXIT_ZOMBIE状态的进程，所以进程现在是不会再运行了。
	 */
	schedule();
	/**
	 * 当然，谁还会让死掉的进程继续运行，说明内核一定是错了
	 * 注：难道schedule被谁改了，没有判断EXIT_ZOMBIE？？？
	 */
	BUG();
	/* Avoid "noreturn function does return".  */
	/**
	 * 仅仅为了防止编译器报警告信息而已，仅此而已。
	 */
	for (;;) ;
}

NORET_TYPE void complete_and_exit(struct completion *comp, long code)
{
	if (comp)
		complete(comp);
	
	do_exit(code);
}

EXPORT_SYMBOL(complete_and_exit);

asmlinkage long sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
}

/**
 * 返回PIDTYPE_TGID类型的散列表中，task指示的下一个轻量级进程的进程描述符。
 * 由于散列链表是循环的，若应用于传统进程，那么该返回的是进程本身的描述符地址。
 */
task_t fastcall *next_thread(const task_t *p)
{
	return pid_task(p->pids[PIDTYPE_TGID].pid_list.next, PIDTYPE_TGID);
}

EXPORT_SYMBOL(next_thread);

/*
 * Take down every thread in the group.  This is called by fatal signals
 * as well as by sys_exit_group (below).
 */
/**
 * 杀死属于current线程组的所有进程.它接受进程终止代码作为参数.
 * 这个参数可能是系统调用exit_group()指定的一个值,也可能是内核提供的一个错误代号.
 */
NORET_TYPE void
do_group_exit(int exit_code)
{
	BUG_ON(exit_code & 0x80); /* core dumps don't get here */

	/**
	 * 检查进程的SIGNAL_GROUP_EXIT,如果不为0,说明内核已经开始为线程组执行退出的过程.
	 */
	if (current->signal->flags & SIGNAL_GROUP_EXIT)
		exit_code = current->signal->group_exit_code;
	else if (!thread_group_empty(current)) {
		/**
		 * 设置进程的SIGNAL_GROUP_EXIT标志,并把终止代号放在sig->group_exit_code
		 */
		struct signal_struct *const sig = current->signal;
		struct sighand_struct *const sighand = current->sighand;
		read_lock(&tasklist_lock);
		spin_lock_irq(&sighand->siglock);
		if (sig->flags & SIGNAL_GROUP_EXIT)
			/* Another thread got here before we took the lock.  */
			exit_code = sig->group_exit_code;
		else {
			sig->flags = SIGNAL_GROUP_EXIT;
			sig->group_exit_code = exit_code;
			/**
			 * zap_other_threads杀死线程组中的其他线程.
			 * 它扫描PIDTYPE_TGID类型的散列表中的每个PID链表,向表中其他进程发送SIGKILL信号.
			 */
			zap_other_threads(current);
		}
		spin_unlock_irq(&sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	/**
	 * 杀死当前进程,此过程不再返回.
	 */
	do_exit(exit_code);
	/* NOTREACHED */
}

/*
 * this kills every thread in the thread group. Note that any externally
 * wait4()-ing process will get the correct exit code - even if this
 * thread is not the thread group leader.
 */
asmlinkage void sys_exit_group(int error_code)
{
	do_group_exit((error_code & 0xff) << 8);
}

static int eligible_child(pid_t pid, int options, task_t *p)
{
	if (pid > 0) {
		if (p->pid != pid)
			return 0;
	} else if (!pid) {
		if (process_group(p) != process_group(current))
			return 0;
	} else if (pid != -1) {
		if (process_group(p) != -pid)
			return 0;
	}

	/*
	 * Do not consider detached threads that are
	 * not ptraced:
	 */
	if (p->exit_signal == -1 && !p->ptrace)
		return 0;

	/* Wait for all children (clone and not) if __WALL is set;
	 * otherwise, wait for clone children *only* if __WCLONE is
	 * set; otherwise, wait for non-clone children *only*.  (Note:
	 * A "clone" child here is one that reports to its parent
	 * using a signal other than SIGCHLD.) */
	if (((p->exit_signal != SIGCHLD) ^ ((options & __WCLONE) != 0))
	    && !(options & __WALL))
		return 0;
	/*
	 * Do not consider thread group leaders that are
	 * in a non-empty thread group:
	 */
	if (current->tgid != p->tgid && delay_group_leader(p))
		return 2;

	if (security_task_wait(p))
		return 0;

	return 1;
}

static int wait_noreap_copyout(task_t *p, pid_t pid, uid_t uid,
			       int why, int status,
			       struct siginfo __user *infop,
			       struct rusage __user *rusagep)
{
	int retval = rusagep ? getrusage(p, RUSAGE_BOTH, rusagep) : 0;
	put_task_struct(p);
	if (!retval)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval)
		retval = put_user(0, &infop->si_errno);
	if (!retval)
		retval = put_user((short)why, &infop->si_code);
	if (!retval)
		retval = put_user(pid, &infop->si_pid);
	if (!retval)
		retval = put_user(uid, &infop->si_uid);
	if (!retval)
		retval = put_user(status, &infop->si_status);
	if (!retval)
		retval = pid;
	return retval;
}

/*
 * Handle sys_wait4 work for one task in state EXIT_ZOMBIE.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_zombie(task_t *p, int noreap,
			    struct siginfo __user *infop,
			    int __user *stat_addr, struct rusage __user *ru)
{
	unsigned long state;
	int retval;
	int status;

	if (unlikely(noreap)) {
		pid_t pid = p->pid;
		uid_t uid = p->uid;
		int exit_code = p->exit_code;
		int why, status;

		if (unlikely(p->exit_state != EXIT_ZOMBIE))
			return 0;
		if (unlikely(p->exit_signal == -1 && p->ptrace == 0))
			return 0;
		get_task_struct(p);
		read_unlock(&tasklist_lock);
		if ((exit_code & 0x7f) == 0) {
			why = CLD_EXITED;
			status = exit_code >> 8;
		} else {
			why = (exit_code & 0x80) ? CLD_DUMPED : CLD_KILLED;
			status = exit_code & 0x7f;
		}
		return wait_noreap_copyout(p, pid, uid, why,
					   status, infop, ru);
	}

	/*
	 * Try to move the task's state to DEAD
	 * only one thread is allowed to do this:
	 */
	state = xchg(&p->exit_state, EXIT_DEAD);
	if (state != EXIT_ZOMBIE) {
		BUG_ON(state != EXIT_DEAD);
		return 0;
	}
	if (unlikely(p->exit_signal == -1 && p->ptrace == 0)) {
		/*
		 * This can only happen in a race with a ptraced thread
		 * dying on another processor.
		 */
		return 0;
	}

	if (likely(p->real_parent == p->parent) && likely(p->signal)) {
		/*
		 * The resource counters for the group leader are in its
		 * own task_struct.  Those for dead threads in the group
		 * are in its signal_struct, as are those for the child
		 * processes it has previously reaped.  All these
		 * accumulate in the parent's signal_struct c* fields.
		 *
		 * We don't bother to take a lock here to protect these
		 * p->signal fields, because they are only touched by
		 * __exit_signal, which runs with tasklist_lock
		 * write-locked anyway, and so is excluded here.  We do
		 * need to protect the access to p->parent->signal fields,
		 * as other threads in the parent group can be right
		 * here reaping other children at the same time.
		 */
		spin_lock_irq(&p->parent->sighand->siglock);
		p->parent->signal->cutime =
			cputime_add(p->parent->signal->cutime,
			cputime_add(p->utime,
			cputime_add(p->signal->utime,
				    p->signal->cutime)));
		p->parent->signal->cstime =
			cputime_add(p->parent->signal->cstime,
			cputime_add(p->stime,
			cputime_add(p->signal->stime,
				    p->signal->cstime)));
		p->parent->signal->cmin_flt +=
			p->min_flt + p->signal->min_flt + p->signal->cmin_flt;
		p->parent->signal->cmaj_flt +=
			p->maj_flt + p->signal->maj_flt + p->signal->cmaj_flt;
		p->parent->signal->cnvcsw +=
			p->nvcsw + p->signal->nvcsw + p->signal->cnvcsw;
		p->parent->signal->cnivcsw +=
			p->nivcsw + p->signal->nivcsw + p->signal->cnivcsw;
		spin_unlock_irq(&p->parent->sighand->siglock);
	}

	/*
	 * Now we are sure this task is interesting, and no other
	 * thread can reap it because we set its state to EXIT_DEAD.
	 */
	read_unlock(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	status = (p->signal->flags & SIGNAL_GROUP_EXIT)
		? p->signal->group_exit_code : p->exit_code;
	if (!retval && stat_addr)
		retval = put_user(status, stat_addr);
	if (!retval && infop)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval && infop)
		retval = put_user(0, &infop->si_errno);
	if (!retval && infop) {
		int why;

		if ((status & 0x7f) == 0) {
			why = CLD_EXITED;
			status >>= 8;
		} else {
			why = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
			status &= 0x7f;
		}
		retval = put_user((short)why, &infop->si_code);
		if (!retval)
			retval = put_user(status, &infop->si_status);
	}
	if (!retval && infop)
		retval = put_user(p->pid, &infop->si_pid);
	if (!retval && infop)
		retval = put_user(p->uid, &infop->si_uid);
	if (retval) {
		// TODO: is this safe?
		p->exit_state = EXIT_ZOMBIE;
		return retval;
	}
	retval = p->pid;
	if (p->real_parent != p->parent) {
		write_lock_irq(&tasklist_lock);
		/* Double-check with lock held.  */
		if (p->real_parent != p->parent) {
			__ptrace_unlink(p);
			// TODO: is this safe?
			p->exit_state = EXIT_ZOMBIE;
			/*
			 * If this is not a detached task, notify the parent.
			 * If it's still not detached after that, don't release
			 * it now.
			 */
			if (p->exit_signal != -1) {
				do_notify_parent(p, p->exit_signal);
				if (p->exit_signal != -1)
					p = NULL;
			}
		}
		write_unlock_irq(&tasklist_lock);
	}
	if (p != NULL)
		release_task(p);
	BUG_ON(!retval);
	return retval;
}

/*
 * Handle sys_wait4 work for one task in state TASK_STOPPED.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_stopped(task_t *p, int delayed_group_leader, int noreap,
			     struct siginfo __user *infop,
			     int __user *stat_addr, struct rusage __user *ru)
{
	int retval, exit_code;

	if (!p->exit_code)
		return 0;
	if (delayed_group_leader && !(p->ptrace & PT_PTRACED) &&
	    p->signal && p->signal->group_stop_count > 0)
		/*
		 * A group stop is in progress and this is the group leader.
		 * We won't report until all threads have stopped.
		 */
		return 0;

	/*
	 * Now we are pretty sure this task is interesting.
	 * Make sure it doesn't get reaped out from under us while we
	 * give up the lock and then examine it below.  We don't want to
	 * keep holding onto the tasklist_lock while we call getrusage and
	 * possibly take page faults for user memory.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	if (unlikely(noreap)) {
		pid_t pid = p->pid;
		uid_t uid = p->uid;
		int why = (p->ptrace & PT_PTRACED) ? CLD_TRAPPED : CLD_STOPPED;

		exit_code = p->exit_code;
		if (unlikely(!exit_code) ||
		    unlikely(p->state > TASK_STOPPED))
			goto bail_ref;
		return wait_noreap_copyout(p, pid, uid,
					   why, (exit_code << 8) | 0x7f,
					   infop, ru);
	}

	write_lock_irq(&tasklist_lock);

	/*
	 * This uses xchg to be atomic with the thread resuming and setting
	 * it.  It must also be done with the write lock held to prevent a
	 * race with the EXIT_ZOMBIE case.
	 */
	exit_code = xchg(&p->exit_code, 0);
	if (unlikely(p->exit_state)) {
		/*
		 * The task resumed and then died.  Let the next iteration
		 * catch it in EXIT_ZOMBIE.  Note that exit_code might
		 * already be zero here if it resumed and did _exit(0).
		 * The task itself is dead and won't touch exit_code again;
		 * other processors in this function are locked out.
		 */
		p->exit_code = exit_code;
		exit_code = 0;
	}
	if (unlikely(exit_code == 0)) {
		/*
		 * Another thread in this function got to it first, or it
		 * resumed, or it resumed and then died.
		 */
		write_unlock_irq(&tasklist_lock);
bail_ref:
		put_task_struct(p);
		/*
		 * We are returning to the wait loop without having successfully
		 * removed the process and having released the lock. We cannot
		 * continue, since the "p" task pointer is potentially stale.
		 *
		 * Return -EAGAIN, and do_wait() will restart the loop from the
		 * beginning. Do _not_ re-acquire the lock.
		 */
		return -EAGAIN;
	}

	/* move to end of parent's list to avoid starvation */
	remove_parent(p);
	add_parent(p, p->parent);

	write_unlock_irq(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	if (!retval && stat_addr)
		retval = put_user((exit_code << 8) | 0x7f, stat_addr);
	if (!retval && infop)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval && infop)
		retval = put_user(0, &infop->si_errno);
	if (!retval && infop)
		retval = put_user((short)((p->ptrace & PT_PTRACED)
					  ? CLD_TRAPPED : CLD_STOPPED),
				  &infop->si_code);
	if (!retval && infop)
		retval = put_user(exit_code, &infop->si_status);
	if (!retval && infop)
		retval = put_user(p->pid, &infop->si_pid);
	if (!retval && infop)
		retval = put_user(p->uid, &infop->si_uid);
	if (!retval)
		retval = p->pid;
	put_task_struct(p);

	BUG_ON(!retval);
	return retval;
}

/*
 * Handle do_wait work for one task in a live, non-stopped state.
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_continued(task_t *p, int noreap,
			       struct siginfo __user *infop,
			       int __user *stat_addr, struct rusage __user *ru)
{
	int retval;
	pid_t pid;
	uid_t uid;

	if (unlikely(!p->signal))
		return 0;

	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED))
		return 0;

	spin_lock_irq(&p->sighand->siglock);
	/* Re-check with the lock held.  */
	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED)) {
		spin_unlock_irq(&p->sighand->siglock);
		return 0;
	}
	if (!noreap)
		p->signal->flags &= ~SIGNAL_STOP_CONTINUED;
	spin_unlock_irq(&p->sighand->siglock);

	pid = p->pid;
	uid = p->uid;
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	if (!infop) {
		retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
		put_task_struct(p);
		if (!retval && stat_addr)
			retval = put_user(0xffff, stat_addr);
		if (!retval)
			retval = p->pid;
	} else {
		retval = wait_noreap_copyout(p, pid, uid,
					     CLD_CONTINUED, SIGCONT,
					     infop, ru);
		BUG_ON(retval == 0);
	}

	return retval;
}


static inline int my_ptrace_child(struct task_struct *p)
{
	if (!(p->ptrace & PT_PTRACED))
		return 0;
	if (!(p->ptrace & PT_ATTACHED))
		return 1;
	/*
	 * This child was PTRACE_ATTACH'd.  We should be seeing it only if
	 * we are the attacher.  If we are the real parent, this is a race
	 * inside ptrace_attach.  It is waiting for the tasklist_lock,
	 * which we have to switch the parent links, but has already set
	 * the flags in p->ptrace.
	 */
	return (p->parent != p->real_parent);
}

static long do_wait(pid_t pid, int options, struct siginfo __user *infop,
		    int __user *stat_addr, struct rusage __user *ru)
{
	DECLARE_WAITQUEUE(wait, current);
	struct task_struct *tsk;
	int flag, retval;

	add_wait_queue(&current->signal->wait_chldexit,&wait);
repeat:
	/*
	 * We will set this flag if we see any child that might later
	 * match our criteria, even if we are not able to reap it yet.
	 */
	flag = 0;
	current->state = TASK_INTERRUPTIBLE;
	read_lock(&tasklist_lock);
	tsk = current;
	do {
		struct task_struct *p;
		struct list_head *_p;
		int ret;

		list_for_each(_p,&tsk->children) {
			p = list_entry(_p,struct task_struct,sibling);

			ret = eligible_child(pid, options, p);
			if (!ret)
				continue;

			switch (p->state) {
			case TASK_TRACED:
				if (!my_ptrace_child(p))
					continue;
				/*FALLTHROUGH*/
			case TASK_STOPPED:
				/*
				 * It's stopped now, so it might later
				 * continue, exit, or stop again.
				 */
				flag = 1;
				if (!(options & WUNTRACED) &&
				    !my_ptrace_child(p))
					continue;
				retval = wait_task_stopped(p, ret == 2,
							   (options & WNOWAIT),
							   infop,
							   stat_addr, ru);
				if (retval == -EAGAIN)
					goto repeat;
				if (retval != 0) /* He released the lock.  */
					goto end;
				break;
			default:
			// case EXIT_DEAD:
				if (p->exit_state == EXIT_DEAD)
					continue;
			// case EXIT_ZOMBIE:
				if (p->exit_state == EXIT_ZOMBIE) {
					/*
					 * Eligible but we cannot release
					 * it yet:
					 */
					if (ret == 2)
						goto check_continued;
					if (!likely(options & WEXITED))
						continue;
					retval = wait_task_zombie(
						p, (options & WNOWAIT),
						infop, stat_addr, ru);
					/* He released the lock.  */
					if (retval != 0)
						goto end;
					break;
				}
check_continued:
				/*
				 * It's running now, so it might later
				 * exit, stop, or stop and then continue.
				 */
				flag = 1;
				if (!unlikely(options & WCONTINUED))
					continue;
				retval = wait_task_continued(
					p, (options & WNOWAIT),
					infop, stat_addr, ru);
				if (retval != 0) /* He released the lock.  */
					goto end;
				break;
			}
		}
		if (!flag) {
			list_for_each(_p, &tsk->ptrace_children) {
				p = list_entry(_p, struct task_struct,
						ptrace_list);
				if (!eligible_child(pid, options, p))
					continue;
				flag = 1;
				break;
			}
		}
		if (options & __WNOTHREAD)
			break;
		tsk = next_thread(tsk);
		if (tsk->signal != current->signal)
			BUG();
	} while (tsk != current);

	read_unlock(&tasklist_lock);
	if (flag) {
		retval = 0;
		if (options & WNOHANG)
			goto end;
		retval = -ERESTARTSYS;
		if (signal_pending(current))
			goto end;
		schedule();
		goto repeat;
	}
	retval = -ECHILD;
end:
	current->state = TASK_RUNNING;
	remove_wait_queue(&current->signal->wait_chldexit,&wait);
	if (infop) {
		if (retval > 0)
		retval = 0;
		else {
			/*
			 * For a WNOHANG return, clear out all the fields
			 * we would set so the user can easily tell the
			 * difference.
			 */
			if (!retval)
				retval = put_user(0, &infop->si_signo);
			if (!retval)
				retval = put_user(0, &infop->si_errno);
			if (!retval)
				retval = put_user(0, &infop->si_code);
			if (!retval)
				retval = put_user(0, &infop->si_pid);
			if (!retval)
				retval = put_user(0, &infop->si_uid);
			if (!retval)
				retval = put_user(0, &infop->si_status);
		}
	}
	return retval;
}

asmlinkage long sys_waitid(int which, pid_t pid,
			   struct siginfo __user *infop, int options,
			   struct rusage __user *ru)
{
	long ret;

	if (options & ~(WNOHANG|WNOWAIT|WEXITED|WSTOPPED|WCONTINUED))
		return -EINVAL;
	if (!(options & (WEXITED|WSTOPPED|WCONTINUED)))
		return -EINVAL;

	switch (which) {
	case P_ALL:
		pid = -1;
		break;
	case P_PID:
		if (pid <= 0)
			return -EINVAL;
		break;
	case P_PGID:
		if (pid <= 0)
			return -EINVAL;
		pid = -pid;
		break;
	default:
		return -EINVAL;
	}

	ret = do_wait(pid, options, infop, NULL, ru);

	/* avoid REGPARM breakage on x86: */
	prevent_tail_call(ret);
	return ret;
}

asmlinkage long sys_wait4(pid_t pid, int __user *stat_addr,
			  int options, struct rusage __user *ru)
{
	long ret;

	if (options & ~(WNOHANG|WUNTRACED|WCONTINUED|
			__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;
	ret = do_wait(pid, options | WEXITED, NULL, stat_addr, ru);

	/* avoid REGPARM breakage on x86: */
	prevent_tail_call(ret);
	return ret;
}

#ifdef __ARCH_WANT_SYS_WAITPID

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 */
asmlinkage long sys_waitpid(pid_t pid, int __user *stat_addr, int options)
{
	return sys_wait4(pid, stat_addr, options, NULL);
}

#endif
