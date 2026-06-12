/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zephyr/toolchain.h"
#define DT_DRV_COMPAT infineon_tc4x_geth

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(eth_tc4x_geth, CONFIG_ETHERNET_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/devicetree/clocks.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/sys/util.h>

#include <soc.h>
#include <IfxGeth_reg.h>
#include <IfxHsphy_reg.h>

#if CONFIG_SOC_TC4DX
struct eth_tc4x_geth_port {
	const struct pinctrl_dev_config *eth_pins;
	uint8_t interface;
	uint8_t txq_en;
	uint8_t rxc_en;
	uint8_t fwd_en;
	uint8_t txq_src[8];
	uint8_t rxc_tgt[8];
	bool enabled;
};
#endif

struct eth_tc4x_geth_config {
	mm_reg_t base;
	const struct device *clkctrl;
	uint32_t clk;
#if CONFIG_SOC_TC4DX
	const struct eth_tc4x_geth_port ports[2];
#else
	const struct pinctrl_dev_config *eth_pins;
	uint8_t interface;
#endif
};

#if CONFIG_SOC_TC4DX
static void eth_tc4x_geth_configure_bridge(const struct eth_tc4x_geth_config *cfg)
{
	Ifx_GETH_BRIDGE_FORWARD_CONTROL fwd_ctrl = MODULE_GETH0.BRIDGE.FORWARD_CONTROL;
	uint8_t i;
	uint8_t dma_port_sel = 0;

	if ((cfg->ports[0].enabled && cfg->ports[1].enabled) == 0) {
		fwd_ctrl.B.PORT_SEL = cfg->ports[1].enabled ? 1 : 0;
		fwd_ctrl.B.Q_CH_MAPPING_EN = 0;
		fwd_ctrl.B.MAX_PKT_LENGTH = NET_ETH_MTU;
		MODULE_GETH0.BRIDGE.FORWARD_CONTROL = fwd_ctrl;
		return;
	}

	for (i = 0; i < 2; i++) {
		Ifx_GETH_BRIDGE_PORT_0_CONTROL ctrl = {
			.B =
				{
					.FWD_EN = cfg->ports[i].fwd_en,
					.RXC_EN = cfg->ports[i].rxc_en,
					.TXQ_EN = cfg->ports[i].txq_en,
				},
		};
		Ifx_GETH_BRIDGE_TXQ_MAP_PORT_0 txq_map = {
			.B =
				{
					.TXQ0 = cfg->ports[i].txq_src[0],
					.TXQ1 = cfg->ports[i].txq_src[1],
					.TXQ2 = cfg->ports[i].txq_src[2],
					.TXQ3 = cfg->ports[i].txq_src[3],
					.TXQ4 = cfg->ports[i].txq_src[4],
					.TXQ5 = cfg->ports[i].txq_src[5],
					.TXQ6 = cfg->ports[i].txq_src[6],
					.TXQ7 = cfg->ports[i].txq_src[7],
				},
		};
		Ifx_GETH_BRIDGE_RXC_MAP_PORT_0 rxc_map = {
			.B =
				{
					.RXC0 = cfg->ports[i].rxc_tgt[0],
					.RXC1 = cfg->ports[i].rxc_tgt[1],
					.RXC2 = cfg->ports[i].rxc_tgt[2],
					.RXC3 = cfg->ports[i].rxc_tgt[3],
					.RXC4 = cfg->ports[i].rxc_tgt[4],
					.RXC5 = cfg->ports[i].rxc_tgt[5],
					.RXC6 = cfg->ports[i].rxc_tgt[6],
					.RXC7 = cfg->ports[i].rxc_tgt[7],
				},
		};

		if (i == 0) {
			MODULE_GETH0.BRIDGE.PORT_0_CONTROL.U = ctrl.U;
			MODULE_GETH0.BRIDGE.TXQ_MAP_PORT_0.U = txq_map.U;
			MODULE_GETH0.BRIDGE.RXC_MAP_PORT_0.U = rxc_map.U;
		} else {
			MODULE_GETH0.BRIDGE.PORT_1_CONTROL.U = ctrl.U;
			MODULE_GETH0.BRIDGE.TXQ_MAP_PORT_1.U = txq_map.U;
			MODULE_GETH0.BRIDGE.RXC_MAP_PORT_1.U = rxc_map.U;
		}
	}

	uint8_t j;
	for (j = 0; j < 8; j++) {
		if ((cfg->ports[1].txq_en & !cfg->ports[1].fwd_en) & BIT(j)) {
			dma_port_sel |= BIT(cfg->ports[1].txq_src[j]);
		}
		if ((cfg->ports[1].rxc_en & !cfg->ports[0].fwd_en) & BIT(j)) {
			dma_port_sel |= BIT(cfg->ports[1].rxc_tgt[j]);
		}
	}
	MODULE_GETH0.BRIDGE.DMA_PORT_SELECTION.U = dma_port_sel;

	fwd_ctrl.B.PORT_SEL = 0;
	fwd_ctrl.B.Q_CH_MAPPING_EN = 1;
	fwd_ctrl.B.MAX_PKT_LENGTH = NET_ETH_MTU;
	MODULE_GETH0.BRIDGE.FORWARD_CONTROL = fwd_ctrl;
}
#endif

static int eth_tc4x_geth_init(const struct device *dev)
{
	const struct eth_tc4x_geth_config *cfg = dev->config;
	int ret;
	__maybe_unused uint32_t mac_enable = 0;

	if (!device_is_ready(cfg->clkctrl)) {
		return -EIO;
	}

	/* Enable GETH clock */
	ret = clock_control_on(cfg->clkctrl, (clock_control_subsys_t)&cfg->clk);
	if (ret) {
		return ret;
	}

	/* Enable modules */
	if (!aurix_enable_clock((uintptr_t)&MODULE_GETH0.CLC, 1000)) {
		return -EIO;
	}

	if (!aurix_enable_clock((uintptr_t)&MODULE_HSPHY.CLC, 1000)) {
		return -EIO;
	}

#if CONFIG_SOC_TC4DX
	for (uint8_t i = 0; i < 2; i++) {
		if (!cfg->ports[i].enabled) {
			continue;
		}

		mac_enable |= BIT(i);

		MODULE_HSPHY.ETH[i].B.EPR = cfg->ports[i].interface;
		if (cfg->ports[i].eth_pins) {
			pinctrl_apply_state(cfg->ports[i].eth_pins, PINCTRL_STATE_DEFAULT);
		}
	}
#else
	MODULE_HSPHY.ETH[1].B.EPR = cfg->interface;
	if (cfg->eth_pins) {
		pinctrl_apply_state(cfg->eth_pins, PINCTRL_STATE_DEFAULT);
	}
	mac_enable |= 2;
#endif

	if (!aurix_kernel_reset((uintptr_t)&MODULE_GETH0.RST.CTRLA, 1000)) {
		return -EIO;
	}

	GETH0_MACEN.U = mac_enable;
	GETH0_DMA_MODE.B.SWR = 1;

#if CONFIG_SOC_TC4DX
	eth_tc4x_geth_configure_bridge(cfg);
#endif

	return 0;
}

#define ETH_TC4X_GETH_MAP_INTERFACE(id) DT_CAT(INTERFACE_TYPE_, id)
#define INTERFACE_TYPE_0                0
#define INTERFACE_TYPE_1                2
#define INTERFACE_TYPE_3                1
#define INTERFACE_TYPE_5                4
#define INTERFACE_TYPE_6                4

#define ETH_TC4X_GETH_INIT_PRIORITY                                                                \
	COND_CODE_1(IS_ENABLED(CONFIG_MDIO), (CONFIG_MDIO_INIT_PRIORITY),                         \
		    (COND_CODE_1(IS_ENABLED(CONFIG_PTP_CLOCK),                                      \
				 (CONFIG_PTP_CLOCK_INIT_PRIORITY), (CONFIG_ETH_INIT_PRIORITY))))
#define ETH_TC4X_GETH_PINCTRL_DEFINE(node_id)                                                      \
	IF_ENABLED(DT_NODE_HAS_COMPAT(node_id, snps_dwc_ether_xgmac),                              \
		   (COND_CODE_1(DT_PINCTRL_HAS_IDX(node_id, 0),                                       \
				(PINCTRL_DT_DEFINE(node_id);), ())))

#if CONFIG_SOC_TC4DX
#define __GETH_RXC_TGT_CH(node_id, prop, idx) DT_PHA_BY_IDX(node_id, prop, idx, channel)
#define __GETH_PORT_RXC_TGT(node_id)                                                               \
	{COND_CODE_1(DT_NODE_HAS_PROP(node_id, snps_rx_channel_targets),                          \
		     (DT_FOREACH_PROP_ELEM_SEP(node_id, snps_rx_channel_targets, __GETH_RXC_TGT_CH, \
					      (,))),                                           \
		     (0))}

#define __GETH_TXQ_SRC_CH(node_id, prop, idx) DT_PHA_BY_IDX(node_id, prop, idx, channel)
#define __GETH_PORT_TXQ_SRC(node_id)                                                               \
	{COND_CODE_1(DT_NODE_HAS_PROP(node_id, snps_tx_queue_sources),                           \
		     (DT_FOREACH_PROP_ELEM_SEP(node_id, snps_tx_queue_sources, __GETH_TXQ_SRC_CH,  \
					      (,))),                                           \
		     (0))}

#define ETH_TC4X_GETH_PORT_IDX(node_id)                                                            \
	((DT_REG_ADDR(node_id) - DT_REG_ADDR(DT_PARENT(node_id)) - 0x10000) / 0x2000)

#define __ETH_TC4X_GETH_PORT_CONFIG(node_id)                                                       \
	IF_ENABLED(DT_NODE_HAS_COMPAT(node_id, snps_dwc_ether_xgmac),                            \
		   ([ETH_TC4X_GETH_PORT_IDX(node_id)] = {                                           \
			   .eth_pins =                                                             \
				   COND_CODE_1(DT_PINCTRL_HAS_IDX(node_id, 0),                      \
					       (PINCTRL_DT_DEV_CONFIG_GET(node_id)), (NULL)),   \
			   .interface =                                                           \
				   ETH_TC4X_GETH_MAP_INTERFACE(                                \
					   DT_ENUM_IDX_OR(node_id, phy_connection_type, 0)), \
			   .txq_en = (1 << DT_PROP_OR(node_id, snps_tx_channel_to_use, 1)) - 1,   \
			   .rxc_en = (1 << DT_PROP_OR(node_id, snps_rx_channel_to_use, 1)) - 1,   \
			   .fwd_en = 0,                                                           \
			   .txq_src = __GETH_PORT_TXQ_SRC(node_id),                              \
			   .rxc_tgt = __GETH_PORT_RXC_TGT(node_id),                              \
			   .enabled = DT_NODE_HAS_STATUS(node_id, okay),                          \
		   },))

#define ETH_TC4X_GETH_PORT_CONFIGS(n) {DT_INST_FOREACH_CHILD(n, __ETH_TC4X_GETH_PORT_CONFIG)}

#define ETH_TC4X_GETH_INIT(n)                                                                      \
	DT_INST_FOREACH_CHILD(n, ETH_TC4X_GETH_PINCTRL_DEFINE)                                     \
	static const struct eth_tc4x_geth_config geth_config##n = {                                \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, app)),                     \
		.clk = DT_INST_CLOCKS_CELL_BY_NAME(n, app, id),                                    \
		.ports = ETH_TC4X_GETH_PORT_CONFIGS(n),                                            \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, eth_tc4x_geth_init, NULL, NULL, &geth_config##n, POST_KERNEL,     \
			      ETH_TC4X_GETH_INIT_PRIORITY, NULL);
#else
#define ETH_TC4X_GETH_INIT(n)                                                                      \
	DT_INST_FOREACH_CHILD(n, ETH_TC4X_GETH_PINCTRL_DEFINE)                                     \
	static const struct eth_tc4x_geth_config geth_config##n = {                                \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, app)),                     \
		.clk = DT_INST_CLOCKS_CELL_BY_NAME(n, app, id),                                    \
		.interface = ETH_TC4X_GETH_MAP_INTERFACE(                                          \
			DT_ENUM_IDX_OR(DT_NODELABEL(eth0), phy_connection_type, 0)),               \
		.eth_pins =  COND_CODE_1(DT_PINCTRL_HAS_IDX(DT_NODELABEL(eth0), 0),                      \
					       (PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(eth0))), (NULL))};                         \
	DEVICE_DT_INST_DEFINE(n, eth_tc4x_geth_init, NULL, NULL, &geth_config##n, POST_KERNEL,     \
			      ETH_TC4X_GETH_INIT_PRIORITY, NULL);
#endif

DT_INST_FOREACH_STATUS_OKAY(ETH_TC4X_GETH_INIT)
