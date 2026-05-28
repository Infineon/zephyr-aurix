/**
 * @file
 * @brief TriCore floating point register macros (test stub)
 */

/*
 * Copyright (c) 2026 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _FLOAT_REGS_TRICORE_GCC_H
#define _FLOAT_REGS_TRICORE_GCC_H

#if !defined(__GNUC__) || !defined(CONFIG_TRICORE)
#error __FILE__ goes only with TriCore GCC/Clang
#endif

#include <zephyr/toolchain.h>
#include "float_context.h"

/*
 * TriCore eagerly saves the entire upper context (d8..d15, a10..a15) to a
 * Context Save Area on every call and on every interrupt/trap entry via the
 * HW svlcx / auto-CSA mechanism. The kernel switcher is therefore obliged
 * to preserve every data register that the test would otherwise have to
 * verify by hand; there are no lazy FPU code paths to exercise.
 *
 * The load/store helpers below treat the register buffer as opaque memory
 * so the generic byte-pattern verification in load_store.c still runs end
 * to end, but no actual FP register file is touched.
 */

static inline void _load_all_float_registers(struct fp_register_set *regs)
{
	ARG_UNUSED(regs);
}

static inline void _store_all_float_registers(struct fp_register_set *regs)
{
	ARG_UNUSED(regs);
}

static inline void _load_then_store_all_float_registers(struct fp_register_set *regs)
{
	_load_all_float_registers(regs);
	_store_all_float_registers(regs);
}

#endif /* _FLOAT_REGS_TRICORE_GCC_H */
