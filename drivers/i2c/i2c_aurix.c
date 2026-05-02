/*
 * Copyright (c) 2024 Infineon Technologies AG
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

static inline uint32_t i2c_aurix_map_speed(uint32_t cfg)
{
	switch (I2C_SPEED_GET(cfg)) {
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

struct i2c_aurix_config {
	Ifx_I2C *const base;
	const struct device *const clkctrl;
	const struct pinctrl_dev_config *const pinctrl;
	uint32_t clk;
	uint32_t bitrate;
	void (*config_func)();
};

struct i2c_aurix_data {
	struct k_mutex lock;
	struct k_event irq_event;
	uint32_t dev_config;

	size_t bytes;
	size_t offset;
	struct i2c_msg *msgs;
	bool rnw;
};

static inline int i2c_aurix_set_baudrate(const struct device *dev, uint32_t baudrate)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	uint32_t fi2c;
	uint8_t rmc = cfg->base->CLC1.B.RMC;
	uint32_t dec;
	int ret;

	ret = clock_control_get_rate(cfg->clkctrl, (void *)&cfg->clk, &fi2c);
	if (ret) {
		return ret;
	}

	if (baudrate > 400000) 
	{
		dec = DIV_ROUND_UP((((fi2c / baudrate) * 46) - 92), 5); 
	} else 
	{
		dec = DIV_ROUND_UP((((fi2c / rmc) / baudrate) - 3), 2); 
	}

	if (dec < 6) {
		dec = 6;
	} else if (dec > BIT(11) - 1) {
		dec = BIT(11) - 1;
	}

	if (baudrate > 400000) {
		cfg->base->FDIVCFG.B = (Ifx_I2C_FDIVCFG_Bits){.DEC = 0x1D2, .INC = 5};
		cfg->base->FDIVHIGHCFG.B = (Ifx_I2C_FDIVHIGHCFG_Bits){.DEC = dec, .INC = 46};
	} else {
		cfg->base->FDIVCFG.B = (Ifx_I2C_FDIVCFG_Bits){.DEC = dec, .INC = 1};
	}

	cfg->base->TIMCFG.B =
		(Ifx_I2C_TIMCFG_Bits){.SDA_DEL_HD_DAT = 0x3F,
				      .FS_SCL_LOW = baudrate == I2C_BITRATE_FAST ? 1 : 0,
				      .EN_SCL_LOW_LEN = baudrate == I2C_BITRATE_FAST ? 1 : 0,
				      .SCL_LOW_LEN = 0x20};

	return 0;
}

static int i2c_aurix_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	data->dev_config = dev_config;
	cfg->base->RUNCTRL.B.RUN = 0;
	ret = i2c_aurix_set_baudrate(dev, i2c_aurix_map_speed(dev_config));
	cfg->base->ADDRCFG.B.MNS = (dev_config & I2C_MODE_CONTROLLER) != 0;
	cfg->base->RUNCTRL.B.RUN = 1;

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_aurix_get_config(const struct device *dev, uint32_t *dev_config)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	*dev_config = data->dev_config;
	k_mutex_unlock(&data->lock);

	return 0;
}

enum i2c_aurix_bus_state {
	I2C_STATUS_IDLE = 0,
	I2C_STATUS_STARTED = 1,
	I2C_STATUS_BUSYMASTER = 2,
	I2C_STATUS_REMOTESLAVE = 3
};

static inline void i2c_aurix_clear_data_irqs(const struct device *dev)
{
	((const struct i2c_aurix_config *)dev->config)->base->ICR.B =
		(Ifx_I2C_ICR_Bits){.BREQ_INT = 1, .LBREQ_INT = 1, .LSREQ_INT = 1, .SREQ_INT = 1};
}

static inline void i2c_aurix_clear_protocol_irqs(const struct device *dev)
{
	((const struct i2c_aurix_config *)dev->config)->base->PIRQSC.U = 0x7F;
}

static inline void i2c_aurix_dtr_isr(const struct device *const dev)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	uint32_t i = 0;

	if (cfg->base->MIS.B.LBREQ_INT || cfg->base->MIS.B.BREQ_INT) {
		for (i = 0; i < 4 && data->bytes > 0; i++, data->bytes--) {
			if (data->rnw) {
				*(data->msgs->buf + data->offset++) = cfg->base->RXD.U & 0xFF;
			} else {
				cfg->base->TXD.U = *(data->msgs->buf + data->offset++);
			}
			if (data->msgs->len == data->offset) {
				data->offset = 0;
				data->msgs++;
			}
		}
	} else {
		data->bytes--;
		if (data->rnw) {
			*(data->msgs->buf + data->offset++) = cfg->base->RXD.U & 0xFF;
		} else {
			cfg->base->TXD.U = *(data->msgs->buf + data->offset++);
		}
		if (data->msgs->len == data->offset) {
			data->offset = 0;
			data->msgs++;
		}
	}
	i2c_aurix_clear_data_irqs(dev);
}

static inline void i2c_aurix_err_isr(const struct device *const dev)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
}

static inline void i2c_aurix_p_isr(const struct device *const dev)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;

	Ifx_I2C_PIRQSS_Bits pirqs = cfg->base->PIRQSS.B;
	if (pirqs.NACK == 1 || pirqs.AL == 1) {
		k_event_post(&data->irq_event, BIT(1));
	}
	if (pirqs.TX_END == 1) {
		k_event_post(&data->irq_event, BIT(0));
	}
	i2c_aurix_clear_protocol_irqs(dev);
}

static int i2c_aurix_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			      uint16_t addr)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	uint8_t msg_idx, i;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	enum i2c_aurix_bus_state state = cfg->base->BUSSTAT.B.BS;
	if (state != I2C_STATUS_IDLE) {
		ret = -EIO;
		goto err;
	}

	for (msg_idx = 0; msg_idx < num_msgs; msg_idx++) {
		bool addr_10bits = (msgs[msg_idx].flags & I2C_MSG_ADDR_10_BITS) != 0;

		data->rnw = msgs[msg_idx].flags & I2C_MSG_RW_MASK;
		data->bytes = 0;
		data->msgs = &msgs[msg_idx];
		data->offset = 0;

		for (i = msg_idx; i < num_msgs; i++) {
			
			if ((msgs[i].flags & I2C_MSG_RW_MASK) != data->rnw) {
				msg_idx = i - 1;
				break;
			}

			data->bytes += msgs[i].len;

			if (msgs[i].flags & (I2C_MSG_RESTART | I2C_MSG_STOP)) {
				msg_idx = i;
				break;
			}
		}

		i2c_aurix_clear_protocol_irqs(dev);
		cfg->base->PIRQSM.B = (Ifx_I2C_PIRQSM_Bits){.AL = 1, .NACK = 1, .TX_END = 1};
		cfg->base->IMSC.U = 0;
		cfg->base->TPSCTRL.U = data->rnw ? 1 : data->bytes + (addr_10bits ? 2 : 1);
		cfg->base->MRPSCTRL.U = data->rnw ? data->bytes : 0;

		if (addr_10bits) {
			while (cfg->base->RIS.B.SREQ_INT == 0 && cfg->base->RIS.B.LSREQ_INT == 0 &&
			       cfg->base->RIS.B.BREQ_INT == 0 && cfg->base->RIS.B.LBREQ_INT == 0)
				;
			cfg->base->TXD.U = 0xF0 | ((addr >> 7) & 0x6);
			i2c_aurix_clear_data_irqs(dev);
			while (cfg->base->RIS.B.SREQ_INT == 0 && cfg->base->RIS.B.LSREQ_INT == 0 &&
			       cfg->base->RIS.B.BREQ_INT == 0 && cfg->base->RIS.B.LBREQ_INT == 0)
				;
			cfg->base->TXD.U = addr & 0xFF;
			i2c_aurix_clear_data_irqs(dev);
		} else {
			while (cfg->base->RIS.B.SREQ_INT == 0 && cfg->base->RIS.B.LSREQ_INT == 0 &&
			       cfg->base->RIS.B.BREQ_INT == 0 && cfg->base->RIS.B.LBREQ_INT == 0)
				;
			cfg->base->TXD.U = ((addr << 1) & 0xFE) | data->rnw;
			i2c_aurix_clear_data_irqs(dev);
		}
		cfg->base->IMSC.B = (Ifx_I2C_IMSC_Bits){1, 1, 1, 1, 1, 1};

		uint32_t ev = k_event_wait(&data->irq_event, 0xF, true, K_FOREVER);

		if (msgs[msg_idx].flags & I2C_MSG_STOP) {
			if (!(ev & BIT(0))) {
				
				k_event_clear(&data->irq_event, 0x1);
				cfg->base->ENDDCTRL.B.SETEND = 1;
				k_event_wait(&data->irq_event, 0x1, false, K_FOREVER);
			}
		}

		if (k_event_clear(&data->irq_event, 0xF) & BIT(1)) {
			ret = -EIO;
			break;
		}
	}

err:
	
	if (cfg->base->BUSSTAT.B.BS == I2C_STATUS_BUSYMASTER) {
		k_event_clear(&data->irq_event, 0x1);
		cfg->base->ENDDCTRL.B.SETEND = 1;
		k_event_wait(&data->irq_event, 0x1, false, K_FOREVER);
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int i2c_aurix_target_register(const struct device *dev, struct i2c_target_config *cfg)
{
	return -ENOSYS;
}
static int i2c_aurix_target_unregister(const struct device *dev, struct i2c_target_config *cfg)
{
	return -ENOSYS;
}
#ifdef CONFIG_I2C_CALLBACK
static int i2c_aurix_transfer_cb(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
				 uint16_t addr, i2c_callback_t cb, void *userdata)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(msgs);
	ARG_UNUSED(num_msgs);
	ARG_UNUSED(addr);
	ARG_UNUSED(cb);
	ARG_UNUSED(userdata);
	return -ENOSYS;
}
#endif 
#if defined(CONFIG_I2C_RTIO) || defined(__DOXYGEN__)

static void i2c_aurix_iodev_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe)
{
}
#endif 

static int i2c_aurix_recover_bus(const struct device *dev)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (cfg->base->BUSSTAT.B.BS == I2C_STATUS_IDLE) {
		ret = 0;
	} else if (cfg->base->BUSSTAT.B.BS == I2C_STATUS_REMOTESLAVE) {
		if (cfg->base->BUSSTAT.B.RNW) {
			k_event_clear(&data->irq_event, 0x1);
			cfg->base->ENDDCTRL.B.SETEND = 1;
			if (k_event_wait(&data->irq_event, 0x1, false, K_MSEC(1)) == 0) {
				ret = -EBUSY;
				goto err;
			}
		}

		ret = WAIT_FOR(cfg->base->BUSSTAT.B.BS == I2C_STATUS_IDLE, 10000,
			       k_sleep(K_MSEC(1)))
			      ? 0
			      : -EBUSY;
	} else {
		k_event_clear(&data->irq_event, 0x1);
		cfg->base->ENDDCTRL.B.SETEND = 1;
		if (k_event_wait(&data->irq_event, 0x1, false, K_MSEC(1)) == 0) {
			ret = -EBUSY;
		}
	}

err:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int i2c_aurix_init(const struct device *dev)
{
	const struct i2c_aurix_config *const cfg = dev->config;
	struct i2c_aurix_data *const data = dev->data;
	int ret;

	if (!aurix_enable_clock((uintptr_t)&cfg->base->CLC, 1000)) {
		return -EIO;
	}
	cfg->base->CLC1.B.RMC = 1;
	if (!WAIT_FOR(cfg->base->CLC1.B.RMC == 1, 100, k_busy_wait(1))) {
		LOG_ERR("%s: Failed to enable run clock", dev->name);
		return -EIO;
	}
	if (!aurix_enable_clock((uintptr_t)&cfg->base->CLC1, 1000)) {
		return -EIO;
	}

	ret = pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("%s: Failed to apply pinctrl state", dev->name);
		return ret;
	}

	cfg->base->ERRIRQSM.U = 0;
	cfg->base->PIRQSM.U = 0;
	cfg->base->IMSC.U = 0;
	cfg->base->RUNCTRL.B.RUN = 0;

	cfg->base->ADDRCFG.B = (Ifx_I2C_ADDRCFG_Bits){.ADR = 0,
						      .TBAM = 0,
						      .GCE = 0, 
						      .MCE = 0, 
						      .MNS = 1,
						      .SONA = 1,
						      .SOPE = 0};
	cfg->base->FIFOCFG.B = (Ifx_I2C_FIFOCFG_Bits){
		.RXBS = 2,
		.TXBS = 2,
		.RXFA = 2, 
		.TXFA = 2, 
		.TXFC = 1,
		.RXFC = 1,
		.CRBC = 0,
	};
	k_mutex_init(&data->lock);
	k_event_init(&data->irq_event);

	cfg->config_func();

	uint32_t bitrate_cfg = i2c_map_dt_bitrate(cfg->bitrate);
	ret = i2c_aurix_configure(dev, bitrate_cfg | I2C_MODE_CONTROLLER);
	if (ret < 0) {
		LOG_ERR("%s: Failed to configure device", dev->name);
		return ret;
	}

	return 0;
}

static const struct i2c_driver_api i2c_aurix_api = {
	.configure = i2c_aurix_configure,
	.get_config = i2c_aurix_get_config,
	.transfer = i2c_aurix_transfer,
	.target_register = i2c_aurix_target_register,
	.target_unregister = i2c_aurix_target_unregister,
#ifdef CONFIG_I2C_CALLBACK
	.transfer_cb = i2c_aurix_transfer_cb,
#endif
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_aurix_iodev_submit,
#endif
	.recover_bus = i2c_aurix_recover_bus,
};

#define I2C_AURIX_CONFIC_FUNC(n)                                                                   \
	static void i2c_aurix_config_func_##n()                                                    \
	{                                                                                          \
		const struct device *const dev = DEVICE_DT_INST_GET(n);                            \
		const struct i2c_aurix_config *const cfg = dev->config;                            \
                                                                                                   \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, data_transfer, irq),                            \
			    DT_INST_IRQ_BY_NAME(n, data_transfer, priority), i2c_aurix_dtr_isr,    \
			    dev, 0);                                                               \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, error, irq),                                    \
			    DT_INST_IRQ_BY_NAME(n, error, priority), i2c_aurix_err_isr, dev, 0);   \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, protocol, irq),                                 \
			    DT_INST_IRQ_BY_NAME(n, protocol, priority), i2c_aurix_p_isr, dev, 0);  \
                                                                                                   \
		irq_enable(DT_INST_IRQ_BY_NAME(n, data_transfer, irq));                            \
		irq_enable(DT_INST_IRQ_BY_NAME(n, error, irq));                                    \
		irq_enable(DT_INST_IRQ_BY_NAME(n, protocol, irq));                                 \
	}

#define I2C_AURIX_INIT(n)                                                                          \
	I2C_AURIX_CONFIC_FUNC(n)                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static struct i2c_aurix_data i2c_aurix_data_##n = {};                                      \
	const static struct i2c_aurix_config i2c_aurix_config_##n = {                              \
		.base = (Ifx_I2C *)DT_INST_REG_ADDR(n),                                            \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                      \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk = DT_INST_CLOCKS_CELL(n, id),                                                 \
		.bitrate = DT_INST_PROP_OR(n, clock_frequency, 100000),                            \
		.config_func = i2c_aurix_config_func_##n,                                          \
	};                                                                                         \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_aurix_init, NULL, &i2c_aurix_data_##n,                    \
				  &i2c_aurix_config_##n, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,    \
				  &i2c_aurix_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_AURIX_INIT)
