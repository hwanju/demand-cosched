/*
 * Split spinlock implementation out into its own file, so it can be
 * compiled in a FTRACE-compatible way.
 */
#include <linux/kernel_stat.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/log2.h>
#include <linux/gfp.h>

#include <asm/paravirt.h>

#include <xen/interface/xen.h>
#include <xen/events.h>

#include "xen-ops.h"
#include "debugfs.h"

#ifdef CONFIG_XEN_DEBUG_FS
static struct xen_spinlock_stats
{
	u32 taken_slow;
	u32 taken_slow_pickup;
	u32 taken_slow_spurious;

	u32 released_slow;
	u32 released_slow_kicked;

#define HISTO_BUCKETS	30
	u32 histo_spin_blocked[HISTO_BUCKETS+1];

	u64 time_blocked;
} spinlock_stats;

static u8 zero_stats;

static inline void check_zero(void)
{
	if (unlikely(zero_stats)) {
		memset(&spinlock_stats, 0, sizeof(spinlock_stats));
		zero_stats = 0;
	}
}

#define ADD_STATS(elem, val)			\
	do { check_zero(); spinlock_stats.elem += (val); } while(0)

static inline u64 spin_time_start(void)
{
	return xen_clocksource_read();
}

static void __spin_time_accum(u64 delta, u32 *array)
{
	unsigned index = ilog2(delta);

	check_zero();

	if (index < HISTO_BUCKETS)
		array[index]++;
	else
		array[HISTO_BUCKETS]++;
}

static inline void spin_time_accum_blocked(u64 start)
{
	u32 delta = xen_clocksource_read() - start;

	__spin_time_accum(delta, spinlock_stats.histo_spin_blocked);
	spinlock_stats.time_blocked += delta;
}
#else  /* !CONFIG_XEN_DEBUG_FS */
#define TIMEOUT			(1 << 10)
#define ADD_STATS(elem, val)	do { (void)(val); } while(0)

static inline u64 spin_time_start(void)
{
	return 0;
}

static inline void spin_time_accum_blocked(u64 start)
{
}
#endif  /* CONFIG_XEN_DEBUG_FS */

struct xen_lock_waiting {
	struct arch_spinlock *lock;
	__ticket_t want;
};

static DEFINE_PER_CPU(int, lock_kicker_irq) = -1;
static DEFINE_PER_CPU(struct xen_lock_waiting, lock_waiting);
static cpumask_t waiting_cpus;

static void xen_lock_spinning(struct arch_spinlock *lock, __ticket_t want)
{
	int irq = __this_cpu_read(lock_kicker_irq);
	struct xen_lock_waiting *w = &__get_cpu_var(lock_waiting);
	int cpu = smp_processor_id();
	u64 start;
	unsigned long flags;

	/* If kicker interrupts not initialized yet, just spin */
	if (irq == -1)
		return;

	start = spin_time_start();

	/*
	 * Make sure an interrupt handler can't upset things in a
	 * partially setup state.
	 */
	local_irq_save(flags);

	/*
	 * We don't really care if we're overwriting some other
	 * (lock,want) pair, as that would mean that we're currently
	 * in an interrupt context, and the outer context had
	 * interrupts enabled.  That has already kicked the VCPU out
	 * of xen_poll_irq(), so it will just return spuriously and
	 * retry with newly setup (lock,want).
	 *
	 * The ordering protocol on this is that the "lock" pointer
	 * may only be set non-NULL if the "want" ticket is correct.
	 * If we're updating "want", we must first clear "lock".
	 */
	w->lock = NULL;
	smp_wmb();
	w->want = want;
	smp_wmb();
	w->lock = lock;

	/* This uses set_bit, which atomic and therefore a barrier */
	cpumask_set_cpu(cpu, &waiting_cpus);

	ADD_STATS(taken_slow, 1);

	/* clear pending */
	xen_clear_irq_pending(irq);

	/* Only check lock once pending cleared */
	barrier();

	/*
	 * Mark entry to slowpath before doing the pickup test to make
	 * sure we don't deadlock with an unlocker.
	 */
	__ticket_enter_slowpath(lock);

	/*
	 * check again make sure it didn't become free while
	 * we weren't looking 
	 */
	if (ACCESS_ONCE(lock->tickets.head) == want) {
		ADD_STATS(taken_slow_pickup, 1);
		goto out;
	}

	/* Allow interrupts while blocked */
	local_irq_restore(flags);

	/*
	 * If an interrupt happens here, it will leave the wakeup irq
	 * pending, which will cause xen_poll_irq() to return
	 * immediately.
	 */

	/* Block until irq becomes pending (or perhaps a spurious wakeup) */
	xen_poll_irq(irq);
	ADD_STATS(taken_slow_spurious, !xen_test_irq_pending(irq));

	local_irq_save(flags);

	kstat_incr_irqs_this_cpu(irq, irq_to_desc(irq));

out:
	cpumask_clear_cpu(cpu, &waiting_cpus);
	w->lock = NULL;

	local_irq_restore(flags);

	spin_time_accum_blocked(start);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_lock_spinning);

static void xen_unlock_kick(struct arch_spinlock *lock, __ticket_t next)
{
	int cpu;

	ADD_STATS(released_slow, 1);

	for_each_cpu(cpu, &waiting_cpus) {
		const struct xen_lock_waiting *w = &per_cpu(lock_waiting, cpu);

		/* Make sure we read lock before want */
		if (ACCESS_ONCE(w->lock) == lock &&
		    ACCESS_ONCE(w->want) == next) {
			ADD_STATS(released_slow_kicked, 1);
			xen_send_IPI_one(cpu, XEN_SPIN_UNLOCK_VECTOR);
			break;
		}
	}
}

static irqreturn_t dummy_handler(int irq, void *dev_id)
{
	BUG();
	return IRQ_HANDLED;
}

void __cpuinit xen_init_lock_cpu(int cpu)
{
	int irq;
	const char *name;

	name = kasprintf(GFP_KERNEL, "spinlock%d", cpu);
	irq = bind_ipi_to_irqhandler(XEN_SPIN_UNLOCK_VECTOR,
				     cpu,
				     dummy_handler,
				     IRQF_DISABLED|IRQF_PERCPU|IRQF_NOBALANCING,
				     name,
				     NULL);

	if (irq >= 0) {
		disable_irq(irq); /* make sure it's never delivered */
		per_cpu(lock_kicker_irq, cpu) = irq;
	}

	printk("cpu %d spinlock event irq %d\n", cpu, irq);
}

void xen_uninit_lock_cpu(int cpu)
{
	unbind_from_irqhandler(per_cpu(lock_kicker_irq, cpu), NULL);
}

static bool xen_pvspin __initdata = true;

void __init xen_init_spinlocks(void)
{
	if (!xen_pvspin) {
		printk(KERN_DEBUG "xen: PV spinlocks disabled\n");
		return;
	}

	jump_label_inc(&paravirt_ticketlocks_enabled);

	pv_lock_ops.lock_spinning = PV_CALLEE_SAVE(xen_lock_spinning);
	pv_lock_ops.unlock_kick = xen_unlock_kick;
}

static __init int xen_parse_nopvspin(char *arg)
{
	xen_pvspin = false;
	return 0;
}
early_param("xen_nopvspin", xen_parse_nopvspin);

#ifdef CONFIG_XEN_DEBUG_FS

static struct dentry *d_spin_debug;

static int __init xen_spinlock_debugfs(void)
{
	struct dentry *d_xen = xen_init_debugfs();

	if (d_xen == NULL)
		return -ENOMEM;

	d_spin_debug = debugfs_create_dir("spinlocks", d_xen);

	debugfs_create_u8("zero_stats", 0644, d_spin_debug, &zero_stats);

	debugfs_create_u32("taken_slow", 0444, d_spin_debug,
			   &spinlock_stats.taken_slow);
	debugfs_create_u32("taken_slow_pickup", 0444, d_spin_debug,
			   &spinlock_stats.taken_slow_pickup);
	debugfs_create_u32("taken_slow_spurious", 0444, d_spin_debug,
			   &spinlock_stats.taken_slow_spurious);

	debugfs_create_u32("released_slow", 0444, d_spin_debug,
			   &spinlock_stats.released_slow);
	debugfs_create_u32("released_slow_kicked", 0444, d_spin_debug,
			   &spinlock_stats.released_slow_kicked);

	debugfs_create_u64("time_blocked", 0444, d_spin_debug,
			   &spinlock_stats.time_blocked);

	xen_debugfs_create_u32_array("histo_blocked", 0444, d_spin_debug,
				     spinlock_stats.histo_spin_blocked, HISTO_BUCKETS + 1);

	return 0;
}
fs_initcall(xen_spinlock_debugfs);

#endif	/* CONFIG_XEN_DEBUG_FS */
