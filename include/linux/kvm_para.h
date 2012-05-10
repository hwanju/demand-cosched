#ifndef __LINUX_KVM_PARA_H
#define __LINUX_KVM_PARA_H

/*
 * This header file provides a method for making a hypercall to the host
 * Architectures should define:
 * - kvm_hypercall0, kvm_hypercall1...
 * - kvm_arch_para_features
 * - kvm_para_available
 */

/* Return values for hypercalls */
#define KVM_ENOSYS		1000
#define KVM_EFAULT		EFAULT
#define KVM_E2BIG		E2BIG
#define KVM_EPERM		EPERM

#define KVM_HC_VAPIC_POLL_IRQ		1
#define KVM_HC_MMU_OP			2
#define KVM_HC_FEATURES			3
#define KVM_HC_PPC_MAP_MAGIC_PAGE	4
#define KVM_HC_KICK_CPU			5

/*
 * hypercalls use architecture specific
 */
#include <asm/kvm_para.h>

#ifdef __KERNEL__

static inline int kvm_para_has_feature(unsigned int feature)
{
	if (kvm_arch_para_features() & (1UL << feature))
		return 1;
	return 0;
}

#ifdef CONFIG_PARAVIRT_LOCK_HOLDER_GUEST
DECLARE_PER_CPU(struct kvm_lock_holder, lock_holder);
#define set_lock_holder(lock) \
	do { \
		__get_cpu_var(lock_holder).eip[__get_cpu_var(lock_holder).depth & KVM_LOCK_HOLDER_MASK]  = _RET_IP_; \
		__get_cpu_var(lock_holder).lock[__get_cpu_var(lock_holder).depth & KVM_LOCK_HOLDER_MASK] = lock; \
		__get_cpu_var(lock_holder).depth++;	\
	} while(0)
#define clear_lock_holder() \
	do { \
		__get_cpu_var(lock_holder).depth--;	\
		__get_cpu_var(lock_holder).eip[__get_cpu_var(lock_holder).depth & KVM_LOCK_HOLDER_MASK]  = 0; \
		__get_cpu_var(lock_holder).lock[__get_cpu_var(lock_holder).depth & KVM_LOCK_HOLDER_MASK] = 0; \
	} while(0)
#endif /* CONFIG_PARAVIRT_LOCK_HOLDER_GUEST */

#endif /* __KERNEL__ */
#endif /* __LINUX_KVM_PARA_H */

