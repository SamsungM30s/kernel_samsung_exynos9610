/*
 * Based on arch/arm/include/asm/processor.h
 *
 * Copyright (C) 1995-1999 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_PROCESSOR_H
#define __ASM_PROCESSOR_H

#define TASK_SIZE_64		(UL(1) << VA_BITS)

#define KERNEL_DS	UL(-1)
#define USER_DS		(TASK_SIZE_64 - 1)

#ifndef __ASSEMBLY__

/*
 * Default implementation of macro that returns current
 * instruction pointer ("program counter").
 */
#define current_text_addr() ({ __label__ _l; _l: &&_l;})

#ifdef __KERNEL__

#include <linux/string.h>

#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/fpsimd.h>
#include <asm/hw_breakpoint.h>
#include <asm/lse.h>
#include <asm/pgtable-hwdef.h>
#include <asm/ptrace.h>
#include <asm/types.h>

/*
 * TASK_SIZE - the maximum size of a user space task.
 * TASK_UNMAPPED_BASE - the lower boundary of the mmap VM area.
 */
#ifdef CONFIG_COMPAT
#ifdef CONFIG_ARM64_64K_PAGES
/*
 * With CONFIG_ARM64_64K_PAGES enabled, the last page is occupied
 * by the compat vectors page.
 */
#define TASK_SIZE_32		UL(0x100000000)
#else
#define TASK_SIZE_32		(UL(0x100000000) - PAGE_SIZE)
#endif /* CONFIG_ARM64_64K_PAGES */
#define TASK_SIZE		(test_thread_flag(TIF_32BIT) ? \
				TASK_SIZE_32 : TASK_SIZE_64)
#define TASK_SIZE_OF(tsk)	(test_tsk_thread_flag(tsk, TIF_32BIT) ? \
				TASK_SIZE_32 : TASK_SIZE_64)
#else
#define TASK_SIZE		TASK_SIZE_64
#endif /* CONFIG_COMPAT */

#define TASK_UNMAPPED_BASE	(PAGE_ALIGN(TASK_SIZE / 4))

#define STACK_TOP_MAX		TASK_SIZE_64
#ifdef CONFIG_COMPAT
#define AARCH32_KUSER_HELPERS_BASE 0xffff0000
#define STACK_TOP		(test_thread_flag(TIF_32BIT) ? \
				AARCH32_KUSER_HELPERS_BASE : STACK_TOP_MAX)
#else
#define STACK_TOP		STACK_TOP_MAX
#endif /* CONFIG_COMPAT */

extern phys_addr_t arm64_dma_phys_limit;
#define ARCH_LOW_ADDRESS_LIMIT	(arm64_dma_phys_limit - 1)

struct debug_info {
#ifdef CONFIG_HAVE_HW_BREAKPOINT
	/* Have we suspended stepping by a debugger? */
	int			suspended_step;
	/* Allow breakpoints and watchpoints to be disabled for this thread. */
	int			bps_disabled;
	int			wps_disabled;
	/* Hardware breakpoints pinned to this task. */
	struct perf_event	*hbp_break[ARM_MAX_BRP];
	struct perf_event	*hbp_watch[ARM_MAX_WRP];
#endif
};

struct cpu_context {
	unsigned long x19;
	unsigned long x20;
	unsigned long x21;
	unsigned long x22;
	unsigned long x23;
	unsigned long x24;
	unsigned long x25;
	unsigned long x26;
	unsigned long x27;
	unsigned long x28;
	unsigned long fp;
	unsigned long sp;
	unsigned long pc;
};

struct thread_struct {
	struct cpu_context	cpu_context;	/* cpu context */
	unsigned long		tp_value;	/* TLS register */
#ifdef CONFIG_COMPAT
	unsigned long		tp2_value;
#endif
	struct fpsimd_state	fpsimd_state;
	struct fpsimd_kernel_state fpsimd_kernel_state;
	unsigned long		fault_address;	/* fault info */
	unsigned long		fault_code;	/* ESR_EL1 value */
	struct debug_info	debug;		/* debugging */
};

#ifdef CONFIG_COMPAT
#define task_user_tls(t)						\
({									\
	unsigned long *__tls;						\
	if (is_compat_thread(task_thread_info(t)))			\
		__tls = &(t)->thread.tp2_value;				\
	else								\
		__tls = &(t)->thread.tp_value;				\
	__tls;								\
 })
#else
#define task_user_tls(t)	(&(t)->thread.tp_value)
#endif

/* Sync TPIDR_EL0 back to thread_struct for current */
void tls_preserve_current_state(void);

#define INIT_THREAD  {	}

static inline void start_thread_common(struct pt_regs *regs, unsigned long pc)
{
	s32 previous_syscall = regs->syscallno;
	memset(regs, 0, sizeof(*regs));
	regs->syscallno = previous_syscall;
	regs->pc = pc;
}

static inline void set_ssbs_bit(struct pt_regs *regs)
{
	regs->pstate |= PSR_SSBS_BIT;
}

static inline void set_compat_ssbs_bit(struct pt_regs *regs)
{
	regs->pstate |= PSR_AA32_SSBS_BIT;
}

static inline void start_thread(struct pt_regs *regs, unsigned long pc,
				unsigned long sp)
{
	start_thread_common(regs, pc);
	regs->pstate = PSR_MODE_EL0t;

	if (arm64_get_ssbd_state() != ARM64_SSBD_FORCE_ENABLE)
		set_ssbs_bit(regs);

	regs->sp = sp;
}

#ifdef CONFIG_COMPAT
static inline void compat_start_thread(struct pt_regs *regs, unsigned long pc,
				       unsigned long sp)
{
	start_thread_common(regs, pc);
	regs->pstate = COMPAT_PSR_MODE_USR;
	if (pc & 1)
		regs->pstate |= COMPAT_PSR_T_BIT;

#ifdef __AARCH64EB__
	regs->pstate |= COMPAT_PSR_E_BIT;
#endif

	if (arm64_get_ssbd_state() != ARM64_SSBD_FORCE_ENABLE)
		set_compat_ssbs_bit(regs);

	regs->compat_sp = sp;
}
#endif

/* Forward declaration, a strange C thing */
struct task_struct;

/* Free all resources held by a thread. */
extern void release_thread(struct task_struct *);

unsigned long get_wchan(struct task_struct *p);

static inline void cpu_relax(void)
{
	asm volatile("yield" ::: "memory");
}

/* Thread switching */
extern struct task_struct *cpu_switch_to(struct task_struct *prev,
					 struct task_struct *next);

#define task_pt_regs(p) \
	((struct pt_regs *)(THREAD_SIZE + task_stack_page(p)) - 1)

#define KSTK_EIP(tsk)	((unsigned long)task_pt_regs(tsk)->pc)
#define KSTK_ESP(tsk)	user_stack_pointer(task_pt_regs(tsk))

/*
 * Prefetching support
 */
#define ARCH_HAS_PREFETCH
static inline void prefetch(const void *ptr)
{
	asm volatile("prfm pldl1keep, %a0\n" : : "p" (ptr));
}

#define ARCH_HAS_PREFETCHW
static inline void prefetchw(const void *ptr)
{
	asm volatile("prfm pstl1keep, %a0\n" : : "p" (ptr));
}

#define ARCH_HAS_SPINLOCK_PREFETCH
static inline void spin_lock_prefetch(const void *ptr)
{
	asm volatile(ARM64_LSE_ATOMIC_INSN(
		     "prfm pstl1strm, %a0",
		     "nop") : : "p" (ptr));
}

#define HAVE_ARCH_PICK_MMAP_LAYOUT

#endif

void cpu_enable_pan(const struct arm64_cpu_capabilities *__unused);
void cpu_enable_cache_maint_trap(const struct arm64_cpu_capabilities *__unused);

#endif /* __ASSEMBLY__ */
#endif /* __ASM_PROCESSOR_H */
