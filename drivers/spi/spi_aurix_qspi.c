/*
 * Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_aurix_qspi
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aurix_spi, CONFIG_SPI_LOG_LEVEL);

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include "zephyr/devicetree/clocks.h"
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/util.h>

#include "spi_context.h"

#include "soc.h"
#include "IfxQspi_reg.h"

#define QSPI_TXFIFO_DEPTH           8
#define QSPI_ECONZ_NUM              8
#define QSPI_RESET_SM_FIFO          0x1
#define QSPI_MASTER_MODE            0x00
#define QSPI_SLAVE_MODE             0x03
#define QSPI_SLAVE_ENABLE(slave_id) BIT(slave_id)
#define QSPI_FIFO_COMBINED_MODE     0x00
#define QSPI_FIFO_SINGLE_MOVE_MODE  0x01
#define QSPI_FIFO_BATCH_MOVE_MODE1  0x02
#define QSPI_FIFO_BATCH_MOVE_MODE2  0x03
#define QSPI_MIN_RESULTANT_DIVIDER  4 /* As per the documentation (Q+1)*(A+B+C+1)>=4 */
#define QSPI_BIT_SEGMENT_A          1
#define QSPI_BIT_SEGMENT_B          0
#define QSPI_BIT_SEGMENT_C          0
#define QSPI_MAX_TIMEOUT            1000

struct spi_aurix_qspi_config {
	Ifx_QSPI * const base;
	const struct device *const clkctrl;
	const struct pinctrl_dev_config *const pinctrl;
	uint32_t clk;
	uint32_t max_clk_frequency;
	uint32_t irq_tx;
	uint32_t irq_rx;
	uint32_t irq_err;
	uint32_t irq_prio_tx;
	uint32_t irq_prio_rx;
	uint32_t irq_prio_err;
	void (*irq_config_func)(const struct device *dev);
};

struct spi_aurix_qspi_data {
	struct spi_context ctx;
	bool first_write;
	bool last_write;
	uint8_t *tx_buf;
	uint8_t *rx_buf;
	size_t tx_len;
	size_t rx_len;
	uint8_t dfs_value;
};

static void qspi_writebacon_beginstream(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
#ifdef CONFIG_SOC_SERIES_TC3X
	Ifx_QSPI_BACON bacon = {0};
#elif CONFIG_SOC_SERIES_TC4X
	Ifx_QSPI_BACONENTRY bacon = {0};
#endif
	bacon.B.DL = SPI_WORD_SIZE_GET(spi_cfg->operation) - 1;
	bacon.B.CS = spi_cfg->slave;
	bacon.B.MSB = (spi_cfg->operation & SPI_TRANSFER_LSB) ? false : true;
	bacon.B.LAST = 0;
	bacon.B.TRAIL = 7;
	LOG_DBG("BACON DL = %u, CS = %u , MSB = %u", bacon.B.DL, bacon.B.CS, bacon.B.MSB);
	cfg->base->BACONENTRY.U = bacon.U;
}

static void qspi_writebacon_endstream(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
#ifdef CONFIG_SOC_SERIES_TC3X
	Ifx_QSPI_BACON bacon = {0};
#elif CONFIG_SOC_SERIES_TC4X
	Ifx_QSPI_BACONENTRY bacon = {0};
#endif
	bacon.B.DL = SPI_WORD_SIZE_GET(spi_cfg->operation) - 1;
	bacon.B.CS = spi_cfg->slave;
	bacon.B.LAST = 1;
	cfg->base->BACONENTRY.U = bacon.U;
}

static uint8_t get_dfs_value(struct spi_context *ctx)
{
	uint8_t word_size = SPI_WORD_SIZE_GET(ctx->config->operation);

	return ((word_size / 8));
}

static uint32_t spi_aurix_qspi_res_div(void)
{
	if (QSPI_BIT_SEGMENT_C == 0 && QSPI_BIT_SEGMENT_B == 0) {
		return (QSPI_BIT_SEGMENT_A + 2);
	} else if (QSPI_BIT_SEGMENT_B == 0) {
		return (QSPI_BIT_SEGMENT_A + QSPI_BIT_SEGMENT_C + 1);
	} else {
		return (QSPI_BIT_SEGMENT_A + QSPI_BIT_SEGMENT_B + QSPI_BIT_SEGMENT_C + 1);
	}
}

static int spi_aurix_qspi_set_frequency(const struct device *dev, uint32_t freq)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
	struct spi_aurix_qspi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	uint32_t fqspi;
	uint32_t tq;
	uint32_t res_abc_div;
	int ret;
	uint32_t slave_id;

	slave_id = ctx->config->slave;

	if (slave_id >= QSPI_ECONZ_NUM) {
		slave_id = slave_id % QSPI_ECONZ_NUM;
	}

	ret = clock_control_get_rate(cfg->clkctrl, &cfg->clk, &fqspi);

	if (ret) {

		return ret;
	}

	if (freq > cfg->max_clk_frequency) {
		return -EINVAL;
	}

	/* Calculate the resultant ABC Div */
	res_abc_div = spi_aurix_qspi_res_div();

	/* Calculate the time quantum (tq) */
	tq = (fqspi + (QSPI_MIN_RESULTANT_DIVIDER * cfg->max_clk_frequency - 1)) /
	     (QSPI_MIN_RESULTANT_DIVIDER * cfg->max_clk_frequency - 1);
	cfg->base->GLOBALCON.B.TQ = tq;
	cfg->base->ECON[slave_id].B.Q = (fqspi / (res_abc_div * freq * (tq + 1))) - 1;
	cfg->base->ECON[slave_id].B.A = QSPI_BIT_SEGMENT_A;
	cfg->base->ECON[slave_id].B.B = QSPI_BIT_SEGMENT_B;
	cfg->base->ECON[slave_id].B.C = QSPI_BIT_SEGMENT_C;
	return 0;
}

static int spi_aurix_qspi_config(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
	struct spi_aurix_qspi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int slave_id = spi_cfg->slave;
	uint32_t result;

	/* Activate the slave select */
	cfg->base->SSOC.B.OEN = QSPI_SLAVE_ENABLE(slave_id);

	if (slave_id >= QSPI_ECONZ_NUM) {
		slave_id = slave_id % QSPI_ECONZ_NUM;
	}

	/* Set the data rate */
	result = spi_aurix_qspi_set_frequency(dev, spi_cfg->frequency);
	if (result != 0) {
		return result;
	}

	/* Slave ID should not surpass the maximum value */
	if (slave_id > CONFIG_SPI_AURIX_QSPI_MAX_SLAVE) {
		return -EINVAL;
	}

	/* Configure Clock Polarity and Phase  */
	if (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPOL) {
		cfg->base->ECON[slave_id].B.CPOL = true;
	} else {
		cfg->base->ECON[slave_id].B.CPOL = false;
	}

	/* Configure LoopBack */
	cfg->base->GLOBALCON.B.LB = (spi_cfg->operation & SPI_MODE_LOOP) ? true : false;
	LOG_DBG("Loopback %u", cfg->base->GLOBALCON.B.LB);

	if (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA) {
		cfg->base->ECON[slave_id].B.CPH = true;
	} else {
		cfg->base->ECON[slave_id].B.CPH = false;
	}

	data->first_write = true;

	/* Enable Single Move Mode for the TXFIFO */
	cfg->base->GLOBALCON1.B.TXFM = QSPI_FIFO_SINGLE_MOVE_MODE;

	/* Enable Single Move Mode for RXFIFO */
	cfg->base->GLOBALCON1.B.RXFM = QSPI_FIFO_SINGLE_MOVE_MODE;

	/* Enable State Machine */
	cfg->base->GLOBALCON.B.EN = true;

	/* Enable Transmit Interrupt */
	cfg->base->GLOBALCON1.B.TXEN = true;

	/* Enable Receive Interrupt */
	cfg->base->GLOBALCON1.B.RXEN = true;

	return 0;
}

static int spi_aurix_qspi_init(const struct device *dev)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
	struct spi_aurix_qspi_data *data = dev->data;
	int ret;

	/* Enable the Clock to QSPI */
	if (!aurix_enable_clock((uintptr_t)&cfg->base->CLC, QSPI_MAX_TIMEOUT)) {
		return -EIO;
	}

	/* Configure dt provided device signals when available */
	ret = pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);

	if (ret < 0) {
		return ret;
	}

	/* Enable the CLKSEL */
	cfg->base->GLOBALCON.B.CLKSEL = true;

	/* Reset the state machine , TX FIFO and RX FIFO */
	cfg->base->GLOBALCON.B.RESETS = QSPI_RESET_SM_FIFO;

	/* Default Master Transmit and receive */
	cfg->base->GLOBALCON.B.MS = QSPI_MASTER_MODE;

	/* No LoopBack */
	cfg->base->GLOBALCON.B.LB = false;

	/* Clear Flags (If Any) */
	cfg->base->FLAGSCLEAR.U = UINT32_MAX;

	/* Configure slave select (master) */
	spi_context_cs_configure_all(&data->ctx);

	/* IRQ Configure function */
	cfg->irq_config_func(dev);

	return 0;
}

static int qspi_transfer_short(const struct device *dev)
{
	struct spi_aurix_qspi_config *const cfg = dev->config;
	struct spi_aurix_qspi_data *const data = dev->data;
	struct spi_context *ctx = &data->ctx;

	if (ctx->tx_len <= 0) {
		return 0;
	}

	LOG_DBG("Entered SPI TRANSFER SHORT ");

	data->last_write = (ctx->tx_len == 1) ? true : false;

	if (data->first_write) {
		LOG_DBG("First Write");
		data->first_write = false;
		qspi_writebacon_beginstream(dev, ctx->config);
		return 0;
	}

	if (data->last_write) {
		LOG_DBG("Last Write Nesting");

		if (SPI_WORD_SIZE_GET(ctx->config->operation) == 8) {
			cfg->base->DATAENTRY[0].U = *((uint8_t *)ctx->tx_buf);
			LOG_DBG("Wrote to fifo %u ", *(uint8_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		} else if (SPI_WORD_SIZE_GET(ctx->config->operation) == 16) {
			cfg->base->DATAENTRY[0].U = *((uint16_t *)ctx->tx_buf);
			LOG_DBG("Wrote to fifo %u ", *(uint16_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		} else {
			cfg->base->DATAENTRY[0].U = *((uint32_t *)ctx->tx_buf);
			LOG_DBG("Wrote to fifo %u ", *(uint32_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		}
		qspi_writebacon_endstream(dev, ctx->config);
	} else {
		if (SPI_WORD_SIZE_GET(ctx->config->operation) == 8) {
			cfg->base->DATAENTRY[0].U = *((uint8_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		} else if (SPI_WORD_SIZE_GET(ctx->config->operation) == 16) {
			cfg->base->DATAENTRY[0].U = *((uint16_t *)ctx->tx_buf);
			LOG_DBG("Wrote to fifo %u ", *(uint16_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		} else {
			cfg->base->DATAENTRY[0].U = *((uint32_t *)ctx->tx_buf);
			LOG_DBG("Wrote to fifo %u ", *(uint32_t *)ctx->tx_buf);
			spi_context_update_tx(ctx, data->dfs_value, 1);
		}
	}

	return 0;
}

static int qspi_receive_short(const struct device *dev)
{
	struct spi_aurix_qspi_config *const cfg = dev->config;
	struct spi_aurix_qspi_data *const data = dev->data;
	struct spi_context *ctx = &data->ctx;

	LOG_DBG("In Receive API");
	if (ctx->rx_len == 0) {
		return 0;
	}

	if (SPI_WORD_SIZE_GET(ctx->config->operation) == 8) {

		if (ctx->rx_buf == NULL) {
#ifdef CONFIG_SOC_SERIES_TC3X
			uint8_t val = (cfg->base->RXEXIT.U & 0xFF);
#elif CONFIG_SOC_SERIES_TC4X
			uint8_t val = (cfg->base->RXEXIT[0].U & 0xFF);
#endif
			spi_context_update_rx(ctx, data->dfs_value, 1);
			LOG_DBG("Received %u", val);
		} else {
			uint32_t rec_val;
#ifdef CONFIG_SOC_SERIES_TC3X
			rec_val = (cfg->base->RXEXIT.U & 0xFF);
#elif CONFIG_SOC_SERIES_TC4X
			rec_val = (cfg->base->RXEXIT[0].U & 0xFF);
#endif
			LOG_DBG("Recevied Value %u", rec_val);
			memcpy(ctx->rx_buf, &rec_val, data->dfs_value);
			spi_context_update_rx(ctx, data->dfs_value, 1);
		}
	} else if (SPI_WORD_SIZE_GET(ctx->config->operation) == 16) {
		if (ctx->rx_buf == NULL) {
#ifdef CONFIG_SOC_SERIES_TC3X
			uint16_t val = (cfg->base->RXEXIT.U & 0xFFFF);
#elif CONFIG_SOC_SERIES_TC4X
			uint16_t val = (cfg->base->RXEXIT[0].U & 0xFFFF);
#endif
			spi_context_update_rx(ctx, data->dfs_value, 1);
			LOG_DBG("Received %u", val);
		} else {
			uint32_t rec_val;
#ifdef CONFIG_SOC_SERIES_TC3X
			rec_val = (cfg->base->RXEXIT.U & 0xFFFF);
#elif CONFIG_SOC_SERIES_TC4X
			rec_val = (cfg->base->RXEXIT[0].U & 0xFFFF);
#endif
			memcpy(ctx->rx_buf, &rec_val, data->dfs_value);
			spi_context_update_rx(ctx, data->dfs_value, 1);
		}
	} else {
		if (ctx->rx_buf == NULL) {
#ifdef CONFIG_SOC_SERIES_TC3X
			uint32_t val = (cfg->base->RXEXIT.U);
#elif CONFIG_SOC_SERIES_TC4X
			uint32_t val = (cfg->base->RXEXIT[0].U);
#endif
			spi_context_update_rx(ctx, data->dfs_value, 1);
			LOG_DBG("Received %u", val);
		} else {
			uint32_t rec_val;
#ifdef CONFIG_SOC_SERIES_TC3X
			rec_val = (cfg->base->RXEXIT.U);
#elif CONFIG_SOC_SERIES_TC4X
			rec_val = (cfg->base->RXEXIT[0].U);
#endif
			memcpy(ctx->rx_buf, &rec_val, data->dfs_value);
			spi_context_update_rx(ctx, data->dfs_value, 1);
		}
	}

	return 0;
}

static int transceive(const struct device *dev, const struct spi_config *spi_cfg,
		      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
		      bool asynchronous, spi_callback_t cb, void *userdata)
{
	int result = 0;
	struct spi_aurix_qspi_config *const cfg = dev->config;
	struct spi_aurix_qspi_data *const data = dev->data;
	struct spi_context *ctx = &data->ctx;

	ctx->config = spi_cfg;

	
        spi_context_release(ctx, result);
        spi_context_lock(ctx, asynchronous, cb, userdata, spi_cfg);

	result = spi_aurix_qspi_config(dev, spi_cfg);
	if (result) {
		LOG_ERR("Error in SPI Configuration (result: 0x%x)", result);
		spi_context_release(ctx, result);
		return result;
	}

	data->dfs_value = get_dfs_value(ctx);

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, data->dfs_value);
	spi_context_cs_control(ctx, true);

	LOG_DBG(" Entering SPI Short Data Mode Transfer");

	qspi_transfer_short(dev);

	result = spi_context_wait_for_completion(&data->ctx);

        spi_context_release(ctx, result);

	return result;
}

static int spi_aurix_qspi_transceive(const struct device *dev, const struct spi_config *spi_cfg,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs)
{
	return transceive(dev, spi_cfg, tx_bufs, rx_bufs, false, NULL, NULL);
}

static int spi_aurix_qspi_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_aurix_qspi_config *cfg = dev->config;
	struct spi_aurix_qspi_data *const data = dev->data;

	/* Disable Interrupts from the module */
	irq_disable(cfg->irq_tx);
	irq_disable(cfg->irq_rx);
	irq_disable(cfg->irq_err);

	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static void spi_aurix_qspi_isr(const struct device *dev)
{

	const struct spi_aurix_qspi_config *cfg = dev->config;
	struct spi_aurix_qspi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;

	/* Check for the interrupt cause and clear the flags */
	LOG_DBG("TXF=%d RXF=%d RXCNT=%d", cfg->base->STATUS.B.TXF, cfg->base->STATUS.B.RXF,
		cfg->base->STATUS.B.RXFIFOLEVEL);

	LOG_DBG("Entered SPI ISR");
	/* Transmit Interrupt request flag */
	if (cfg->base->STATUS.B.TXF) {
		/* Clear the flag */
		cfg->base->FLAGSCLEAR.B.TXC = true;

		/* Handle Transmit */
		qspi_transfer_short(dev);
	}
	if (cfg->base->STATUS.B.RXF) {
		LOG_DBG("Receive Interrupt Triggered");
		/* Clear the flag */
		cfg->base->FLAGSCLEAR.B.RXC = true;
		/* Handle Receive */
		qspi_receive_short(dev);
	}

	if (ctx->tx_len == 0 && ctx->rx_len == 0) {
		spi_context_cs_control(ctx, false);
		spi_context_complete(ctx, dev, 0);
	}
}

static DEVICE_API(spi, qspi_driver_api) = {
	.transceive = spi_aurix_qspi_transceive,
	.release = spi_aurix_qspi_release,
};
#define SPI_AURIX_QSPI_INIT(n)                                                                     \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static void spi_aurix_qspi_irq_config_func_##n(const struct device *dev)                   \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, tx, irq), DT_INST_IRQ_BY_NAME(n, tx, priority), \
			    spi_aurix_qspi_isr, DEVICE_DT_INST_GET(n), 0);                         \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, rx, irq), DT_INST_IRQ_BY_NAME(n, rx, priority), \
			    spi_aurix_qspi_isr, DEVICE_DT_INST_GET(n), 0);                         \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, err, irq),                                      \
			    DT_INST_IRQ_BY_NAME(n, err, priority), spi_aurix_qspi_isr,             \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQ_BY_NAME(n, tx, irq));                                       \
		irq_enable(DT_INST_IRQ_BY_NAME(n, rx, irq));                                       \
		irq_enable(DT_INST_IRQ_BY_NAME(n, err, irq));                                      \
	}                                                                                          \
	static const struct spi_aurix_qspi_config spi_aurix_qspi_config_##n = {                    \
		.base = (Ifx_QSPI *)DT_INST_REG_ADDR(n),                                           \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                      \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk = DT_INST_CLOCKS_CELL(n, id),                                                 \
		.max_clk_frequency = DT_INST_PROP(n, max_clock_frequency),                         \
		.irq_tx = DT_INST_IRQ_BY_NAME(n, tx, irq),                                         \
		.irq_rx = DT_INST_IRQ_BY_NAME(n, rx, irq),                                         \
		.irq_err = DT_INST_IRQ_BY_NAME(n, err, irq),                                       \
		.irq_prio_tx = DT_INST_IRQ_BY_NAME(n, tx, priority),                               \
		.irq_prio_rx = DT_INST_IRQ_BY_NAME(n, rx, priority),                               \
		.irq_prio_err = DT_INST_IRQ_BY_NAME(n, err, priority),                             \
		.irq_config_func = spi_aurix_qspi_irq_config_func_##n,                             \
	};                                                                                         \
                                                                                                   \
	static struct spi_aurix_qspi_data spi_aurix_qspi_data_##n = {                              \
		SPI_CONTEXT_INIT_LOCK(spi_aurix_qspi_data_##n, ctx),                               \
		SPI_CONTEXT_INIT_SYNC(spi_aurix_qspi_data_##n, ctx),                               \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, spi_aurix_qspi_init, NULL, &spi_aurix_qspi_data_##n,              \
			      &spi_aurix_qspi_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,   \
			      &qspi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_AURIX_QSPI_INIT)
