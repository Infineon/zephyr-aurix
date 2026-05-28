/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief TriCore specific kernel interface header
 *
 * This header contains the TriCore specific kernel interface.  It is
 * included by the kernel interface architecture-abstraction header
 * (include/zephyr/arch/cpu.h).
 */

#ifndef ZEPHYR_INCLUDE_ARCH_TRICORE_ARCH_H_
#define ZEPHYR_INCLUDE_ARCH_TRICORE_ARCH_H_

#include <zephyr/arch/tricore/thread.h>
#include <zephyr/arch/tricore/exception.h>
#include <zephyr/arch/tricore/error.h>
#include <zephyr/arch/tricore/irq.h>
#include <zephyr/arch/tricore/cr.h>

#include <zephyr/arch/common/sys_io.h>
#include <zephyr/arch/common/sys_bitops.h>
#include <zephyr/arch/common/ffs.h>

/** @cond INTERNAL_HIDDEN */
/* Stack align and MPU data protection region align are both 8*/
#define ARCH_STACK_PTR_ALIGN 8
/** @endcond */

#ifndef _ASMLANGUAGE
#include <zephyr/sys/util.h>

#ifdef CONFIG_TRICORE_MPU
#include <zephyr/arch/tricore/mpu.h>
#endif

#if defined(CONFIG_USERSPACE)
#include <zephyr/arch/tricore/syscall.h>
#endif

#if defined(CONFIG_MPU_STACK_GUARD)
/* Reserve the MPU stack-guard region inside the stack object so that
 * thread.stack_info.start sits above the guard. Otherwise kernel paths
 * that scan the full stack range (z_stack_space_get, sentinel checks)
 * touch the carved-out guard and take a Class 1 TIN 2 MPU read trap.
 */
#define ARCH_THREAD_STACK_RESERVED ((size_t)Z_TRICORE_STACK_GUARD_SIZE)
#define ARCH_KERNEL_STACK_RESERVED ((size_t)Z_TRICORE_STACK_GUARD_SIZE)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @cond INTERNAL_HIDDEN */

static ALWAYS_INLINE unsigned int arch_irq_lock(void)
{
	unsigned int key = cr_read(TRICORE_ICR);

	__asm__ volatile("disable");

	return key;
}

static ALWAYS_INLINE bool arch_irq_unlocked(unsigned int key)
{
	return !!(key & BIT(15));
}

static ALWAYS_INLINE void arch_irq_unlock(unsigned int key)
{
	if (arch_irq_unlocked(key)) {
		__asm__ volatile("enable");
	}
}

static inline bool arch_cpu_irqs_are_enabled(void)
{
	return !!(cr_read(TRICORE_ICR) & BIT(15));
}

extern uint32_t sys_clock_cycle_get_32(void);

static inline uint32_t arch_k_cycle_get_32(void)
{
	return sys_clock_cycle_get_32();
}

extern uint64_t sys_clock_cycle_get_64(void);

static inline uint64_t arch_k_cycle_get_64(void)
{
	return sys_clock_cycle_get_64();
}

static ALWAYS_INLINE void arch_nop(void)
{
	__asm__ volatile("nop");
}

/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_TRICORE_ARCH_H_ */
