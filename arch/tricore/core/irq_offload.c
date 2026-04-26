/*
 * Copyright (c) 2026 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/irq.h>
#include <zephyr/irq_offload.h>

static volatile irq_offload_routine_t offload_routine;
static volatile const void *offload_param;

void z_irq_do_offload(void)
{
	irq_offload_routine_t tmp;

	if (offload_routine == NULL) {
		return;
	}

	tmp = offload_routine;
	offload_routine = NULL;

	tmp((const void *)offload_param);
}

void arch_irq_offload(irq_offload_routine_t routine, const void *parameter)
{
	unsigned int key;

	key = irq_lock();
	offload_routine = routine;
	offload_param = parameter;
	z_irq_do_offload();
	irq_unlock(key);
}

void arch_irq_offload_init(void)
{
}
