/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kernel_arch_data.h"
#include "zephyr/arch/tricore/thread.h"
#include "zephyr/sys/util_macro.h"
#include <zephyr/kernel.h>
#include <ksched.h>
#include <sys/cdefs.h>
#include <zephyr/tracing/tracing.h>

union z_tricore_context __kstackmem __aligned(4 * 16) z_tricore_csa[CONFIG_TRICORE_CSA_COUNT];

int arch_coprocessors_disable(struct k_thread *thread)
{
	return -ENOTSUP;
}

void z_tricore_reclaim_csa(struct k_thread *thread)
{
	uint32_t pcxi = thread->callee_saved.pcxi & 0xFFFFF;
	struct z_tricore_lower_context *csa;
	unsigned int key;

	if (pcxi == 0) {
		return;
	}

	csa = UINT_TO_POINTER(((pcxi & 0xF0000) << 12) | ((pcxi & 0xFFFF) << 6));
	key = irq_lock();
	__asm volatile("dsync" ::: "memory");

	while (csa->pcxi != 0) {
		csa->pcxi &= 0xFFFFF;
		csa = UINT_TO_POINTER(((csa->pcxi & 0xF0000) << 12) |
				      ((csa->pcxi & 0xFFFF) << 6));
	}

	csa->pcxi = cr_read(TRICORE_FCX);
	cr_write(TRICORE_FCX, pcxi);
	thread->callee_saved.pcxi = 0;

	irq_unlock(key);
}

unsigned int z_tricore_create_context(struct k_thread *thread, k_thread_entry_t entry, void *p1,
				      void *p2, void *p3, char *stack_ptr)
{
	uint32_t icr = cr_read(TRICORE_ICR);

	__asm volatile("disable" ::: "memory");
	uint32_t fcx = cr_read(TRICORE_FCX);
	z_tricore_lower_context_t *lower =
		(z_tricore_lower_context_t *)(((fcx & 0xF0000) << 12) | ((fcx & 0xFFFF) << 6));
	z_tricore_upper_context_t *upper =
		(z_tricore_upper_context_t *)(((lower->pcxi & 0xF0000) << 12) |
					      ((lower->pcxi & 0xFFFF) << 6));
	cr_write(TRICORE_FCX, upper->pcxi);
	if (icr & 0x8000) {
		__asm volatile("enable" ::: "memory");
	}

	lower->a4 = (uint32_t)entry;
	lower->a5 = (uint32_t)p1;
	lower->a6 = (uint32_t)p2;
	lower->a7 = (uint32_t)p3;
	lower->a11 = (uint32_t)z_thread_entry;
	lower->pcxi |= (1 << 21) | (1 << 20); 

	upper->a10 = (uint32_t)stack_ptr;
	upper->a11 = (uint32_t)z_thread_entry;
	upper->psw = (1 << 7); 
	upper->pcxi = 0;

#if defined(CONFIG_USERSPACE)
	if (thread->base.user_options & K_USER) {
		upper->psw |= (1 << 10); 
	} else {
#endif
		upper->psw |= (1 << 8) | (2 << 10); 
#if defined(CONFIG_USERSPACE)
	}
#endif

	return fcx;
}

uint32_t z_tricore_create_context_from_mem(struct k_thread *thread, uint32_t *mem)
{
	return z_tricore_create_context(thread, (k_thread_entry_t)mem[0], (void *)mem[1],
					(void *)mem[2], (void *)mem[3], (char *)mem[4]);
}

void arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack, char *stack_ptr,
		     k_thread_entry_t entry, void *p1, void *p2, void *p3)
{
#if !defined(CONFIG_SMP)
	thread->callee_saved.pcxi = z_tricore_create_context(thread, entry, p1, p2, p3, stack_ptr);
#else
	thread->callee_saved.pcxi = 0;
	thread->arch.arg_mem = stack;
	thread->arch.arg_mem[0] = (uint32_t)entry;
	thread->arch.arg_mem[1] = (uint32_t)p1;
	thread->arch.arg_mem[2] = (uint32_t)p2;
	thread->arch.arg_mem[3] = (uint32_t)p3;
	thread->arch.arg_mem[4] = (uint32_t)stack_ptr;
#endif

	thread->arch.prs =
#if defined(CONFIG_TRICORE_MPU)
		MAX(1, FIELD_GET(K_PROTECTION_SET_MASK, thread->base.user_options));
#else
		FIELD_GET(K_PROTECTION_SET_MASK, thread->base.user_options);
#endif

#if CONFIG_CPU_TC18
	thread->callee_saved.pprs = thread->arch.prs;
#endif

	thread->switch_handle = thread;
}

void z_impl_k_thread_abort(k_tid_t thread)
{
	SYS_PORT_TRACING_OBJ_FUNC_ENTER(k_thread, abort, thread);

	z_thread_abort(thread);

	if (thread != _current) {
		z_tricore_reclaim_csa(thread);
	}

	SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, abort, thread);
}

#ifdef CONFIG_USERSPACE

FUNC_NORETURN void arch_user_mode_enter(k_thread_entry_t user_entry, void *p1, void *p2, void *p3)
{
	ARG_UNUSED(user_entry);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	__ASSERT(false, "arch_user_mode_enter not implemented for TriCore");
	for (;;) {
		k_cpu_idle();
	}
	CODE_UNREACHABLE;
}

#endif 

#ifndef CONFIG_MULTITHREADING

FUNC_NORETURN void z_tricore_switch_to_main_no_multithreading(k_thread_entry_t main_entry, void *p1,
							    void *p2, void *p3)
{
}
#endif
