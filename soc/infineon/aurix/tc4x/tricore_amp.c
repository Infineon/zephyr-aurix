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

#define IFX_CPU_PROTSPRSE(n)		(IFX_CPUn_REG(n) + 0xE008U)
#define IFX_CPU_ACCENSPRCFG_WRA(n)	(IFX_CPUn_REG(n) + 0xE020U)
#define IFX_CPU_ACCENSPRCFG_WRB(n)	(IFX_CPUn_REG(n) + 0xE024U)

#define IFX_CPU_PROTDLMUSE(n)		(IFX_CPUn_REG(n) + 0xE048U)
#define IFX_CPU_ACCENDLMUCFG_WRA(n)	(IFX_CPUn_REG(n) + 0xE060U)
#define IFX_CPU_ACCENDLMUCFG_WRB(n)	(IFX_CPUn_REG(n) + 0xE064U)

#define IFX_CPU_PROTSFRSE(n)		(IFX_CPUn_REG(n) + 0xE088U)
#define IFX_CPU_ACCENSFRCFG_WRA(n)	(IFX_CPUn_REG(n) + 0xE0A0U)

#define IFX_CPU_PROTSTMSE(n)		(IFX_CPUn_REG(n) + 0xE0D8U)
#define IFX_CPU_ACCENSTMCFG_WRA(n)	(IFX_CPUn_REG(n) + 0xE0E0U)

#define IFX_CPU_PROT_RANGE(c)		((c) << 8)

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

static void tricore_enable_sfr_access(int n)
{
	sys_write32(0, IFX_CPU_PROTSFRSE(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENSFRCFG_WRA(n));

	sys_write32(IFX_CPU_PROT_RANGE(0x7), IFX_CPU_PROTSTMSE(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENSTMCFG_WRA(n));

	sys_write32(IFX_CPU_PROT_RANGE(0xF), IFX_CPU_PROTDLMUSE(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENDLMUCFG_WRA(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENDLMUCFG_WRB(n));

	sys_write32(IFX_CPU_PROT_RANGE(0xF), IFX_CPU_PROTSPRSE(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENSPRCFG_WRA(n));
	sys_write32(0xFFFFFFFFU, IFX_CPU_ACCENSPRCFG_WRB(n));
}

static int tricore_amp_start_cores(void)
{
	uint32_t mask = CONFIG_TRICORE_AMP_CPU_MASK;
	uint32_t hreg;

	if (CONFIG_TRICORE_CORE_ID != 0) {
		return 0;
	}

	tricore_enable_sfr_access(0);

	for (int i = 1; i < TC4X_MAX_CPUS; i++) {
		if (!(mask & BIT(i))) {
			continue;
		}

		tricore_enable_sfr_access(i);

		hreg = sys_read32(IFX_CPU_BOOTCON(i));
		if (hreg & IFX_CPU_HALT) {
			sys_write32(pflash_base[i], IFX_CPU_PC(i));
			sys_write32(hreg & ~IFX_CPU_HALT, IFX_CPU_BOOTCON(i));
		}
	}

	return 0;
}

SYS_INIT(tricore_amp_start_cores, PRE_KERNEL_2, 0);
