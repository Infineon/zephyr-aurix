/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <offsets_short.h>
#include <zephyr/arch/cpu.h>

GTEXT(z_tricore_switch)
GTEXT(z_thread_mark_switched_in)
#if CONFIG_TRICORE_MPU
GTEXT(z_tricore_mpu_configure_thread)
#endif

/* void z_tricore_switch(k_thread_t *switch_to, k_thread_t *switch_from) */
SECTION_FUNC(TEXT, z_tricore_switch)
#if CONFIG_CPU_TC18
	/* Save old context */
	mfcr	%d14, TRICORE_PCXI
	mfcr	%d15, TRICORE_PPRS
	st.d	[%a5]+_thread_offset_to_saved_pcxi, %e14
	/* Load new context */
	ld.d 	%e14, [%a4]+_thread_offset_to_saved_pcxi
#else
	/* Save old context */
	mfcr	%d14, TRICORE_PCXI
	st.w	[%a5]+_thread_offset_to_saved_pcxi, %d14
	/* Load new context */
	ld.w 	%d14, [%a4]+_thread_offset_to_saved_pcxi
#endif

#if CONFIG_TRICORE_MPU
	/* %a4 = next thread; preserve across the call via %a15 which lives in
	 * the upper context and is auto-saved by the call insn.
	 */
	mov.aa	%a15, %a4
	call	z_tricore_mpu_configure_thread
	mov.aa	%a4, %a15
#endif

#if CONFIG_INSTRUMENT_THREAD_SWITCHING
	call	z_thread_mark_switched_in
#endif

	/* Activate new thread context */
#if CONFIG_CPU_TC18
	mtcr	TRICORE_PPRS, %d15
#endif
	mtcr	TRICORE_PCXI, %d14
	isync

	fret
