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

	/* M_CAN module clock is fMCANI (clock_id[0]) per iLLD: the BSP's
	 * bit-timing reference is whatever IfxCan_getModuleFrequency()
	 * returns, which reads PERCCUCON0.CLKSELMCAN — the asynchronous
	 * fMCANI domain. Confirmed against NuttX tc4d7_can.c (module_clock_hz
	 * = 80 MHz fMCANI) and iLLD IfxCan.c:1459/1483. Reporting fMCANH here
	 * mis-scales BRP/TSEG by 100/80 and detunes 500 kbit/s to 625 kbit/s
	 * on the wire.
	 */
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
	/* Override M_CAN's generic min: with 80 MHz core clock the framework
	 * solver otherwise picks BRP=1 / 160 tq for 500 kbit/s, which is
	 * mathematically correct but operationally fragile (no resync margin
	 * past one core-clock cycle). Constrain BRP >= 10 so the solver lands
	 * on the 8..16 tq sweet spot the iLLD/NuttX bit-timing search prefers.
	 */
	.timing_min = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 2,
		.phase_seg2 = 2,
		.prescaler = 10,
	},
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

/* TC3x and TC4x have different MCMCAN register layouts. */
#if defined(CONFIG_SOC_SERIES_TC4X)
#define MCMCAN_CTRL_OFF      0x0000
#define MCMCAN_NODE_NPCR     0x004C
#define MCMCAN_NODE_G0INTR   0x002C
#define MCMCAN_NODE_G1INTR   0x0030
#define MCMCAN_NODE_G2INTR   0x0034
#define MCMCAN_MCR_OFF       0x0070
#define MCMCAN_ACCEN_WRA_OFF 0x0030
#else
#define MCMCAN_CTRL_OFF     0x8000
#define MCMCAN_NODE_GRINT1  0x0014
#define MCMCAN_NODE_GRINT2  0x0018
#define MCMCAN_NODE_NPCR    0x0040
#define MCMCAN_MCR_OFF      0x0030
#endif

#define MCMCAN_MCR          (MCMCAN_CTRL_OFF + MCMCAN_MCR_OFF)
#define MCMCAN_NODE(node)   (MCMCAN_CTRL_OFF + 0x100 + (node) * 0x400)
#define MCMCAN_NPCR(node)   (MCMCAN_NODE(node) + MCMCAN_NODE_NPCR)
#if !defined(CONFIG_SOC_SERIES_TC4X)
#define MCMCAN_GRINT1(node) (MCMCAN_NODE(node) + MCMCAN_NODE_GRINT1)
#define MCMCAN_GRINT2(node) (MCMCAN_NODE(node) + MCMCAN_NODE_GRINT2)
#else
#define MCMCAN_G0INTR(node) (MCMCAN_NODE(node) + MCMCAN_NODE_G0INTR)
#define MCMCAN_G1INTR(node) (MCMCAN_NODE(node) + MCMCAN_NODE_G1INTR)
#define MCMCAN_G2INTR(node) (MCMCAN_NODE(node) + MCMCAN_NODE_G2INTR)
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
#if defined(CONFIG_SOC_SERIES_TC4X)
			/* Mirror NuttX tc4xx_gpio.c::aurix_config_gpio: any
			 * peripheral-owned pad gets PCSRSEL[pin]=1, selecting
			 * "module x port control" instead of the default
			 * Tricore-direct path. iLLD documents PCSRSEL=0 as the
			 * default for CAN, and its diff-equivalent NuttX has
			 * PCSRSEL=BIT(rx_pin)=0x10 on P01 — verified empirically:
			 * NuttX exchanges 16 frames bidir, Zephyr with PCSRSEL=0
			 * gets StuffError on its own readback and hits
			 * ERROR-PASSIVE in <1s. The TX-pin write is overwritten
			 * by the RX-pin write in NuttX (whole-word), so we end up
			 * setting only the RX bit here.
			 */
			if (!pin->output && pin->type == 0) {
				/* AURIX TC4Dx port stride is 0x400; PCSRSEL is at
				 * offset 0x34 within each port. P00 lives at
				 * 0xF003A000.
				 */
				sys_write32(BIT(pin->pin),
					    0xF003A000U + (uint32_t)pin->port * 0x400U + 0x34U);
			}
#endif
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

#if defined(CONFIG_SOC_SERIES_TC4X)
	/* Allow all master TAG IDs to write the M_CAN node registers; without
	 * this CPU0 writes to NBTP/CCCR/etc. are silently dropped because the
	 * reset value of ACCEN_WRA on TC4Dx silicon does not always include
	 * tag 0. */
	sys_write32(0xFFFFFFFF, mcmcan_config->base + MCMCAN_CTRL_OFF + MCMCAN_ACCEN_WRA_OFF);
#endif

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
		 */
		grint1 = ((i * 2) << 28) | ((i * 2) << 24) | ((i * 2) << 20) | ((i * 2) << 16) |
			 ((i * 2) << 12) | ((i * 2) << 8) | ((i * 2) << 4) | ((i * 2) << 0);
		grint2 = ((i * 2) << 28) | ((i * 2) << 24) | ((i * 2) << 20) | ((i * 2 + 1) << 16) |
			 ((i * 2 + 1) << 12) | ((i * 2 + 1) << 8) | ((i * 2 + 1) << 4) |
			 ((i * 2) << 0);
		sys_write32(grint1, mcmcan_config->base + MCMCAN_GRINT1(i));
		sys_write32(grint2, mcmcan_config->base + MCMCAN_GRINT2(i));
#else
		/* Route node i events to SRC_CANINT(i*2) / (i*2+1). */
		{
			uint32_t l0 = (uint32_t)(i * 2);
			uint32_t l1 = (uint32_t)(i * 2 + 1);
			uint32_t g0intr;
			uint32_t g1intr;

			/* G0INTR groups go to line0. */
			g0intr = (l0 << 0) | (l0 << 4) | (l0 << 8) | (l0 << 12) |
				 (l0 << 16) | (l0 << 20) | (l0 << 24) | (l0 << 28);

			/* G1INTR: REINT/RETI/TRAQ/TRACO -> line0;
			 * RxF1F/RxF0F/RxF1N/RxF0N -> line1.
			 */
			g1intr = (l0 << 0) | (l1 << 4) | (l1 << 8) | (l1 << 12) |
				 (l1 << 16) | (l0 << 20) | (l0 << 24) | (l0 << 28);

			sys_write32(g0intr, mcmcan_config->base + MCMCAN_G0INTR(i));
			sys_write32(g1intr, mcmcan_config->base + MCMCAN_G1INTR(i));
			/* G2INTR is CRE-only; we don't use the routing engine,
			 * leave at reset (all CRE groups -> SRC_CANINT0). */
		}
#endif
	}

	/* Enable local clocks. CLKSEL[N] is write-protected unless MCR.CCCE
	 * (bit 31) and MCR.CI (bit 30) are observably set in MCR before the
	 * write that programs CLKSEL — without the priming write CLKSEL
	 * silently stays at 0 ("no clock") on TC4x silicon and the BSP
	 * never starts driving the bus, even though loopback (TEST.LBCK)
	 * still works because it shortcuts the BSP. Mirror iLLD
	 * IfxCan_setClockSource (3 writes: prime, program, drop) and the
	 * NuttX tricore_mcmcan.c sequence at lines 798-801.
	 */
	sys_write32(0xC0000000, mcmcan_config->base + MCMCAN_MCR);
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