/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(eth_tc4x_leth, CONFIG_ETHERNET_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/devicetree/clocks.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/sys/util.h>

#include <soc.h>
#include <IfxLeth_reg.h>

#define DT_DRV_COMPAT infineon_tc4x_leth

struct eth_tc4x_leth_port {
	const struct pinctrl_dev_config *eth_pins;
	uint8_t interface;
	uint8_t enabled;
	uint32_t txq_en: 5;
	uint32_t rxc_en: 8;
	uint32_t fwd_en: 5;
	uint8_t fwd_port[5];
	uint8_t txq_src[5];
	uint8_t rxc_tgt[8];
};

struct eth_tc4x_leth_config {
	mm_reg_t base;
	const struct device *clkctrl;
	uint32_t clk;
	const struct eth_tc4x_leth_port ports[4];
	uint8_t num_ports;
	bool single_port;
	bool skip_init;
};

static int eth_tc4x_leth_reset(const struct device *dev)
{
	MODULE_LETH0.DMA.MODE.B.SWR = 1;
	/* Check if reset is complete */
	if (!WAIT_FOR(MODULE_LETH0.DMA.MODE.B.SWR == 0, 10000, k_busy_wait(50))) {
		return -EIO;
	}

	return 0;
}

static int eth_tc4x_leth_init(const struct device *dev)
{
	const struct eth_tc4x_leth_config *cfg = dev->config;
	int ret;

	if (cfg->skip_init) {
		return 0;
	}

	if (!device_is_ready(cfg->clkctrl)) {
		return -EIO;
	}

	/* Enable LETH clock*/
	ret = clock_control_on(cfg->clkctrl, (clock_control_subsys_t)&cfg->clk);
	if (ret) {
		return ret;
	}

	/* Enable Module */
	if (!aurix_enable_clock((uintptr_t)&MODULE_LETH0.CLC, 1000)) {
		return -EIO;
	}

	uint8_t i;
	uint8_t ports_enabled = 0;
	for (i = 0; i < cfg->num_ports; i++) {
		if (!cfg->ports[i].enabled) {
			continue;
		}
		ports_enabled += (1 < 1);

		MODULE_LETH0.PEN.U &= ~(1 << (i));
		if (!WAIT_FOR((MODULE_LETH0.PEN.U & (1 << (16 + i))) == 0, 100, k_busy_wait(10))) {
			return -EIO;
		}

		MODULE_LETH0.P[i].PORTCTRL0.B.EPR = cfg->ports[i].interface;
		MODULE_LETH0.P[i].PORTCTRL0.B.TC14EN = 1;
		if (cfg->ports[i].eth_pins) {
			pinctrl_apply_state(cfg->ports[i].eth_pins, PINCTRL_STATE_DEFAULT);
		}
	}

	for (i = 0; i < cfg->num_ports; i++) {
		if (!cfg->ports[i].enabled) {
			continue;
		}
	}

	if (!aurix_kernel_reset((uintptr_t)&MODULE_LETH0.RST.CTRLA, 1000)) {
		return -EIO;
	}

	/* Trigger latching of new config */
	MODULE_LETH0.DMA.MODE.B.SWR = 1;

	ret = eth_tc4x_leth_reset(dev);
	if (ret) {
		LOG_ERR("Failed to reset leth ports");
		return ret;
	}

	if (cfg->single_port) {
		uint8_t port = __builtin_ctz(ports_enabled);
		Ifx_LETH_BRIDGE_ETHBR_FWD_CTRL_REG ethbr = MODULE_LETH0.BRIDGE.ETHBR_FWD_CTRL_REG;
		ethbr.B.PORT_SEL = port;
		ethbr.B.Q_CH_MAPPING_EN = 0;
		ethbr.B.MAX_PKT_LENGTH = NET_ETH_MTU;
		MODULE_LETH0.BRIDGE.ETHBR_FWD_CTRL_REG = ethbr;
	} else {
		for (i = 0; i < cfg->num_ports; i++) {
			if (!cfg->ports[i].enabled) {
				continue;
			}
			Ifx_LETH_BRIDGE_PORT_CTRL_MAP_CTRL_REG ctrl_reg = {
				.B = {.FWD_EN = cfg->ports[i].fwd_en,
				      .RXC_EN = cfg->ports[i].rxc_en,
				      .TXQ_EN = cfg->ports[i].txq_en}};
			MODULE_LETH0.BRIDGE.PORT_CTRL_MAP[i].CTRL_REG = ctrl_reg;
			Ifx_LETH_BRIDGE_PORT_CTRL_MAP_FWD_PORT_MAP fwd_port_reg = {
				.B = {.TXQ0_FWD_PORT_NUM = cfg->ports[i].fwd_port[0],
				      .TXQ1_FWD_PORT_NUM = cfg->ports[i].fwd_port[1],
				      .TXQ2_FWD_PORT_NUM = cfg->ports[i].fwd_port[2],
				      .TXQ3_FWD_PORT_NUM = cfg->ports[i].fwd_port[3],
				      .TXQ4_FWD_PORT_NUM = cfg->ports[i].fwd_port[4]}};
			MODULE_LETH0.BRIDGE.PORT_CTRL_MAP[i].FWD_PORT_MAP = fwd_port_reg;
			Ifx_LETH_BRIDGE_PORT_CTRL_MAP_RXC_MAP rxc_map = {
				.B = {.RXC0_MAP = cfg->ports[i].rxc_tgt[0],
				      .RXC1_MAP = cfg->ports[i].rxc_tgt[1],
				      .RXC2_MAP = cfg->ports[i].rxc_tgt[2],
				      .RXC3_MAP = cfg->ports[i].rxc_tgt[3],
				      .RXC4_MAP = cfg->ports[i].rxc_tgt[4],
				      .RXC5_MAP = cfg->ports[i].rxc_tgt[5],
				      .RXC6_MAP = cfg->ports[i].rxc_tgt[6],
				      .RXC7_MAP = cfg->ports[i].rxc_tgt[7]}};
			MODULE_LETH0.BRIDGE.PORT_CTRL_MAP[i].RXC_MAP = rxc_map;
			Ifx_LETH_BRIDGE_PORT_CTRL_MAP_TXQ_MAP txq_map = {
				.B = {
					.TXQ0_MAP = cfg->ports[i].txq_src[0],
					.TXQ1_MAP = cfg->ports[i].txq_src[1],
					.TXQ2_MAP = cfg->ports[i].txq_src[2],
					.TXQ3_MAP = cfg->ports[i].txq_src[3],
					.TXQ4_MAP = cfg->ports[i].txq_src[4],
				}};
			MODULE_LETH0.BRIDGE.PORT_CTRL_MAP[i].TXQ_MAP = txq_map;
		}
		Ifx_LETH_BRIDGE_ETHBR_FWD_CTRL_REG ethbr = MODULE_LETH0.BRIDGE.ETHBR_FWD_CTRL_REG;
		ethbr.B.PORT_SEL = 0;
		ethbr.B.Q_CH_MAPPING_EN = 1;
		ethbr.B.MAX_PKT_LENGTH = NET_ETH_MTU;
		MODULE_LETH0.BRIDGE.ETHBR_FWD_CTRL_REG = ethbr;
	}

	return 0;
}

#define ETH_TC4X_LETH_INTERFACE(id) DT_CAT(INTERFACE_TYPE_, id)
#define INTERFACE_TYPE_0            0
#define INTERFACE_TYPE_1            1
#define INTERFACE_TYPE_4            3

#define __TXQ_NR(node_id)                                                                          \
	(1 << COND_CODE_1(DT_REG_HAS_IDX(node_id, 0), (DT_REG_ADDR(node_id)), (DT_NODE_CHILD_IDX(node_id))) )
#define __TXQ_CONFIG(node_id)             DT_CHILD(node_id, txq_config)
#define __TXQ_FOREACH(node_id, func, sep) DT_FOREACH_CHILD_SEP(__TXQ_CONFIG(node_id), func, sep)
#define __PORT_TXQ_EN(node_id)                                                                     \
	COND_CODE_1(DT_NODE_EXISTS(__TXQ_CONFIG(node_id)),                                   \
		    (__TXQ_FOREACH(node_id, __TXQ_NR, (, ))), (1))

#define __PORT_FWD_EN(node_id) 0

#define __RXC_TGT_CH(node_id, prop, idx) DT_PHA_BY_IDX(node_id, prop, idx, channel)
#define __PORT_RXC_TGT(node_id)                                                                    \
	{DT_FOREACH_PROP_ELEM_SEP(node_id,  snps_rx_channel_targets, __RXC_TGT_CH, (,))}

#define __TXQ_SRC_CH(node_id, prop, idx) DT_PHA_BY_IDX(node_id, prop, idx, channel)
#define __PORT_TXQ_SRC(node_id)                                                                    \
	{DT_FOREACH_PROP_ELEM_SEP(node_id,  snps_tx_queue_sources, __TXQ_SRC_CH, (,))}

#define __ETH_TC4X_LETH_PORT_ENABLED(node_id, mac)                                                 \
	(DT_NODE_HAS_STATUS(node_id, okay) && DT_REG_ADDR(mac) <= DT_REG_ADDR(node_id) &&          \
	 DT_REG_ADDR(node_id) < DT_REG_ADDR(mac) + DT_REG_SIZE(mac))
#define ETH_TC4X_LETH_PORT_ENABLED(node_id)                                                        \
	DT_FOREACH_CHILD_STATUS_OKAY_SEP_VARGS(DT_PARENT(node_id), __ETH_TC4X_LETH_PORT_ENABLED,   \
					       (||), node_id)
#define __ETH_TC4X_LETH_PORT_CONFIG(node_id)                                                       \
	IF_ENABLED(DT_NODE_HAS_COMPAT(node_id, snps_dwc_ether_qos),                          \
		   (ETH_TC4X_LETH_PORT_CONFIG(node_id)))
#define ETH_TC4X_LETH_PORT_CONFIG(node_id)                                                         \
	{                                                                                          \
		.eth_pins = PINCTRL_DT_DEV_CONFIG_GET(node_id),                                    \
		.interface =                                                                       \
			ETH_TC4X_LETH_INTERFACE(DT_ENUM_IDX_OR(node_id, phy_connection_type, 0)),  \
		.enabled =                                                                         \
			DT_NODE_HAS_STATUS(node_id, okay) || ETH_TC4X_LETH_PORT_ENABLED(node_id),  \
		.rxc_en = (1 << DT_PROP_OR(node_id, snps_rx_channels_to_use, 1)) - 1,              \
		.txq_en = __PORT_TXQ_EN(node_id),                                                  \
		.fwd_en = __PORT_FWD_EN(node_id),                                                  \
		.fwd_port = {0},                                                                   \
		.rxc_tgt = __PORT_RXC_TGT(node_id),                                                \
		.txq_src = __PORT_TXQ_SRC(node_id),                                                \
	},

#define ETH_TC4X_LETH_PORT_CONFIGS(n) {DT_INST_FOREACH_CHILD(n, __ETH_TC4X_LETH_PORT_CONFIG)}

#define __PORT_TARGET(node_id)                                                                     \
	IF_ENABLED(DT_NODE_HAS_COMPAT(node_id, snps_dwc_ether_qos),(\
 && !DT_NODE_HAS_PROP(node_id, snps_rx_channel_targets) && !DT_NODE_HAS_PROP(node_id, snps_tx_queue_sources)))
#define __ETH_TC4X_LETH_PINCTRL(node_id)                                                           \
	IF_ENABLED(DT_NODE_HAS_COMPAT(node_id, snps_dwc_ether_qos),                          \
		   (PINCTRL_DT_DEFINE(node_id);))
#define ETH_TC4X_LETH_INIT(n)                                                                      \
	DT_INST_FOREACH_CHILD(n, __ETH_TC4X_LETH_PINCTRL)                                          \
	static const struct eth_tc4x_leth_config leth_config##n = {                                \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, app)),                     \
		.clk = DT_INST_CLOCKS_CELL_BY_NAME(n, app, id),                                    \
		.ports = ETH_TC4X_LETH_PORT_CONFIGS(n),                                            \
		.skip_init = DT_INST_PROP(n, skip_init),                                           \
		.num_ports = (DT_INST_CHILD_NUM(n) - 1) / 3,                                       \
		.single_port = DT_NUM_INST_STATUS_OKAY(snps_dwc_ether_qos) ==                      \
			       1 DT_INST_FOREACH_CHILD(n, __PORT_TARGET),                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, eth_tc4x_leth_init, NULL, NULL, &leth_config##n, POST_KERNEL,     \
			      CONFIG_MDIO_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ETH_TC4X_LETH_INIT)
