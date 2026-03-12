/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT snps_dwc_ether_qos_mdio

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mdio_qos, CONFIG_MDIO_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/util.h>

#define PHY_OPERATION_TIMEOUT_US 250000

#define MAC_MDIO_ADDRESS      0x0
#define MAC_MDIO_ADDRESS_PA   GENMASK(25, 21)
#define MAC_MDIO_ADDRESS_RDA  GENMASK(20, 16)
#define MAC_MDIO_ADDRESS_CR   GENMASK(11, 8)
#define MAC_MDIO_ADDRESS_GOC  GENMASK(3, 2)
#define MAC_MDIO_ADDRESS_C45  BIT(1)
#define MAC_MDIO_ADDRESS_BUSY BIT(0)

#define MAC_MDIO_DATA 0x4

struct mdio_qos_dev_data {
	struct k_sem sem;
	uint32_t cr;
};

struct mdio_qos_dev_config {
	uintptr_t base_addr;
	const struct pinctrl_dev_config *pinctrl;
	const struct device *clkctrl;
	uint32_t clk;
};

static int ALWAYS_INLINE qos_mdio_busy(const struct device *dev)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	return (sys_read32(cfg->base_addr + MAC_MDIO_ADDRESS) & MAC_MDIO_ADDRESS_BUSY) != 0;
}

static void ALWAYS_INLINE qos_mdio_transfer(const struct device *dev, uint8_t prtad,
						uint8_t regad, bool write, bool c45)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *data = dev->data;

	sys_write32(FIELD_PREP(MAC_MDIO_ADDRESS_CR, data->cr) |
			    FIELD_PREP(MAC_MDIO_ADDRESS_PA, prtad) |
			    FIELD_PREP(MAC_MDIO_ADDRESS_RDA, regad) |
			    FIELD_PREP(MAC_MDIO_ADDRESS_GOC, write ? 0x1 : 0x3) |
			    (c45 ? MAC_MDIO_ADDRESS_C45 : 0) | MAC_MDIO_ADDRESS_BUSY,
		    cfg->base_addr + MAC_MDIO_ADDRESS);
}

static int mdio_qos_read(const struct device *dev, uint8_t prtad, uint8_t regad, uint16_t *data)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *const dev_data = dev->data;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (qos_mdio_busy(dev)) {
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	qos_mdio_transfer(dev, prtad, regad, false, false);

	if (!WAIT_FOR(!qos_mdio_busy(dev), PHY_OPERATION_TIMEOUT_US, k_sleep(K_USEC(1000)))) {
		LOG_ERR("phy timeout");
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	*data = sys_read32(cfg->base_addr + MAC_MDIO_DATA);

	k_sem_give(&dev_data->sem);
	return 0;
}

static int mdio_qos_write(const struct device *dev, uint8_t prtad, uint8_t regad, uint16_t data)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *const dev_data = dev->data;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (qos_mdio_busy(dev)) {
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	sys_write32(data, cfg->base_addr + MAC_MDIO_DATA);
	qos_mdio_transfer(dev, prtad, regad, true, false);

	if (!WAIT_FOR(!qos_mdio_busy(dev), PHY_OPERATION_TIMEOUT_US, k_sleep(K_USEC(1000)))) {
		LOG_ERR("phy timeout");
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	k_sem_give(&dev_data->sem);
	return 0;
}

static int mdio_qos_read_c45(const struct device *dev, uint8_t prtad, uint8_t devad,
				 uint16_t regad, uint16_t *data)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *const dev_data = dev->data;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (qos_mdio_busy(dev)) {
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	sys_write32(cfg->base_addr + MAC_MDIO_DATA, (regad << 16));
	qos_mdio_transfer(dev, prtad, devad, false, true);

	if (!WAIT_FOR(!qos_mdio_busy(dev), PHY_OPERATION_TIMEOUT_US, k_sleep(K_USEC(1000)))) {
		LOG_ERR("phy timeout");
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}
	*data = sys_read32(cfg->base_addr + MAC_MDIO_DATA) & 0xFFFF;

	k_sem_give(&dev_data->sem);
	return 0;
}

static int mdio_qos_write_c45(const struct device *dev, uint8_t prtad, uint8_t devad,
				  uint16_t regad, uint16_t data)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *const dev_data = dev->data;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (qos_mdio_busy(dev)) {
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	sys_write32(cfg->base_addr + MAC_MDIO_DATA, (regad << 16) | data);
	qos_mdio_transfer(dev, prtad, devad, true, true);

	if (!WAIT_FOR(!qos_mdio_busy(dev), PHY_OPERATION_TIMEOUT_US, k_sleep(K_USEC(1000)))) {
		LOG_ERR("phy timeout");
		k_sem_give(&dev_data->sem);
		return -ETIMEDOUT;
	}

	k_sem_give(&dev_data->sem);
	return 0;
}

static inline uint32_t mdio_qos_get_cr(uint32_t rate)
{
	if (rate <= 35000000) {
		return 0x2;
	}
	if (rate <= 60000000) {
		return 0x3;
	}
	if (rate <= 100000000) {
		return 0x0;
	}
	if (rate <= 150000000) {
		return 0x1;
	}
	if (rate <= 250000000) {
		return 0x4;
	}
	if (rate <= 300000000) {
		return 0x5;
	}
	if (rate <= 500000000) {
		return 0x6;
	}
	if (rate <= 800000000) {
		return 0x7;
	}

	return 0x7;
}

static int mdio_qos_initialize(const struct device *dev)
{
	const struct mdio_qos_dev_config *const cfg = dev->config;
	struct mdio_qos_dev_data *const dev_data = dev->data;
	int ret;
	uint32_t rate;

	k_sem_init(&dev_data->sem, 1, 1);

	ret = pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret) {
		LOG_ERR("Failed to apply pinctrl state");
		return ret;
	}

	if (!device_is_ready(cfg->clkctrl)) {
		LOG_ERR("Clock control is not ready");
		return -EIO;
	}

	ret = clock_control_get_rate(cfg->clkctrl, (clock_control_subsys_t)&cfg->clk, &rate);
	if (ret) {
		LOG_ERR("Failed to receive csr clock rate");
		return ret;
	}
	dev_data->cr = mdio_qos_get_cr(rate);

	return 0;
}

static DEVICE_API(mdio, mdio_qos_driver_api) = {
	.read = mdio_qos_read,
	.write = mdio_qos_write,
	.read_c45 = mdio_qos_read_c45,
	.write_c45 = mdio_qos_write_c45,
};

#define MDIO_qos_CONFIG(n)                                                                     \
	static const struct mdio_qos_dev_config mdio_qos_dev_config_##n = {                \
		.base_addr = DT_INST_REG_ADDR(n),                                                  \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                      \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk = DT_INST_CLOCKS_CELL(n, id)};

#define MDIO_qos_DEVICE(n)                                                                     \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	MDIO_qos_CONFIG(n);                                                                    \
	static struct mdio_qos_dev_data mdio_qos_dev_data##n;                              \
	DEVICE_DT_INST_DEFINE(n, &mdio_qos_initialize, NULL, &mdio_qos_dev_data##n,        \
			      &mdio_qos_dev_config_##n, POST_KERNEL,                           \
			      CONFIG_MDIO_INIT_PRIORITY, &mdio_qos_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MDIO_qos_DEVICE)
