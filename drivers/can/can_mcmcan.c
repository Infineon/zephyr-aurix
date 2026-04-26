/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc.h"
#include "zephyr/arch/common/sys_io.h"
#include "zephyr/devicetree.h"
#include "zephyr/devicetree/clocks.h"
#include "zephyr/kernel.h"
#include "zephyr/sys/util.h"
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/can_mcan.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(can_mcmcan, CONFIG_CAN_LOG_LEVEL);

#define DT_DRV_COMPAT infineon_mcmcan_node

struct mcmcan_config {
	mm_reg_t base;
	const struct device *clock_dev;
	uint32_t clock_id[2];
	const struct pinctrl_dev_config *pinctrl[4];
};

struct mcmcan_node_config {
	mm_reg_t base;
	mem_addr_t mram;
	const struct device *parent;
	void (*irq_config_func)(const struct device *dev);
};

static int mcmcan_mcan_read_reg(const struct device *dev, uint16_t reg, uint32_t *val)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;

	return can_mcan_sys_read_reg(mcmcan_config->base, reg, val);
}

static int mcmcan_mcan_write_reg(const struct device *dev, uint16_t reg, uint32_t val)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;

	return can_mcan_sys_write_reg(mcmcan_config->base, reg, val);
}

static int mcmcan_mcan_read_mram(const struct device *dev, uint16_t offset, void *dst, size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;

	return can_mcan_sys_read_mram(mcmcan_config->mram, offset, dst, len);
}

static int mcmcan_mcan_write_mram(const struct device *dev, uint16_t offset, const void *src,
				  size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;

	return can_mcan_sys_write_mram(mcmcan_config->mram, offset, src, len);
}

static int mcmcan_mcan_clear_mram(const struct device *dev, uint16_t offset, size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;

	return can_mcan_sys_clear_mram(mcmcan_config->mram, offset, len);
}

static int mcmcan_mcan_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_node_config = mcan_config->custom;
	const struct mcmcan_config *mcmcan_config = mcmcan_node_config->parent->config;

	return clock_control_get_rate(mcmcan_config->clock_dev,
				      (clock_control_subsys_t)&mcmcan_config->clock_id[0], rate);
}

static int mcmcan_node_init(const struct device *dev)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct mcmcan_node_config *mcmcan_config = mcan_config->custom;
	int err;

	if (!device_is_ready(mcmcan_config->parent)) {
		LOG_ERR("MCMCAN module not ready");
		return -ENODEV;
	}

	err = can_mcan_configure_mram(dev, 0, mcmcan_config->mram);
	if (err != 0) {
		return -EIO;
	}

	err = can_mcan_init(dev);
	if (err) {
		LOG_ERR("failed to initialize mcan (err %d)", err);
		return err;
	}

	mcmcan_config->irq_config_func(dev);

	return 0;
}

static const struct can_driver_api mcmcan_mcan_driver_api = {
	.get_capabilities = can_mcan_get_capabilities,
	.start = can_mcan_start,
	.stop = can_mcan_stop,
	.set_mode = can_mcan_set_mode,
	.set_timing = can_mcan_set_timing,
	.send = can_mcan_send,
	.add_rx_filter = can_mcan_add_rx_filter,
	.remove_rx_filter = can_mcan_remove_rx_filter,
#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
	.recover = can_mcan_recover,
#endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */
	.get_state = can_mcan_get_state,
	.set_state_change_callback = can_mcan_set_state_change_callback,
	.get_core_clock = mcmcan_mcan_get_core_clock,
	.get_max_filters = can_mcan_get_max_filters,
	.timing_min = CAN_MCAN_TIMING_MIN_INITIALIZER,
	.timing_max = CAN_MCAN_TIMING_MAX_INITIALIZER,
#ifdef CONFIG_CAN_FD_MODE
	.set_timing_data = can_mcan_set_timing_data,
	.timing_data_min = CAN_MCAN_TIMING_DATA_MIN_INITIALIZER,
	.timing_data_max = CAN_MCAN_TIMING_DATA_MAX_INITIALIZER,
#endif /* CONFIG_CAN_FD_MODE */
};

static const struct can_mcan_ops mcmcan_mcan_ops = {
	.read_reg = mcmcan_mcan_read_reg,
	.write_reg = mcmcan_mcan_write_reg,
	.read_mram = mcmcan_mcan_read_mram,
	.write_mram = mcmcan_mcan_write_mram,
	.clear_mram = mcmcan_mcan_clear_mram,
};

#define MCMCAN_NODE_INIT(n)                                                                        \
	CAN_MCAN_DT_INST_BUILD_ASSERT_MRAM_CFG(n);                                                 \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static void mcmcan_mcan_irq_config_##n(const struct device *dev);                          \
                                                                                                   \
	CAN_MCAN_DT_INST_CALLBACKS_DEFINE(n, mcmcan_mcan_cbs_##n);                                 \
                                                                                                   \
	static const struct mcmcan_node_config mcmcan_node_config_##n = {                          \
		.base = CAN_MCAN_DT_INST_MCAN_ADDR(n),                                             \
		.mram = CAN_MCAN_DT_INST_MRAM_ADDR(n),                                             \
		.irq_config_func = mcmcan_mcan_irq_config_##n,                                     \
		.parent = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(n))), \
	};                                                                                         \
	static const struct can_mcan_config mcmcan_mcan_config_##n = CAN_MCAN_DT_CONFIG_INST_GET(  \
		n, &mcmcan_node_config_##n, &mcmcan_mcan_ops, &mcmcan_mcan_cbs_##n);               \
                                                                                                   \
	static struct can_mcan_data can_mcan_data_##n = CAN_MCAN_DATA_INITIALIZER(NULL);           \
                                                                                                   \
	CAN_DEVICE_DT_INST_DEFINE(n, mcmcan_node_init, NULL, &can_mcan_data_##n,                   \
				  &mcmcan_mcan_config_##n, POST_KERNEL, CONFIG_CAN_INIT_PRIORITY,  \
				  &mcmcan_mcan_driver_api);                                        \
                                                                                                   \
	static void mcmcan_mcan_irq_config_##n(const struct device *dev)                           \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, line0, irq),                                    \
			    DT_INST_IRQ_BY_NAME(n, line0, priority), can_mcan_line_0_isr,          \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQ_BY_NAME(n, line0, irq));                                    \
                                                                                                   \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, line1, irq),                                    \
			    DT_INST_IRQ_BY_NAME(n, line1, priority), can_mcan_line_1_isr,          \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQ_BY_NAME(n, line1, irq));                                    \
	}

DT_INST_FOREACH_STATUS_OKAY(MCMCAN_NODE_INIT)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT infineon_mcmcan

/*
 * AURIX MCMCAN module register layout differs between TC3x and TC4x.
 *
 * On TC3x, the controller "reg" property covers the full Ifx_CAN window
 * (32KB Message RAM at +0x0000 followed by control registers at +0x8000),
 * so register accesses use a +0x8000 base offset.  On TC4x the Message RAM
 * lives in a separate sysbus window (0xF4700000 for CAN0, etc.) and the
 * controller "reg" property points directly at the control register block,
 * so the +0x8000 offset disappears.
 *
 * Within Ifx_CAN_N the per-node interrupt-routing registers also moved:
 *   TC3x: GRINT1 @ +0x14, GRINT2 @ +0x18 (two-register routing scheme)
 *   TC4x: G0INTR @ +0x2C, G1INTR @ +0x30, G2INTR @ +0x34 (three-register
 *         scheme with a different field layout).
 * NPCR/PORTCTRL also moved from +0x40 (TC3x) to +0x4C (TC4x).
 *
 * The driver below programs the TC3x routing registers explicitly; on TC4x
 * the GxINTR registers are left at their reset value and IR slot wiring is
 * done entirely through the DT "interrupts" property of each child node.
 */
#if defined(CONFIG_SOC_SERIES_TC4X)
#define MCMCAN_CTRL_OFF     0x0000
#define MCMCAN_NODE_NPCR    0x004C
#else
#define MCMCAN_CTRL_OFF     0x8000
#define MCMCAN_NODE_GRINT1  0x0014
#define MCMCAN_NODE_GRINT2  0x0018
#define MCMCAN_NODE_NPCR    0x0040
#endif

#define MCMCAN_MCR          (MCMCAN_CTRL_OFF + 0x30)
#define MCMCAN_NODE(node)   (MCMCAN_CTRL_OFF + 0x100 + (node) * 0x400)
#define MCMCAN_NPCR(node)   (MCMCAN_NODE(node) + MCMCAN_NODE_NPCR)
#if !defined(CONFIG_SOC_SERIES_TC4X)
#define MCMCAN_GRINT1(node) (MCMCAN_NODE(node) + MCMCAN_NODE_GRINT1)
#define MCMCAN_GRINT2(node) (MCMCAN_NODE(node) + MCMCAN_NODE_GRINT2)
#endif

static int mcmcan_set_node_pinctrl(const struct device *dev, uint8_t node)
{
	const struct mcmcan_config *mcmcan_config = dev->config;
	const struct pinctrl_dev_config *pincfg;
	int i, j;
	int err;

	pincfg = mcmcan_config->pinctrl[node];

	err = pinctrl_apply_state(pincfg, PINCTRL_STATE_DEFAULT);
	if (err) {
		return err;
	}

	for (i = 0; i < pincfg->state_cnt; i++) {
		if (pincfg->states[i].id != PINCTRL_STATE_DEFAULT) {
			continue;
		}
		for (j = 0; j < pincfg->states[i].pin_cnt; j++) {
			const pinctrl_soc_pin_t *pin = &pincfg->states[i].pins[j];
			if (!pin->output && pin->type == 0) {
				sys_write32(pin->alt, mcmcan_config->base + MCMCAN_NPCR(node));
			}
		}
	}

	return 0;
}

static int mcmcan_init(const struct device *dev)
{
	const struct mcmcan_config *mcmcan_config = dev->config;
	int i, err;
	uint32_t mcr;
#if !defined(CONFIG_SOC_SERIES_TC4X)
	uint32_t grint1, grint2;
#endif

	/* Global clock enable */
	if (!device_is_ready(mcmcan_config->clock_dev)) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

	for (i = 0; i < 2; i++) {
		err = clock_control_on(mcmcan_config->clock_dev,
				       (clock_control_subsys_t)&mcmcan_config->clock_id[i]);
		if (err) {
			LOG_ERR("failed to enable clock (err %d)", err);
			return -EINVAL;
		}
	}

	if (!aurix_enable_clock(mcmcan_config->base + MCMCAN_CTRL_OFF, 1000)) {
		LOG_ERR("failed to enable clock gate");
		return -ETIMEDOUT;
	}

	/* Initialize MRAM */
	sys_write32(0xC0000000, mcmcan_config->base + MCMCAN_MCR);
	if (!WAIT_FOR((sys_read32(mcmcan_config->base + MCMCAN_MCR) & BIT(28)) == 0, 1000,
		      k_busy_wait(1))) {
		LOG_ERR("message not ready to be cleared");
		return -ETIMEDOUT;
	}
	sys_write32(0xC0000000 | BIT(29), mcmcan_config->base + MCMCAN_MCR);
	sys_read32(mcmcan_config->base + MCMCAN_MCR);
	if (!WAIT_FOR((sys_read32(mcmcan_config->base + MCMCAN_MCR) & BIT(28)) == 0, 1000,
		      k_busy_wait(1))) {
		LOG_ERR("message ram not cleared");
		return -ETIMEDOUT;
	}
	sys_write32(0x0, mcmcan_config->base + MCMCAN_MCR);

	mcr = 0;
	for (i = 0; i < 4; i++) {
		if (!mcmcan_config->pinctrl[i]) {
			continue;
		}
		/* Set local clock enable */
		mcr |= (0x3 << i * 2);

		/* Set pinctrl */
		err = mcmcan_set_node_pinctrl(dev, i);
		if (err) {
			return err;
		}

#if !defined(CONFIG_SOC_SERIES_TC4X)
		/*
		 * TC3x interrupt-compactor: program the two GRINTx registers
		 * so rxfifo0 / rxfifo1 IRQs go to line1 and the rest to line0.
		 * TC4x has a different (3-register) scheme; leave the GxINTR
		 * registers at reset and rely on the IR-slot wiring from DT.
		 */
		grint1 = ((i * 2) << 28) | ((i * 2) << 24) | ((i * 2) << 20) | ((i * 2) << 16) |
			 ((i * 2) << 12) | ((i * 2) << 8) | ((i * 2) << 4) | ((i * 2) << 0);
		grint2 = ((i * 2) << 28) | ((i * 2) << 24) | ((i * 2) << 20) | ((i * 2 + 1) << 16) |
			 ((i * 2 + 1) << 12) | ((i * 2 + 1) << 8) | ((i * 2 + 1) << 4) |
			 ((i * 2) << 0);
		sys_write32(grint1, mcmcan_config->base + MCMCAN_GRINT1(i));
		sys_write32(grint2, mcmcan_config->base + MCMCAN_GRINT2(i));
#endif
	}

	/* Enable local clocks */
	sys_write32(0xC0000000 | mcr, mcmcan_config->base + MCMCAN_MCR);
	sys_write32(mcr, mcmcan_config->base + MCMCAN_MCR);

	return 0;
}

#define MCMCAN_NODE_PINCTRL(child)                                                                 \
	COND_CODE_1(DT_NODE_HAS_STATUS(child, okay), (PINCTRL_DT_DEV_CONFIG_GET(child)), (NULL))

#define MCMCAN_INIT(n)                                                                             \
	static const struct mcmcan_config mcmcan_config_##n = {                                    \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.clock_dev = DEVICE_DT_GET_OR_NULL(DT_INST_CLOCKS_CTLR(n)),                        \
		.clock_id =                                                                        \
			{                                                                          \
				DT_INST_CLOCKS_CELL_BY_IDX(n, 0, id),                              \
				DT_INST_CLOCKS_CELL_BY_IDX(n, 1, id),                              \
			},                                                                         \
		.pinctrl = {DT_INST_FOREACH_CHILD_SEP(n, MCMCAN_NODE_PINCTRL, (, ))}};             \
	DEVICE_DT_INST_DEFINE(n, mcmcan_init, NULL, NULL, &mcmcan_config_##n, POST_KERNEL,         \
			      CONFIG_CAN_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MCMCAN_INIT)