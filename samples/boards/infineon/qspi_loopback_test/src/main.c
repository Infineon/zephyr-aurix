/*
 * Copyright (c) 2026 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>

#define SPI_NODE DT_ALIAS(test_qspi)

static const struct device *spi_dev = DEVICE_DT_GET(SPI_NODE);

int main(void)
{
	if (!device_is_ready(spi_dev)) {
		printk("SPI device not ready\n");
		return 0;
	}

	uint8_t tx_data[] = "Hello QSPI ";
	struct spi_buf tx_buf = {
		.buf = tx_data,
		.len = sizeof(tx_data),
	};
	struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};

	uint8_t rx_data[sizeof(tx_data)] = {0};
	struct spi_buf rx_buf = {
		.buf = rx_data,
		.len = sizeof(rx_data),
	};
	struct spi_buf_set rx = {
		.buffers = &rx_buf,
		.count = 1,
	};

	struct spi_config spi_cfg = {
		.frequency = 1000000,
		.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_LOOP,
		.slave = 0,
		.cs = NULL,
	};

	int ret = spi_transceive(spi_dev, &spi_cfg, &tx, &rx);

	if (ret == 0) {
		printk("SPI transfer done\n");
	} else {
		printk("SPI error: %d\n", ret);
	}

	for (int j = 0; j < (int)sizeof(rx_data); j++) {
		printk("%c\n", rx_data[j]);
	}

	return 0;
}
