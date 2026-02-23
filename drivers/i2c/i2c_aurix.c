/*
 * Copyright (c) 2024 Infineon Technologies AG
 * Copyright (c) 2025 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT infineon_aurix_i2c

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_aurix, CONFIG_I2C_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include "zephyr/devicetree/clocks.h"
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/util.h>

#include "i2c-priv.h"
#include "soc.h"
#include "IfxI2c_reg.h"

#define I2C_TIMEOUT_US		100000
#define I2C_POLL_US		1

#define RIS_LSREQ		BIT(0)
#define RIS_SREQ		BIT(1)
#define RIS_LBREQ		BIT(2)
#define RIS_BREQ		BIT(3)
#define RIS_DTR_ALL		(RIS_LSREQ | RIS_SREQ | RIS_LBREQ | RIS_BREQ)

#define ICR_DTR_ALL		RIS_DTR_ALL

#define PIRQ_NACK		BIT(4)
#define PIRQ_AL			BIT(3)
#define PIRQ_TX_END		BIT(5)
#define PIRQ_ALL		0x7fu

#define ERRIRQ_ALL		0x0fu

#define BUSSTAT_BS_MASK		0x3u
#define BUSSTAT_IDLE		0
#define BUSSTAT_BUSYMASTER	2

#define ADDRCFG_MNS		BIT(19)
#define ADDRCFG_SONA		BIT(20)

#define FIFOCFG_VAL		((2u << 0) | (2u << 4) | (2u << 8) | \
				 (2u << 12) | BIT(16) | BIT(17))

#define RUNCTRL_RUN		BIT(0)

#define ENDDCTRL_SETEND		BIT(1)

#define TIMCFG_SDA_DEL_SHIFT	0
#define TIMCFG_FS_SCL_LOW	BIT(15)
#define TIMCFG_EN_SCL_LOW_LEN	BIT(14)
#define TIMCFG_SCL_LOW_SHIFT	24

#define FDIVCFG_DEC_SHIFT	0
#define FDIVCFG_INC_SHIFT	16

#define CLC1_RMC_SHIFT		8
#define CLC1_RMC_MASK		(0xffu << CLC1_RMC_SHIFT)
#define CLC1_DISR		BIT(0)

struct i2c_aurix_config {
	Ifx_I2C *const base;
	const struct device *const clkctrl;
	const struct pinctrl_dev_config *const pinctrl;
	uint32_t clk;
	uint32_t bitrate;
};

struct i2c_aurix_data {
	struct k_mutex lock;
	uint32_t dev_config;
	uint32_t frequency;
};

static uint32_t i2c_aurix_speed_to_hz(uint32_t dev_config)
{
	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		return I2C_BITRATE_STANDARD;
	case I2C_SPEED_FAST:
		return I2C_BITRATE_FAST;
	case I2C_SPEED_FAST_PLUS:
		return I2C_BITRATE_FAST_PLUS;
	case I2C_SPEED_HIGH:
		return I2C_BITRATE_HIGH;
	case I2C_SPEED_ULTRA:
		return I2C_BITRATE_ULTRA;
	default:
		return 0;
	}
}

static int i2c_aurix_wait_fifo_req(Ifx_I2C *base)
{
	uint32_t elapsed = 0;

	while (elapsed < I2C_TIMEOUT_US) {
		if (base->PIRQSS.U & (PIRQ_NACK | PIRQ_AL)) {
			return -EIO;
		}
		if (base->RIS.U & RIS_DTR_ALL) {
			return 0;
		}
		k_busy_wait(I2C_POLL_US);
		elapsed += I2C_POLL_US;
	}

	return -ETIMEDOUT;
}

static int i2c_aurix_wait_pirq(Ifx_I2C *base)
{
	uint32_t elapsed = 0;

	while (elapsed < I2C_TIMEOUT_US) {
		uint32_t pirq = base->PIRQSS.U;

		if (pirq & (PIRQ_NACK | PIRQ_AL)) {
			return -EIO;
		}
		if (pirq & PIRQ_TX_END) {
			return 0;
		}
		k_busy_wait(I2C_POLL_US);
		elapsed += I2C_POLL_US;
	}

	return -ETIMEDOUT;
}

static void i2c_aurix_clear_irqs(Ifx_I2C *base)
{
	base->PIRQSC.U = PIRQ_ALL;
	base->ERRIRQSC.U = ERRIRQ_ALL;
	base->ICR.U = ICR_DTR_ALL;
}

static int i2c_aurix_send_stop(Ifx_I2C *base)
{
	uint32_t elapsed = 0;

	base->PIRQSC.U = PIRQ_TX_END;
	base->ENDDCTRL.U = ENDDCTRL_SETEND;

	while (elapsed < I2C_TIMEOUT_US) {
		if (base->PIRQSS.U & PIRQ_TX_END) {
			base->PIRQSC.U = PIRQ_TX_END;
			return 0;
		}
		k_busy_wait(I2C_POLL_US);
		elapsed += I2C_POLL_US;
	}

	return -ETIMEDOUT;
}

static int i2c_aurix_set_baudrate(const struct device *dev, uint32_t baudrate)
{
	const struct i2c_aurix_config *cfg = dev->config;
	uint32_t fi2c;
	uint32_t rmc, dec, timcfg;
	int ret;

	ret = clock_control_get_rate(cfg->clkctrl, (void *)&cfg->clk, &fi2c);
	if (ret) {
		return ret;
	}

	rmc = (cfg->base->CLC1.U & CLC1_RMC_MASK) >> CLC1_RMC_SHIFT;
	if (rmc == 0) {
		rmc = 1;
	}

	if (baudrate > 400000) {
		dec = ((fi2c / baudrate) * 46 - 92 + 4) / 5;
	} else {
		dec = ((fi2c / rmc / baudrate) - 3 + 1) / 2;
	}

	if (dec < 6) {
		dec = 6;
	} else if (dec > 0x7ff) {
		dec = 0x7ff;
	}

	if (baudrate > 400000) {
		cfg->base->FDIVCFG.U = (0x1d2u << FDIVCFG_DEC_SHIFT) |
					(5u << FDIVCFG_INC_SHIFT);
		cfg->base->FDIVHIGHCFG.U = (dec << FDIVCFG_DEC_SHIFT) |
					    (46u << FDIVCFG_INC_SHIFT);
	} else {
		cfg->base->FDIVCFG.U = (dec << FDIVCFG_DEC_SHIFT) |
					(1u << FDIVCFG_INC_SHIFT);
	}

	timcfg = 0x3fu << TIMCFG_SDA_DEL_SHIFT;
	if (baudrate == 400000) {
		timcfg |= TIMCFG_FS_SCL_LOW | TIMCFG_EN_SCL_LOW_LEN |
			  (0x20u << TIMCFG_SCL_LOW_SHIFT);
	}
	cfg->base->TIMCFG.U = timcfg;

	return 0;
}

static int i2c_aurix_do_msg(const struct device *dev, struct i2c_msg *msgs,
			    uint8_t num_msgs, uint16_t addr, bool is_read,
			    bool send_stop)
{
	const struct i2c_aurix_config *cfg = dev->config;
	Ifx_I2C *base = cfg->base;
	bool is_10bit = (msgs[0].flags & I2C_MSG_ADDR_10_BITS) != 0;
	uint32_t addr_bytes = is_10bit ? 2 : 1;
	uint32_t total_bytes = 0;
	uint8_t mi = 0, offset = 0;
	int ret;

	for (int i = 0; i < num_msgs; i++) {
		total_bytes += msgs[i].len;
	}

	i2c_aurix_clear_irqs(base);

	base->PIRQSM.U = PIRQ_AL | PIRQ_NACK | PIRQ_TX_END;
	base->IMSC.U = 0;

	if (is_read) {
		base->TPSCTRL.U = addr_bytes;
		base->MRPSCTRL.U = total_bytes;
	} else {
		base->TPSCTRL.U = addr_bytes + total_bytes;
		base->MRPSCTRL.U = 0;
	}

	ret = i2c_aurix_wait_fifo_req(base);
	if (ret < 0) {
		goto err;
	}

	if (is_10bit) {
		base->TXD.U = 0xf0 | ((addr >> 7) & 0x06);
		base->ICR.U = ICR_DTR_ALL;

		ret = i2c_aurix_wait_fifo_req(base);
		if (ret < 0) {
			goto err;
		}
		base->TXD.U = addr & 0xff;
	} else {
		base->TXD.U = ((addr << 1) & 0xfe) | (is_read ? 1 : 0);
	}

	base->ICR.U = ICR_DTR_ALL;

	while (total_bytes > 0) {
		uint32_t ris, count;

		ret = i2c_aurix_wait_fifo_req(base);
		if (ret < 0) {
			goto err;
		}

		ris = base->RIS.U;
		count = (ris & (RIS_LBREQ | RIS_BREQ)) ?
			MIN(4u, total_bytes) : 1;

		for (uint32_t i = 0; i < count && total_bytes > 0; i++) {
			if (is_read) {
				msgs[mi].buf[offset] = base->RXD.U & 0xff;
			} else {
				base->TXD.U = msgs[mi].buf[offset];
			}
			offset++;
			total_bytes--;
			if (offset >= msgs[mi].len) {
				mi++;
				offset = 0;
			}
		}

		base->ICR.U = ICR_DTR_ALL;
	}

	ret = i2c_aurix_wait_pirq(base);
	if (ret < 0) {
		goto err;
	}

	base->PIRQSC.U = PIRQ_TX_END;

	if (send_stop) {
		ret = i2c_aurix_send_stop(base);
	}

	return ret;

err:
	if ((base->BUSSTAT.U & BUSSTAT_BS_MASK) == BUSSTAT_BUSYMASTER) {
		i2c_aurix_send_stop(base);
	}
	i2c_aurix_clear_irqs(base);
	return ret;
}

static int i2c_aurix_transfer(const struct device *dev, struct i2c_msg *msgs,
			      uint8_t num_msgs, uint16_t addr)
{
	const struct i2c_aurix_config *cfg = dev->config;
	struct i2c_aurix_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	if ((cfg->base->BUSSTAT.U & BUSSTAT_BS_MASK) != BUSSTAT_IDLE) {
		ret = -EBUSY;
		goto out;
	}

	uint8_t i = 0;

	while (i < num_msgs) {
		bool is_read = (msgs[i].flags & I2C_MSG_RW_MASK) != 0;
		int start = i;

		while (i < num_msgs) {
			if ((msgs[i].flags & I2C_MSG_RW_MASK) != is_read) {
				break;
			}
			i++;
			if (msgs[i - 1].flags & (I2C_MSG_RESTART | I2C_MSG_STOP)) {
				break;
			}
		}

		bool stop = (i >= num_msgs) ||
			    (msgs[i - 1].flags & I2C_MSG_STOP);

		ret = i2c_aurix_do_msg(dev, &msgs[start], i - start,
				       addr, is_read, stop);
		if (ret < 0) {
			break;
		}
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int i2c_aurix_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_aurix_config *cfg = dev->config;
	struct i2c_aurix_data *data = dev->data;
	uint32_t baudrate;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	baudrate = i2c_aurix_speed_to_hz(dev_config);
	if (baudrate == 0) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	data->dev_config = dev_config;
	cfg->base->RUNCTRL.U = 0;

	ret = i2c_aurix_set_baudrate(dev, baudrate);

	cfg->base->ADDRCFG.U = ADDRCFG_MNS | ADDRCFG_SONA;
	cfg->base->RUNCTRL.U = RUNCTRL_RUN;

	data->frequency = baudrate;
	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_aurix_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_aurix_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	*dev_config = data->dev_config;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int i2c_aurix_recover_bus(const struct device *dev)
{
	const struct i2c_aurix_config *cfg = dev->config;
	struct i2c_aurix_data *data = dev->data;
	uint32_t bs;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	bs = cfg->base->BUSSTAT.U & BUSSTAT_BS_MASK;
	if (bs == BUSSTAT_IDLE) {
		goto out;
	}

	if (bs == BUSSTAT_BUSYMASTER) {
		ret = i2c_aurix_send_stop(cfg->base);
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int i2c_aurix_init(const struct device *dev)
{
	const struct i2c_aurix_config *cfg = dev->config;
	struct i2c_aurix_data *data = dev->data;
	int ret;

	if (!aurix_enable_clock((uintptr_t)&cfg->base->CLC, 1000)) {
		return -EIO;
	}

	cfg->base->CLC1.U = (cfg->base->CLC1.U & ~CLC1_RMC_MASK) |
			     (1u << CLC1_RMC_SHIFT);
	if (!WAIT_FOR(((cfg->base->CLC1.U & CLC1_RMC_MASK) >> CLC1_RMC_SHIFT)
		      == 1, 100, k_busy_wait(1))) {
		LOG_ERR("%s: RMC not ready", dev->name);
		return -EIO;
	}

	if (!aurix_enable_clock((uintptr_t)&cfg->base->CLC1, 1000)) {
		return -EIO;
	}

	ret = pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	cfg->base->ERRIRQSM.U = 0;
	cfg->base->PIRQSM.U = 0;
	cfg->base->IMSC.U = 0;
	cfg->base->RUNCTRL.U = 0;
	cfg->base->FIFOCFG.U = FIFOCFG_VAL;

	k_mutex_init(&data->lock);

	uint32_t bitrate_cfg = i2c_map_dt_bitrate(cfg->bitrate);

	ret = i2c_aurix_configure(dev, bitrate_cfg | I2C_MODE_CONTROLLER);
	if (ret < 0) {
		LOG_ERR("%s: configure failed", dev->name);
		return ret;
	}

	return 0;
}

static DEVICE_API(i2c, i2c_aurix_api) = {
	.configure = i2c_aurix_configure,
	.get_config = i2c_aurix_get_config,
	.transfer = i2c_aurix_transfer,
	.recover_bus = i2c_aurix_recover_bus,
};

#define I2C_AURIX_INIT(n)							\
	PINCTRL_DT_INST_DEFINE(n);						\
	static struct i2c_aurix_data i2c_aurix_data_##n;			\
	static const struct i2c_aurix_config i2c_aurix_config_##n = {		\
		.base = (Ifx_I2C *)DT_INST_REG_ADDR(n),			\
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),			\
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),		\
		.clk = DT_INST_CLOCKS_CELL(n, id),				\
		.bitrate = DT_INST_PROP_OR(n, clock_frequency, 100000),		\
	};									\
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_aurix_init, NULL,			\
				  &i2c_aurix_data_##n,				\
				  &i2c_aurix_config_##n,			\
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,	\
				  &i2c_aurix_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_AURIX_INIT)
