/*
 * Copyright (c) 2026 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT infineon_aurix_qspi

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_aurix, CONFIG_SPI_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/devicetree/clocks.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "spi_context.h"
#include "soc.h"
#include "IfxQspi_reg.h"

#define QSPI_TIMEOUT_LOOPS 100000U

struct spi_aurix_config {
	Ifx_QSPI *const base;
	const struct device *const clkctrl;
	const struct pinctrl_dev_config *const pinctrl;
	uint32_t clk;
	uint32_t sclk_default;
	bool loopback;
};

struct spi_aurix_data {
	struct spi_context ctx;
	uint32_t fqspi;
	uint32_t configured_frequency;
	uint8_t configured_mode;
	uint8_t configured_word_size;
	bool configured;
};

static inline uint32_t qspi_rxexit(Ifx_QSPI *base)
{
#ifdef CONFIG_SOC_SERIES_TC3X
	return base->RXEXIT.U;
#else
	return base->RXEXIT[0].U;
#endif
}

static void spi_aurix_drain_rx(Ifx_QSPI *base)
{
	uint32_t guard = QSPI_TIMEOUT_LOOPS;

	while (base->STATUS.B.RXFIFOLEVEL != 0U && guard-- != 0U) {
		(void)qspi_rxexit(base);
	}
}

static void spi_aurix_clear_flags(Ifx_QSPI *base)
{
	base->FLAGSCLEAR.U = 0x1FFU | BIT(9) | BIT(10);
}

static void spi_aurix_program_econ(Ifx_QSPI *base, struct spi_aurix_data *data, uint8_t cs,
				   uint32_t frequency, uint8_t mode)
{
	const uint32_t a = 1U;
	const uint32_t b = 0U;
	const uint32_t c = 2U;
	uint32_t segs = a + 1U + b + c;
	uint32_t q;
	uint32_t econ;

	if (frequency == 0U) {
		frequency = 1000000U;
	}

	q = (data->fqspi + frequency * segs - 1U) / (frequency * segs);
	if (q == 0U) {
		q = 1U;
	}
	q -= 1U;
	if (q > 63U) {
		q = 63U;
	}

	econ = (q & 0x3FU) |
	       (a << 6) |
	       (b << 8) |
	       (c << 10);
	if (mode & 1U) {
		econ |= BIT(12); /* CPH */
	}
	if (mode & 2U) {
		econ |= BIT(13); /* CPOL */
	}

	base->ECON[cs & 0x7U].U = econ;
}

static int spi_aurix_configure(const struct spi_aurix_config *cfg,
			       struct spi_aurix_data *data, const struct spi_config *spi_cfg)
{
	Ifx_QSPI *base = cfg->base;
	uint8_t word_size;
	uint8_t mode;
	uint8_t cs;

	if (spi_cfg->operation & (SPI_OP_MODE_SLAVE | SPI_TRANSFER_LSB |
				  SPI_LINES_DUAL | SPI_LINES_QUAD | SPI_LINES_OCTAL)) {
		LOG_ERR("unsupported operation flags 0x%x", spi_cfg->operation);
		return -ENOTSUP;
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	if (word_size != 8U && word_size != 16U) {
		LOG_ERR("unsupported word size %u", word_size);
		return -ENOTSUP;
	}

	mode = ((spi_cfg->operation & SPI_MODE_CPOL) ? 2U : 0U) |
	       ((spi_cfg->operation & SPI_MODE_CPHA) ? 1U : 0U);
	cs = (uint8_t)(spi_cfg->slave & 0xFU);

	spi_aurix_program_econ(base, data, cs, spi_cfg->frequency, mode);

	data->configured_frequency = spi_cfg->frequency;
	data->configured_mode = mode;
	data->configured_word_size = word_size;
	data->configured = true;
	return 0;
}

static int spi_aurix_xfer_one(Ifx_QSPI *base, uint16_t tx, uint16_t *rx, uint8_t cs,
			      uint8_t word_size, bool last)
{
	uint32_t bacon;
	uint32_t guard = QSPI_TIMEOUT_LOOPS;

	spi_aurix_drain_rx(base);
	spi_aurix_clear_flags(base);

	bacon = ((uint32_t)cs << 28) |
		(((uint32_t)(word_size / 8U - 1U) & 0x1FU) << 23) |
		BIT(22) |                /* BYTE = 1 (DL counts bytes) */
		BIT(21) |                /* MSB first */
		(last ? BIT(0) : 0U) |   /* LAST */
		(1U << 7) |              /* LPRE = 1 */
		(4U << 10) |             /* LEAD = 4 */
		(1U << 13) |             /* TPRE = 1 */
		(4U << 16) |             /* TRAIL = 4 */
		(2U << 1) |              /* IPRE = 2 */
		(4U << 4);               /* IDLE = 4 */

	base->BACONENTRY.U = bacon;
	base->DATAENTRY[0].U = tx;

	while (base->STATUS.B.RXFIFOLEVEL == 0U) {
		if (guard-- == 0U) {
			return -ETIMEDOUT;
		}
	}

	if (rx != NULL) {
		*rx = (uint16_t)qspi_rxexit(base);
	} else {
		(void)qspi_rxexit(base);
	}
	return 0;
}

static int spi_aurix_transceive_impl(const struct device *dev, const struct spi_config *spi_cfg,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs)
{
	const struct spi_aurix_config *cfg = dev->config;
	struct spi_aurix_data *data = dev->data;
	Ifx_QSPI *base = cfg->base;
	uint8_t word_size;
	uint8_t cs;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	ret = spi_aurix_configure(cfg, data, spi_cfg);
	if (ret != 0) {
		goto out;
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	cs = (uint8_t)(spi_cfg->slave & 0xFU);

	bool loopback = cfg->loopback || ((spi_cfg->operation & SPI_MODE_LOOP) != 0U);

	if (base->GLOBALCON.B.LB != (loopback ? 1U : 0U)) {
		base->GLOBALCON.B.LB = loopback ? 1U : 0U;
	}

	if (!loopback) {
		base->SSOC.B.OEN |= BIT(cs);
	}

	uint8_t dfs = word_size / 8U;

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, dfs);

	size_t tx_total = spi_context_total_tx_len(&data->ctx);
	size_t rx_total = spi_context_total_rx_len(&data->ctx);
	size_t total_frames = MAX(tx_total, rx_total) / dfs;

	for (size_t i = 0; i < total_frames; i++) {
		uint16_t tx_word = 0U;
		uint16_t rx_word = 0U;
		bool last = (i == total_frames - 1U);

		if (spi_context_tx_buf_on(&data->ctx)) {
			if (word_size == 8U) {
				tx_word = *(const uint8_t *)data->ctx.tx_buf;
			} else {
				tx_word = *(const uint16_t *)data->ctx.tx_buf;
			}
		}
		spi_context_update_tx(&data->ctx, dfs, 1);

		ret = spi_aurix_xfer_one(base, tx_word, &rx_word, cs, word_size, last);
		if (ret != 0) {
			break;
		}

		if (spi_context_rx_buf_on(&data->ctx)) {
			if (word_size == 8U) {
				*(uint8_t *)data->ctx.rx_buf = (uint8_t)rx_word;
			} else {
				*(uint16_t *)data->ctx.rx_buf = rx_word;
			}
		}
		spi_context_update_rx(&data->ctx, dfs, 1);
	}

out:
	spi_context_release(&data->ctx, ret);
	return ret;
}

static int spi_aurix_transceive(const struct device *dev, const struct spi_config *spi_cfg,
				const struct spi_buf_set *tx_bufs,
				const struct spi_buf_set *rx_bufs)
{
	return spi_aurix_transceive_impl(dev, spi_cfg, tx_bufs, rx_bufs);
}

static int spi_aurix_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_aurix_data *data = dev->data;

	ARG_UNUSED(spi_cfg);
	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static int spi_aurix_init(const struct device *dev)
{
	const struct spi_aurix_config *cfg = dev->config;
	struct spi_aurix_data *data = dev->data;
	Ifx_QSPI *base = cfg->base;
	uint32_t fqspi;
	int ret;

	if (!aurix_enable_clock((uintptr_t)&base->CLC, 1000U)) {
		LOG_ERR("%s: CLC enable failed", dev->name);
		return -EIO;
	}

	ret = pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("%s: pinctrl_apply_state failed: %d", dev->name, ret);
		return ret;
	}

	ret = clock_control_get_rate(cfg->clkctrl, (void *)&cfg->clk, &fqspi);
	if (ret != 0) {
		LOG_ERR("%s: clock_control_get_rate failed: %d", dev->name, ret);
		return ret;
	}
	data->fqspi = fqspi;

	base->GLOBALCON.U = (cfg->loopback ? BIT(14) : 0U) |
			    (15U << 10) |         /* EXPECT = 2^21 Tqspi timeout */
			    BIT(24) |             /* EN = run */
			    (1U << 25) |          /* MS = master */
			    BIT(29) |             /* CLKSEL = fqspi */
			    (1U << 30);           /* RESETS = run */
	base->GLOBALCON1.U = 0U;

	if (cfg->loopback) {
		base->SSOC.U = 0U;
	}

	spi_aurix_program_econ(base, data, 0U, cfg->sclk_default, 0U);
	data->configured_frequency = cfg->sclk_default;
	data->configured_mode = 0U;
	data->configured_word_size = 8U;
	data->configured = true;

	base->FLAGSCLEAR.U = 0x1FFU | BIT(9) | BIT(10) | BIT(15);

	spi_context_unlock_unconditionally(&data->ctx);

	LOG_INF("%s: ready fqspi=%u Hz loopback=%d", dev->name, fqspi, cfg->loopback);
	return 0;
}

static DEVICE_API(spi, spi_aurix_api) = {
	.transceive = spi_aurix_transceive,
	.release = spi_aurix_release,
};

#define SPI_AURIX_INIT(n)                                                                          \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static struct spi_aurix_data spi_aurix_data_##n = {                                        \
		SPI_CONTEXT_INIT_LOCK(spi_aurix_data_##n, ctx),                                    \
		SPI_CONTEXT_INIT_SYNC(spi_aurix_data_##n, ctx),                                    \
	};                                                                                         \
	static const struct spi_aurix_config spi_aurix_config_##n = {                              \
		.base = (Ifx_QSPI *)DT_INST_REG_ADDR(n),                                           \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                      \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk = DT_INST_CLOCKS_CELL(n, id),                                                 \
		.sclk_default = DT_INST_PROP(n, infineon_sclk_frequency),                          \
		.loopback = DT_INST_PROP(n, infineon_loopback),                                    \
	};                                                                                         \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_aurix_init, NULL, &spi_aurix_data_##n,                    \
				  &spi_aurix_config_##n, POST_KERNEL,                              \
				  CONFIG_SPI_INIT_PRIORITY, &spi_aurix_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_AURIX_INIT)
