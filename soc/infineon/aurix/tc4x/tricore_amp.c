/*
 * Copyright (c) 2024 Infineon Technologies AG
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <soc.h>
#include <stdint.h>

#define IFX_CPUn_REG(n)			(0xF8800000U + 0x40000U * (n))
#define IFX_CPU_HALT			BIT(0)
#define IFX_CPU_BOOTCON(n)		(IFX_CPUn_REG(n) + 0x1FE60U)
#define IFX_CPU_PC(n)			(IFX_CPUn_REG(n) + 0x1FE08U)

#define TC4X_MAX_CPUS			6

#define PFLASH_ADDR(n) DT_REG_ADDR(DT_NODELABEL(flash##n))

static const uint32_t pflash_base[TC4X_MAX_CPUS] = {
	PFLASH_ADDR(0),
	PFLASH_ADDR(1),
	PFLASH_ADDR(2),
	PFLASH_ADDR(3),
	PFLASH_ADDR(4),
	PFLASH_ADDR(5),
};

/* Write each secondary's PC and clear BOOTCON.HALT to release it.
 * Don't touch peer-CPU PROT/ACCEN: that triggers a class-4 DAE on
 * cross-core SE writes. Reset defaults are already permissive.
 */
static int tricore_amp_start_cores(void)
{
	uint32_t mask = CONFIG_TRICORE_AMP_CPU_MASK;
	uint32_t hreg;

	if (CONFIG_TRICORE_CORE_ID != 0) {
		return 0;
	}

	for (int i = 1; i < TC4X_MAX_CPUS; i++) {
		if (!(mask & BIT(i))) {
			continue;
		}

		hreg = sys_read32(IFX_CPU_BOOTCON(i));
		if (hreg & IFX_CPU_HALT) {
			sys_write32(pflash_base[i], IFX_CPU_PC(i));
			sys_write32(0, IFX_CPU_BOOTCON(i));
		}
	}

	return 0;
}

SYS_INIT(tricore_amp_start_cores, PRE_KERNEL_2, 0);
