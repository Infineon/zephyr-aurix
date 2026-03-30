# Copyright 2024 Infineon Technologies AG
# SPDX-License-Identifier: Apache-2.0

zephyr_library_sources_ifdef(CONFIG_ETH_DWC_ETHER_QOS eth_qos.c)