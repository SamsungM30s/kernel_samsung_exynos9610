/*
 *  linux/kernel/panic.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This function is used through-out the kernel (including mm and fs)
 * to indicate a major problem.
 */
#include <linux/debug_locks.h>
#include <linux/sched/debug.h>
#include <linux/interrupt.h>
#include <linux/kmsg_dump.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/vt_kern.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/ftrace.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/kexec.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/nmi.h>
#include <linux/console.h>
#include <linux/bug.h>
#include <linux/ratelimit.h>
#include <linux/debug-snapshot.h>

#ifdef CONFIG_SEC_DEBUG_EXTRA_INFO
#include <linux/sec_debug.h>
#endif
#include <linux/sysfs.h>

#define PANIC_TIMER_STEP 100
#define PANIC_BLINK_SPD 18

int panic_on_oops = CONFIG_PANIC_ON_OOPS_VALUE;
static unsigned long tainted_mask;
static int pause_on_oops;
static int pause_on_oops_flag;
static DEFINE_SPINLOCK(pause_on_oops_lock);
bool crash_kexec_post_notifiers;
int panic_on_warn __read_mostly;
static unsigned int warn_limit __read_mostly;

int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);

ATOMIC_NOTIFIER_HEAD(panic_notifier_list);

EXPORT_SYMBOL(panic_notifier_list);

#ifdef CONFIG_SYSCTL
static struct ctl_table kern_panic_table[] = {
	{
		.procname       = "warn_limit",
		.data           = &warn_limit,
		.maxlen         = sizeof(warn_limit),
		.mode           = 0644,
		.proc_handler   = proc_douintvec,
	},
	{ }
};

static __init int kernel_panic_sysctls_init(void)
{
	register_sysctl_init("kernel", kern_panic_table);
	return 0;
}
late_initcall(kernel_panic_sysctls_init);
#endif

static atomic_t warn_count = ATOMIC_INIT(0);

#ifdef CONFIG_SYSFS
static ssize_t warn_count_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *page)
{
	return sysfs_emit(page, "%d\n", atomic_read(&warn_count));
}

static struct kobj_attribute warn_count_attr = __ATTR_RO(warn_count);

static __init int kernel_panic_sysfs_init(void)
{
	sysfs_add_file_to_group(kernel_kobj, &warn_count_attr.attr, NULL);
	return 0;
}
late_initcall(kernel_panic_sysfs_init);
#endif

static long no_blink(int state)
{
	return 0;
}

/* Returns how long it waited in ms */
long (*panic_blink)(int state);
EXPORT_SYMBOL(panic_blink);

/*
 * Stop ourself in panic -- architecture code may override this
 */
void __weak panic_smp_self_stop(void)
{
	while (1)
		cpu_relax();
}

/*
 * Stop ourselves in NMI context if another CPU has already panicked. Arch code
 * may override this to prepare for crash dumping, e.g. save regs info.
 */
void __weak nmi_panic_self_stop(struct pt_regs *regs)
{
	panic_smp_self_stop();
}

/*
 * Stop other CPUs in panic.  Architecture dependent code may override this
 * with more suitable version.  For example, if the architecture supports
 * crash dump, it should save registers of each stopped CPU and disable
 * per-CPU features such as virtualization extensions.
 */
void __weak crash_smp_send_stop(void)
{
	static int cpus_stopped;

	/*
	 * This function can be called twice in panic path, but obviously
	 * we execute this only once.
	 */
	if (cpus_stopped)
		return;

	/*
	 * Note smp_send_stop is the usual smp shutdown function, which
	 * unfortunately means it may not be hardened to work in a panic
	 * situation.
	 */
	smp_send_stop();
	cpus_stopped = 1;
}

atomic_t panic_cpu = ATOMIC_INIT(PANIC_CPU_INVALID);

/*
 * A variant of panic() called from NMI context. We return if we've already
 * panicked on this CPU. If another CPU already panicked, loop in
 * nmi_panic_self_stop() which can provide architecture dependent code such
 * as saving register state for crash dump.
 */
void nmi_panic(struct pt_regs *regs, const char *msg)
{
	int old_cpu, cpu;

	cpu = raw_smp_processor_id();
	old_cpu = atomic_cmpxchg(&panic_cpu, PANIC_CPU_INVALID, cpu);

	if (old_cpu == PANIC_CPU_INVALID)
		panic("%s", msg);
	else if (old_cpu != cpu)
		nmi_panic_self_stop(regs);
}
EXPORT_SYMBOL(nmi_panic);

void check_panic_on_warn(const char *origin)
{
	unsigned int limit;

	if (panic_on_warn)
		panic("%s: panic_on_warn set ...\n", origin);

	limit = READ_ONCE(warn_limit);
	if (atomic_inc_return(&warn_count) >= limit && limit)
		panic("%s: system warned too often (kernel.warn_limit is %d)",
		      origin, limit);
}

/**
 *	panic - halt the system
 *	@fmt: The text string to print
 *
 *	Display a message, then perform cleanups.
 *
 *	This function never returns.
 */
void panic(const char *fmt, ...)
{
	static char buf[1024];
	va_list args;
	long i, i_next = 0;
	int state = 0;
	int old_cpu, this_cpu;
	bool _crash_kexec_post_notifiers = crash_kexec_post_notifiers;
#ifdef CONFIG_SEC_DEBUG_EXTRA_INFO
	struct pt_regs regs;

	regs.regs[30] = _RET_IP_;
	regs.pc = regs.regs[30] - sizeof(unsigned int);
#endif

	/*
	* dbg_snapshot_early_panic is for supporting wapper functions
	* to users need to run SoC specific function in NOT interrupt
	* context
	*/
	dbg_snapshot_early_panic();

	if (panic_on_warn) {
		/*
		 * This thread may hit another WARN() in the panic path.
		 * Resetting this prevents additional WARN() from panicking the
		 * system on this thread.  Other threads are blocked by the
		 * panic_mutex in panic().
		 */
		panic_on_warn = 0;
	}

	/*
	 * Disable local interrupts. This will prevent panic_smp_self_stop
	 * from deadlocking the first cpu that invokes the panic, since
	 * there is nothing to prevent an interrupt handler (that runs
	 * after setting panic_cpu) from invoking panic() again.
	 */
	local_irq_disable();
	preempt_disable_notrace();

	/*
	 * It's possible to come here directly from a panic-assertion and
	 * not have preempt disabled. Some functions called from here want
	 * preempt to be disabled. No point enabling it later though...
	 *
	 * Only one CPU is allowed to execute the panic code from here. For
	 * multiple parallel invocations of panic, all other CPUs either
	 * stop themself or will wait until they are stopped by the 1st CPU
	 * with smp_send_stop().
	 *
	 * `old_cpu == PANIC_CPU_INVALID' means this is the 1st CPU which
	 * comes here, so go ahead.
	 * `old_cpu == this_cpu' means we came from nmi_panic() which sets
	 * panic_cpu to this CPU.  In this case, this is also the 1st CPU.
	 */
	this_cpu = raw_smp_processor_id();
	old_cpu  = atomic_cmpxchg(&panic_cpu, PANIC_CPU_INVALID, this_cpu);

	if (old_cpu != PANIC_CPU_INVALID) {
		dbg_snapshot_hook_hardlockup_exit();
		panic_smp_self_stop();
	}

	console_verbose();
	bust_spinlocks(1);
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

#ifdef CONFIG_SEC_DEBUG_AUTO_COMMENT
	if (buf[strlen(buf) - 1] == '\n')
		buf[strlen(buf) - 1] = '\0';
#endif
#ifdef CONFIG_SEC_DEBUG_EXTRA_INFO
	if (strncmp(buf, "Fatal exception", 15))
		sec_debug_set_extra_info_fault(PANIC_FAULT, (unsigned long)regs.pc, &regs);
#endif
	pr_auto(ASL5, "Kernel panic - not syncing: %s\n", buf);

	dbg_snapshot_prepare_panic();
	dbg_snapshot_dump_panic(buf, (size_t)strnlen(buf, sizeof(buf)));
#ifdef CONFIG_DEBUG_BUGVERBOSE
	/*
	 * Avoid nested stack-dumping if a panic occurs during oops processing
	 */
	if (!test_taint(TAINT_DIE) && oops_in_progress <= 1)
		dump_stack();
#endif
	//sysrq_sched_debug_show();

	/*
	 * If we have crashed and we have a crash kernel loaded let it handle
	 * everything else.
	 * If we want to run this after calling panic_notifiers, pass
	 * the "crash_kexec_post_notifiers" option to the kernel.
	 *
	 * Bypass the panic_cpu check and call __crash_kexec directly.
	 */
	if (!_crash_kexec_post_notifiers) {
		printk_safe_flush_on_panic();
		__crash_kexec(NULL);

		/*
		 * Note smp_send_stop is the usual smp shutdown function, which
		 * unfortunately means it may not be hardened to work in a
		 * panic situation.
		 */
		smp_send_stop();
	} else {
		/*
		 * If we want to do crash dump after notifier calls and
		 * kmsg_dump, we will need architecture dependent extra
		 * works in addition to stopping other CPUs.
		 */
		crash_smp_send_stop();
	}

	/*
	 * Run any panic handlers, including those that might need to
	 * add information to the kmsg dump output.
	 */
	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	/* Call flush even twice. It tries harder with a single online CPU */
	printk_safe_flush_on_panic();
	kmsg_dump(KMSG_DUMP_PANIC);

	dbg_snapshot_post_panic();

	/*
	 * If you doubt kdump always works fine in any situation,
	 * "crash_kexec_post_notifiers" offers you a chance to run
	 * panic_notifiers and dumping kmsg before kdump.
	 * Note: since some panic_notifiers can make crashed kernel
	 * more unstable, it can increase risks of the kdump failure too.
	 *
	 * Bypass the panic_cpu check and call __crash_kexec directly.
	 */
	if (_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

#ifdef CONFIG_VT
	unblank_screen();
#endif
	console_unblank();

	/*
	 * We may have ended up stopping the CPU holding the lock (in
	 * smp_send_stop()) while still having some valuable data in the console
	 * buffer.  Try to acquire the lock then release it regardless of the
	 * result.  The release will also print the buffers out.  Locks debug
	 * should be disabled to avoid reporting bad unlock balance when
	 * panic() is not being callled from OOPS.
	 */
	debug_locks_off();
	console_flush_on_panic();

	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		/*
		 * Delay timeout seconds before rebooting the machine.
		 * We can't use the "normal" timers since we just panicked.
		 */
		pr_emerg("Rebooting in %d seconds..\n", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}
	if (panic_timeout != 0) {
		/*
		 * This will not be a clean reboot, with everything
		 * shutting down.  But if there is a chance of
		 * rebooting the system it will be rebooted.
		 */
		emergency_restart();
	}
#ifdef __sparc__
	{
		extern int stop_a_enabled;
		/* Make sure the user can actually press Stop-A (L1-A) */
		stop_a_enabled = 1;
		pr_emerg("Press Stop-A (L1-A) from sun keyboard or send break\n"
			 "twice on console to return to the boot prom\n");
	}
#endif
#if defined(CONFIG_S390)
	{
		unsigned long caller;

		caller = (unsigned long)__builtin_return_address(0);
		disabled_wait(caller);
	}
#endif
	pr_emerg("---[ end Kernel panic - not syncing: %s\n", buf);
	local_irq_enable();
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}

EXPORT_SYMBOL(panic);

/*
 * TAINT_FORCED_RMMOD could be a per-module flag but the module
 * is being removed anyway.
 */
const struct taint_flag taint_flags[TAINT_FLAGS_COUNT] = {
	{ 'P', 'G', true },	/* TAINT_PROPRIETARY_MODULE */
	{ 'F', ' ', true },	/* TAINT_FORCED_MODULE */
	{ 'S', ' ', false },	/* TAINT_CPU_OUT_OF_SPEC */
	{ 'R', ' ', false },	/* TAINT_FORCED_RMMOD */
	{ 'M', ' ', false },	/* TAINT_MACHINE_CHECK */
	{ 'B', ' ', false },	/* TAINT_BAD_PAGE */
	{ 'U', ' ', false },	/* TAINT_USER */
	{ 'D', ' ', false },	/* TAINT_DIE */
	{ 'A', ' ', false },	/* TAINT_OVERRIDDEN_ACPI_TABLE */
	{ 'W', ' ', false },	/* TAINT_WARN */
	{ 'C', ' ', true },	/* TAINT_CRAP */
	{ 'I', ' ', false },	/* TAINT_FIRMWARE_WORKAROUND */
	{ 'O', ' ', true },	/* TAINT_OOT_MODULE */
	{ 'E', ' ', true },	/* TAINT_UNSIGNED_MODULE */
	{ 'L', ' ', false },	/* TAINT_SOFTLOCKUP */
	{ 'K', ' ', true },	/* TAINT_LIVEPATCH */
};

/**
 *	print_tainted - return a string to represent the kernel taint state.
 *
 *  'P' - Proprietary module has been loaded.
 *  'F' - Module has been forcibly loaded.
 *  'S' - SMP with CPUs not designed for SMP.
 *  'R' - User forced a module unload.
 *  'M' - System experienced a machine check exception.
 *  'B' - System has hit bad_page.
 *  'U' - Userspace-defined naughtiness.
 *  'D' - Kernel has oopsed before
 *  'A' - ACPI table overridden.
 *  'W' - Taint on warning.
 *  'C' - modules from drivers/staging are loaded.
 *  'I' - Working around severe firmware bug.
 *  'O' - Out-of-tree module has been loaded.
 *  'E' - Unsigned module has been loaded.
 *  'L' - A soft lockup has previously occurred.
 *  'K' - Kernel has been live patched.
 *
 *	The string is overwritten by the next call to print_tainted().
 */
const char *print_tainted(void)
{
	static char buf[TAINT_FLAGS_COUNT + sizeof("Tainted: ")];

	if (tainted_mask) {
		char *s;
		int i;

		s = buf + sprintf(buf, "Tainted: ");
		for (i = 0; i < TAINT_FLAGS_COUNT; i++) {
			const struct taint_flag *t = &taint_flags[i];
			*s++ = test_bit(i, &tainted_mask) ?
					t->c_true : t->c_false;
		}
		*s = 0;
	} else
		snprintf(buf, sizeof(buf), "Not tainted");

	return buf;
}

int test_taint(unsigned flag)
{
	return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

unsigned long get_taint(void)
{
	return tainted_mask;
}

/**
 * add_taint: add a taint flag if not already set.
 * @flag: one of the TAINT_* constants.
 * @lockdep_ok: whether lock debugging is still OK.
 *
 * If something bad has gone wrong, you'll want @lockdebug_ok = false, but for
 * some notewortht-but-not-corrupting cases, it can be set to true.
 */
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
	if (lockdep_ok == LOCKDEP_NOW_UNRELIABLE && __debug_locks_off())
		pr_warn("Disabling lock debugging due to kernel taint\n");

	set_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(add_taint);

static void spin_msec(int msecs)
{
	int i;

	for (i = 0; i < msecs; i++) {
		touch_nmi_watchdog();
		mdelay(1);
	}
}

/*
 * It just happens that oops_enter() and oops_exit() are identically
 * implemented...
 */
static void do_oops_enter_exit(void)
{
	unsigned long flags;
	static int spin_counter;

	if (!pause_on_oops)
		return;

	spin_lock_irqsave(&pause_on_oops_lock, flags);
	if (pause_on_oops_flag == 0) {
		/* This CPU may now print the oops message */
		pause_on_oops_flag = 1;
	} else {
		/* We need to stall this CPU */
		if (!spin_counter) {
			/* This CPU gets to do the counting */
			spin_counter = pause_on_oops;
			do {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(MSEC_PER_SEC);
				spin_lock(&pause_on_oops_lock);
			} while (--spin_counter);
			pause_on_oops_flag = 0;
		} else {
			/* This CPU waits for a different one */
			while (spin_counter) {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(1);
				spin_lock(&pause_on_oops_lock);
			}
		}
	}
	spin_unlock_irqrestore(&pause_on_oops_lock, flags);
}

/*
 * Return true if the calling CPU is allowed to print oops-related info.
 * This is a bit racy..
 */
int oops_may_print(void)
{
	return pause_on_oops_flag == 0;
}

/*
 * Called when the architecture enters its oops handler, before it prints
 * anything.  If this is the first CPU to oops, and it's oopsing the first
 * time then let it proceed.
 *
 * This is all enabled by the pause_on_oops kernel boot option.  We do all
 * this to ensure that oopses don't scroll off the screen.  It has the
 * side-effect of preventing later-oopsing CPUs from mucking up the display,
 * too.
 *
 * It turns out that the CPU which is allowed to print ends up pausing for
 * the right duration, whereas all the other CPUs pause for twice as long:
 * once in oops_enter(), once in oops_exit().
 */
void oops_enter(void)
{
	tracing_off();
	/* can't trust the integrity of the kernel anymore: */
	debug_locks_off();
	do_oops_enter_exit();
}

/*
 * 64-bit random ID for oopses:
 */
static u64 oops_id;

static int init_oops_id(void)
{
	if (!oops_id)
		get_random_bytes(&oops_id, sizeof(oops_id));
	else
		oops_id++;

	return 0;
}
late_initcall(init_oops_id);

void print_oops_end_marker(void)
{
	init_oops_id();
	pr_warn("---[ end trace %016llx ]---\n", (unsigned long long)oops_id);
}

/*
 * Called when the architecture exits its oops handler, after printing
 * everything.
 */
void oops_exit(void)
{
	do_oops_enter_exit();
	print_oops_end_marker();
	kmsg_dump(KMSG_DUMP_OOPS);
}

struct warn_args {
	const char *fmt;
	va_list args;
};

void __warn(const char *file, int line, void *caller, unsigned taint,
	    struct pt_regs *regs, struct warn_args *args)
{
	disable_trace_on_warning();

	pr_warn("------------[ cut here ]------------\n");

	if (file)
		pr_warn("WARNING: CPU: %d PID: %d at %s:%d %pS\n",
			raw_smp_processor_id(), current->pid, file, line,
			caller);
	else
		pr_warn("WARNING: CPU: %d PID: %d at %pS\n",
			raw_smp_processor_id(), current->pid, caller);

	if (args)
		vprintk(args->fmt, args->args);

	check_panic_on_warn("kernel");

	print_modules();

	dump_stack();

	print_oops_end_marker();

	/* Just a warning, don't kill lockdep. */
	add_taint(taint, LOCKDEP_STILL_OK);
}

#ifdef WANT_WARN_ON_SLOWPATH
void warn_slowpath_fmt(const char *file, int line, const char *fmt, ...)
{
	struct warn_args args;

	args.fmt = fmt;
	va_start(args.args, fmt);
	__warn(file, line, __builtin_return_address(0), TAINT_WARN, NULL,
	       &args);
	va_end(args.args);
}
EXPORT_SYMBOL(warn_slowpath_fmt);

void warn_slowpath_fmt_taint(const char *file, int line,
			     unsigned taint, const char *fmt, ...)
{
	struct warn_args args;

	args.fmt = fmt;
	va_start(args.args, fmt);
	__warn(file, line, __builtin_return_address(0), taint, NULL, &args);
	va_end(args.args);
}
EXPORT_SYMBOL(warn_slowpath_fmt_taint);

void warn_slowpath_null(const char *file, int line)
{
	__warn(file, line, __builtin_return_address(0), TAINT_WARN, NULL, NULL);
}
EXPORT_SYMBOL(warn_slowpath_null);
#endif

#ifdef CONFIG_CC_STACKPROTECTOR

/*
 * Called when gcc's -fstack-protector feature is used, and
 * gcc detects corruption of the on-stack canary value
 */
__visible void __stack_chk_fail(void)
{
	panic("stack-protector: Kernel stack is corrupted in: %pB\n",
		__builtin_return_address(0));
}
EXPORT_SYMBOL(__stack_chk_fail);

#endif

#ifdef CONFIG_ARCH_HAS_REFCOUNT
void refcount_error_report(struct pt_regs *regs, const char *err)
{
	WARN_RATELIMIT(1, "refcount_t %s at %pB in %s[%d], uid/euid: %u/%u\n",
		err, (void *)instruction_pointer(regs),
		current->comm, task_pid_nr(current),
		from_kuid_munged(&init_user_ns, current_uid()),
		from_kuid_munged(&init_user_ns, current_euid()));
}
#endif

core_param(panic, panic_timeout, int, 0644);
core_param(pause_on_oops, pause_on_oops, int, 0644);
core_param(panic_on_warn, panic_on_warn, int, 0644);
core_param(crash_kexec_post_notifiers, crash_kexec_post_notifiers, bool, 0644);

static int __init oops_setup(char *s)
{
	if (!s)
		return -EINVAL;
	if (!strcmp(s, "panic"))
		panic_on_oops = 1;
	return 0;
}
early_param("oops", oops_setup);
