/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/devicetree.h>
#include <zephyr/drivers/interrupt_controller/intc_aurix_ir.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util_macro.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mbox_aurix_gpsr, CONFIG_MBOX_LOG_LEVEL);

#define DT_DRV_COMPAT infineon_aurix_gpsr

#if CONFIG_SOC_SERIES_TC3X
#include "IfxSrc_reg.h"
#define AURIX_GPSR_IRQ_BASE 612
#elif CONFIG_SOC_SERIES_TC4X
#include "IfxInt_reg.h"
#define AURIX_GPSR_IRQ_BASE 1304
#endif

struct mbox_aurix_config {
	uint16_t irqs[8];
	uint8_t rx_channels;
	void (*irq_init)();
};

struct mbox_aurix_cb {
	mbox_callback_t cb;
	void *user_data;
};

struct mbox_aurix_data {
	struct mbox_aurix_cb callbacks[8];
};

static int mbox_aurix_send(const struct device *dev, mbox_channel_id_t id,
			   const struct mbox_msg *msg)
{
	const struct mbox_aurix_config *cfg = dev->config;

	if (id >= 8) {
		return -EINVAL;
	}

#if CONFIG_SOC_SERIES_TC3X
	if (msg) {
		return -EINVAL;
	}
	intc_aurix_ir_irq_raise(cfg->irqs[id]);
#elif CONFIG_SOC_SERIES_TC4X
	uint8_t gpsr = (cfg->irqs[id] - AURIX_GPSR_IRQ_BASE) / 8;
	uint8_t swc = (cfg->irqs[id] - AURIX_GPSR_IRQ_BASE) % 8;
	uint16_t data = 0;
	uint8_t lockset = 0;

	if (msg) {
		/* Check message size*/
		if (msg->size > 2) {
			return -EINVAL;
		}
		/* Check message status */
		if (MODULE_INT.GPSRG[gpsr].SWC[swc].B.LOCKSTAT) {
			return -EIO;
		}
		memcpy(&data, msg->data, msg->size);
		lockset = msg->size != 0 ? 1 : 0;
	}

	/* Atomically commit DATA + LOCKSET + SETR in a single 32-bit store.
	 * Going through .B as a struct literal lets the compiler emit
	 * sub-word stores, which on this register means LOCKSET can land
	 * before DATA and the data write is then dropped by the lock.
	 */
	MODULE_INT.GPSRG[gpsr].SWC[swc].U =
		((uint32_t)data) |
		((uint32_t)lockset << 16) |
		(1U << 29);
#endif

	return 0;
}

static int mbox_aurix_register_callback(const struct device *dev, mbox_channel_id_t channel_id,
					mbox_callback_t cb, void *user_data)
{
	const struct mbox_aurix_config *cfg = dev->config;
	struct mbox_aurix_data *data = dev->data;

	if (!(cfg->rx_channels & (1 << channel_id))) {
		return -EINVAL;
	}

	data->callbacks[channel_id] = (struct mbox_aurix_cb){cb, user_data};

	return 0;
}

static int mbox_aurix_mtu_get(const struct device *dev)
{
#if CONFIG_SOC_SERIES_TC3X
	return 0;
#elif CONFIG_SOC_SERIES_TC4X
	return 2;
#endif
}

static uint32_t mbox_aurix_max_channels_get(const struct device *dev)
{
	return 8;
}

static int mbox_aurix_set_enabled(const struct device *dev, mbox_channel_id_t channel_id,
				  bool enabled)
{
	const struct mbox_aurix_config *cfg = dev->config;
	if (!(cfg->rx_channels & (1 << channel_id))) {
		return -EINVAL;
	}

	if (enabled) {
		intc_aurix_ir_irq_enable(cfg->irqs[channel_id]);
	} else {
		intc_aurix_ir_irq_disable(cfg->irqs[channel_id]);
	}
	return 0;
}

static int mbox_aurix_init(const struct device *dev)
{
	const struct mbox_aurix_config *cfg = dev->config;

	/* Nothing to initialize if no RX-Channels are used */
	if (cfg->rx_channels == 0) {
		return 0;
	}

	cfg->irq_init();

	return 0;
}

static void mbox_aurix_isr(void *user_data)
{
	const struct device *dev = user_data;
	const struct mbox_aurix_config *cfg = dev->config;
	struct mbox_aurix_data *data = dev->data;
	uint16_t msg_data = 0;
	struct mbox_msg msg = {&msg_data, 0};
	uint16_t irq = intc_aurix_ir_get_active();
	uint8_t grp = (irq - AURIX_GPSR_IRQ_BASE) / 8;
	uint8_t ch = (irq - AURIX_GPSR_IRQ_BASE) % 8;

	/* Check for invald irq */
	if (!(cfg->rx_channels & (1 << ch))) {
		return;
	}

#if CONFIG_SOC_SERIES_TC4X
	if (MODULE_INT.GPSRG[grp].SWC[ch].B.LOCKSTAT) {
		msg.size = 2;
		msg_data = MODULE_INT.GPSRG[grp].SWC[ch].B.DATA;
		/* Clear LOCKSTAT (W1C) via .U so the write lands in one
		 * store, regardless of compiler bitfield codegen.
		 */
		MODULE_INT.GPSRG[grp].SWC[ch].U = (1U << 17);
	}
#endif

	if (data->callbacks[ch].cb) {
		data->callbacks[ch].cb(dev, ch, data->callbacks[ch].user_data, &msg);
	}
}

DEVICE_API(mbox, mbox_aurix_api) = {
	.send = mbox_aurix_send,
	.register_callback = mbox_aurix_register_callback,
	.mtu_get = mbox_aurix_mtu_get,
	.max_channels_get = mbox_aurix_max_channels_get,
	.set_enabled = mbox_aurix_set_enabled,
};

#define MBOX_AURIX_IRQ_DEFINE(node_id, prop, idx)                                                  \
	IRQ_CONNECT(DT_IRQN_BY_IDX(node_id, DT_PROP_BY_IDX(node_id, prop, idx)),                   \
		    DT_IRQ_BY_IDX(node_id, DT_PROP_BY_IDX(node_id, prop, idx), priority),          \
		    mbox_aurix_isr, DEVICE_DT_GET(node_id), 0);

#define MBOX_AURIX_IRQN(i, n)                  DT_INST_IRQN_BY_IDX(n, i)
#define MBOX_AURIX_CHANNEL(node_id, prop, idx) (1 << DT_PROP_BY_IDX(node_id, prop, idx))

#define MBOX_AURIX_IRQ_INIT(n)                                                                     \
	static void mbox_aurix_irq_init_##n()                                                      \
	{                                                                                          \
		DT_INST_FOREACH_PROP_ELEM_SEP(n, rx_channels, MBOX_AURIX_IRQ_DEFINE, (;));         \
	}

#define MBOX_AURIX_INIT(n)                                                                         \
	MBOX_AURIX_IRQ_INIT(n)                                                                     \
	const struct mbox_aurix_config mbox_aurix_config_##n = {                                   \
		.irqs = {LISTIFY(DT_NUM_IRQS(DT_DRV_INST(n)), MBOX_AURIX_IRQN, (, ), n)},          \
		.rx_channels =                                                                     \
			DT_INST_FOREACH_PROP_ELEM_SEP(n, rx_channels, MBOX_AURIX_CHANNEL, (|)),    \
		.irq_init = mbox_aurix_irq_init_##n,                                               \
	};                                                                                         \
	static struct mbox_aurix_data mbox_aurix_data_##n = {};                                    \
	DEVICE_DT_INST_DEFINE(n, mbox_aurix_init, NULL, &mbox_aurix_data_##n,                      \
			      &mbox_aurix_config_##n, POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY,      \
			      &mbox_aurix_api)

DT_INST_FOREACH_STATUS_OKAY(MBOX_AURIX_INIT)
