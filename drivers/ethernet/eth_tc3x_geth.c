/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_tc3x_geth

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(geth, CONFIG_ETHERNET_LOG_LEVEL);
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/devicetree/clocks.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/net/ethernet.h>

#include <soc.h>

#define GETH_CLC     0x2000
#define GETH_GPCTL   0x2008
#define GETH_KRST0   0x2014
#define GETH_SKEWCTL 0x2040
#define DMA_MODE     0x1000
#define DMA_MODE_SWR BIT(0)

struct eth_tc3xx_geth_config {
	mm_reg_t base;
	const struct pinctrl_dev_config *eth_pins;
	uint8_t interface;
	bool skip_init;
};

static int eth_tc3xx_geth_init(const struct device *dev)
{
	const struct eth_tc3xx_geth_config *cfg = dev->config;
	uint32_t gpctl;
	int ret;

	if (cfg->skip_init) {
		return 0;
	}

	/* Enable Module */
	if (!aurix_enable_clock(cfg->base + GETH_CLC, 1000)) {
		return -EIO;
	}
	/* Set gpctl and skew to zero due to errata */
	sys_write32(0, cfg->base + GETH_GPCTL);
	sys_write32(0, cfg->base + GETH_SKEWCTL);
	if (!aurix_kernel_reset(cfg->base + GETH_KRST0, 1000)) {
		return -EIO;
	}
	k_busy_wait(1);
	pinctrl_apply_state(cfg->eth_pins, PINCTRL_STATE_DEFAULT);
	sys_write32(sys_read32(cfg->base + GETH_GPCTL) | (cfg->interface << 22),
		    cfg->base + GETH_GPCTL);
	
	/* Set skew for RGMII interface */
	if (cfg->interface == 1) {
		sys_write32(9, cfg->base + GETH_SKEWCTL);
	}

	/* Software reset */
	sys_write32(DMA_MODE_SWR, cfg->base + DMA_MODE);

	return 0;
}

#define ETH_TC3XX_GETH_INTERFACE(id) DT_CAT(INTERFACE_TYPE_, id)
#define INTERFACE_TYPE_0             0
#define INTERFACE_TYPE_1             4
#define INTERFACE_TYPE_3             1

#define ETH_TC3XX_GETH_CHECK_CON_TYPE(n)                                                           \
	COND_CODE_1(DT_NODE_HAS_COMPAT(n, snps_dwc_ether_qos),                               \
		    (DT_ENUM_IDX(n, phy_connection_type)), ())
#define ETH_TC3XX_GETH_GET_CON_TYPE(n)                                                             \
	DT_FOREACH_CHILD(DT_DRV_INST(n), ETH_TC3XX_GETH_CHECK_CON_TYPE)

#define __ETH_TC3XX_GETH_ETH_PINS(n)                                                               \
	COND_CODE_1(DT_NODE_HAS_COMPAT(n, snps_dwc_ether_qos),                               \
		    (PINCTRL_DT_DEV_CONFIG_GET(n)), ())
#define ETH_TC3XX_GETH_ETH_PINS(n) DT_FOREACH_CHILD(DT_DRV_INST(n), __ETH_TC3XX_GETH_ETH_PINS)

#define __ETH_TC3XX_GETH_CHILD_PINS(n)                                                             \
	COND_CODE_1(DT_PINCTRL_HAS_IDX(n, 0), (PINCTRL_DT_DEFINE(n);), ())
#define ETH_TC3XX_GETH_CHILD_PINS(n) DT_FOREACH_CHILD(DT_DRV_INST(n), __ETH_TC3XX_GETH_CHILD_PINS)

#define ETH_TC3XX_GETH_INIT(n)                                                                     \
	ETH_TC3XX_GETH_CHILD_PINS(n)                                                               \
	static const struct eth_tc3xx_geth_config geth_config##n = {                               \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.skip_init = DT_INST_PROP(n, skip_init),                                           \
		.eth_pins = ETH_TC3XX_GETH_ETH_PINS(n),                                            \
		.interface = ETH_TC3XX_GETH_INTERFACE(ETH_TC3XX_GETH_GET_CON_TYPE(n))};            \
	DEVICE_DT_INST_DEFINE(n, eth_tc3xx_geth_init, NULL, NULL, &geth_config##n, POST_KERNEL,    \
			      CONFIG_ETH_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ETH_TC3XX_GETH_INIT)
