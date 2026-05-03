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

#define VMT1_BASE			0xF0420000U
#define VMT1_CLC			(VMT1_BASE + 0x00U)
#define VMT1_MEMTEST			(VMT1_BASE + 0x80U)
#define VMT1_MC_BASE			(VMT1_BASE + 0x1000U)

#define VMT1_MC_STRIDE			0x40U
#define VMT1_MC_MCONTROL(i)		(VMT1_MC_BASE + (i) * VMT1_MC_STRIDE + 0x0CU)
#define VMT1_MC_MSTATUS(i)		(VMT1_MC_BASE + (i) * VMT1_MC_STRIDE + 0x10U)

#define VMT_MCONTROL_START_BIT		BIT(0)
#define VMT_MCONTROL_SRAM_CLR_BIT	BIT(15)
#define VMT_MSTATUS_DONE_BIT		BIT(0)

static int vmt_clock_enable(void)
{
	uint32_t clc = sys_read32(VMT1_CLC);

	if ((clc & 0x2U) == 0U) {
		return 0;
	}

	sys_write32(clc & ~BIT(0), VMT1_CLC);
	for (int spin = 0; spin < 1000; spin++) {
		if ((sys_read32(VMT1_CLC) & 0x2U) == 0U) {
			return 0;
		}
	}
	return -1;
}

static void vmt_scrub_channel(unsigned int ch)
{
	sys_write32(VMT_MCONTROL_SRAM_CLR_BIT | VMT_MCONTROL_START_BIT,
		    VMT1_MC_MCONTROL(ch));
	sys_write32(VMT_MCONTROL_SRAM_CLR_BIT, VMT1_MC_MCONTROL(ch));
	for (int spin = 0; spin < 1000000; spin++) {
		if (sys_read32(VMT1_MC_MSTATUS(ch)) & VMT_MSTATUS_DONE_BIT) {
			return;
		}
	}
}

static void tricore_amp_scrub_dlmu(uint32_t mask)
{
	if (vmt_clock_enable() != 0) {
		return;
	}

	sys_write32(0xFU, VMT1_MEMTEST);

	for (unsigned int i = 0; i < TC4X_MAX_CPUS; i++) {
		if ((i == 0) || (mask & BIT(i))) {
			vmt_scrub_channel(i);
		}
	}

	sys_write32(0x0U, VMT1_MEMTEST);
}

static int tricore_amp_start_cores(void)
{
	uint32_t mask = CONFIG_TRICORE_AMP_CPU_MASK;

	if (CONFIG_TRICORE_CORE_ID != 0) {
		return 0;
	}

	tricore_amp_scrub_dlmu(mask);

	for (int i = 1; i < TC4X_MAX_CPUS; i++) {
		if (!(mask & BIT(i))) {
			continue;
		}

		aurix_start_core(i, pflash_base[i]);
	}

	return 0;
}

SYS_INIT(tricore_amp_start_cores, PRE_KERNEL_2, 0);
