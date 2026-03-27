# Copyright (c) 2024 Infineon Technologies AG
# SPDX-License-Identifier: Apache-2.0
zephyr_sources(soc.c cpu_idle.c)
zephyr_library_sources_ifdef(CONFIG_TRICORE_AMP tricore_amp.c)
zephyr_include_directories(.)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/tricore/linker.ld CACHE INTERNAL "")
