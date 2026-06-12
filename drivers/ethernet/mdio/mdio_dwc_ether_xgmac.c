/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT snps_dwc_ether_xgmac_mdio

#include <errno.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mdio_xgmac, CONFIG_MDIO_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define MDIO_SINGLE_COMMAND_ADDRESS            0x200
#define MDIO_SINGLE_COMMAND_ADDRESS_RA         GENMASK(15, 0)
#define MDIO_SINGLE_COMMAND_ADDRESS_PA         GENMASK(20, 16)
#define MDIO_SINGLE_COMMAND_ADDRESS_DA         GENMASK(25, 21)
#define MDIO_SINGLE_COMMAND_CONTROL_DATA       0x204
#define MDIO_SINGLE_COMMAND_CONTROL_DATA_SDATA GENMASK(15, 0)
#define MDIO_SINGLE_COMMAND_CONTROL_DATA_CMD   GENMASK(17, 16)
#define MDIO_SINGLE_COMMAND_CONTROL_DATA_CR    GENMASK(21, 19)
#define MDIO_SINGLE_COMMAND_CONTROL_DATA_SBUSY BIT(22)
#define MDIO_CLAUSE_22_PORT                    0x220

struct mdio_xgmac_dev_data {
	struct k_sem sem;
	uint32_t cr;
	uint32_t c45;
};

struct mdio_xgmac_dev_config {
	uintptr_t base_addr;
	const struct pinctrl_dev_config *pinctrl;
	const struct device *clkctrl;
	uint32_t clk;
};

static bool ALWAYS_INLINE mdio_xgmac_busy(const struct device *dev)
{
	const struct mdio_xgmac_dev_config *const cfg = dev->config;

	return (sys_read32(cfg->base_addr + MDIO_SINGLE_COMMAND_CONTROL_DATA) &
		MDIO_SINGLE_COMMAND_CONTROL_DATA_SBUSY) != 0;
}

static inline int mdio_xgmac_wait_done(const struct device *dev)
{
	k_sleep(K_USEC(25));
	if (mdio_xgmac_busy(dev)) {
		return -ETIMEDOUT;
	}

	return 0;
}

static void ALWAYS_INLINE mdio_xgmac_transfer(const struct device *dev, uint8_t prtad,
					      uint8_t devad, uint16_t regad, bool write,
					      uint16_t wdata)
{
	const struct mdio_xgmac_dev_config *const cfg = dev->config;
	struct mdio_xgmac_dev_data *data = dev->data;

	sys_write32(FIELD_PREP(MDIO_SINGLE_COMMAND_ADDRESS_PA, prtad) |
			    FIELD_PREP(MDIO_SINGLE_COMMAND_ADDRESS_DA, devad) |
			    FIELD_PREP(MDIO_SINGLE_COMMAND_ADDRESS_RA, regad),
		    cfg->base_addr + MDIO_SINGLE_COMMAND_ADDRESS);
	sys_write32(FIELD_PREP(MDIO_SINGLE_COMMAND_CONTROL_DATA_CR, data->cr) |
			    FIELD_PREP(MDIO_SINGLE_COMMAND_CONTROL_DATA_CMD, write ? 0x1 : 0x3) |
			    FIELD_PREP(MDIO_SINGLE_COMMAND_CONTROL_DATA_SDATA, wdata) |
			    MDIO_SINGLE_COMMAND_CONTROL_DATA_SBUSY,
		    cfg->base_addr + MDIO_SINGLE_COMMAND_CONTROL_DATA);
}

static void mdio_xgmac_set_c45(const struct device *dev, uint8_t prtad, bool c45)
{
	const struct mdio_xgmac_dev_config *const cfg = dev->config;
	struct mdio_xgmac_dev_data *const dev_data = dev->data;

	if (c45 && ((dev_data->c45 & BIT(prtad)) == 0)) {
		sys_write32(sys_read32(cfg->base_addr + MDIO_CLAUSE_22_PORT) & ~BIT(prtad),
			    cfg->base_addr + MDIO_CLAUSE_22_PORT);
		dev_data->c45 |= BIT(prtad);
	}
	if (!c45 && ((dev_data->c45 & BIT(prtad)) != 0)) {
		sys_write32(sys_read32(cfg->base_addr + MDIO_CLAUSE_22_PORT) | BIT(prtad),
			    cfg->base_addr + MDIO_CLAUSE_22_PORT);
		dev_data->c45 &= ~BIT(prtad);
	}
}

static inline void mdio_xgmac_get_data(const struct device *dev, uint16_t *data)
{
	const struct mdio_xgmac_dev_config *const cfg = dev->config;

	*data = (sys_read32(cfg->base_addr + MDIO_SINGLE_COMMAND_CONTROL_DATA) &
		 MDIO_SINGLE_COMMAND_CONTROL_DATA_SDATA);
}

static int mdio_xgmac_read(const struct device *dev, uint8_t prtad, uint8_t regad, uint16_t *data)
{
	struct mdio_xgmac_dev_data *const dev_data = dev->data;
	int ret = 0;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (mdio_xgmac_busy(dev)) {
		LOG_ERR("%s: mdio still busy", dev->name);
		ret = -EIO;
		goto out;
	}

	mdio_xgmac_set_c45(dev, prtad, false);
	mdio_xgmac_transfer(dev, prtad, 0, regad, false, 0);
	if ((ret = mdio_xgmac_wait_done(dev))) {
		LOG_ERR("%s: mdio operation timed out", dev->name);
		goto out;
	}
	mdio_xgmac_get_data(dev, data);

out:
	k_sem_give(&dev_data->sem);
	return ret;
}

static int mdio_xgmac_write(const struct device *dev, uint8_t prtad, uint8_t regad, uint16_t data)
{
	struct mdio_xgmac_dev_data *const dev_data = dev->data;
	int ret = 0;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (mdio_xgmac_busy(dev)) {
		LOG_ERR("%s: mdio still busy", dev->name);
		ret = -EIO;
		goto out;
	}

	mdio_xgmac_set_c45(dev, prtad, false);
	mdio_xgmac_transfer(dev, prtad, 0, regad, true, data);
	if ((ret = mdio_xgmac_wait_done(dev))) {
		LOG_ERR("%s: mdio operation timed out", dev->name);
		goto out;
	}

out:
	k_sem_give(&dev_data->sem);
	return ret;
}

static int mdio_xgmac_read_c45(const struct device *dev, uint8_t prtad, uint8_t devad,
			       uint16_t regad, uint16_t *data)
{
	struct mdio_xgmac_dev_data *const dev_data = dev->data;
	int ret = 0;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (mdio_xgmac_busy(dev)) {
		LOG_ERR("%s: mdio still busy", dev->name);
		ret = -EIO;
		goto out;
	}

	mdio_xgmac_set_c45(dev, prtad, true);
	mdio_xgmac_transfer(dev, prtad, devad, regad, false, 0);
	if ((ret = mdio_xgmac_wait_done(dev))) {
		LOG_ERR("%s: mdio operation timed out", dev->name);
		goto out;
	}
	mdio_xgmac_get_data(dev, data);

out:
	k_sem_give(&dev_data->sem);
	return ret;
}

static int mdio_xgmac_write_c45(const struct device *dev, uint8_t prtad, uint8_t devad,
				uint16_t regad, uint16_t data)
{
	struct mdio_xgmac_dev_data *const dev_data = dev->data;
	int ret = 0;

	k_sem_take(&dev_data->sem, K_FOREVER);

	if (mdio_xgmac_busy(dev)) {
		LOG_ERR("%s: mdio still busy", dev->name);
		ret = -EIO;
		goto out;
	}

	mdio_xgmac_set_c45(dev, prtad, true);
	mdio_xgmac_transfer(dev, prtad, devad, regad, true, data);
	if ((ret = mdio_xgmac_wait_done(dev))) {
		LOG_ERR("%s: mdio operation timed out", dev->name);
		goto out;
	}

out:
	k_sem_give(&dev_data->sem);
	return ret;
}

static inline uint32_t mdio_xgmac_get_cr(uint32_t rate)
{
	if (rate <= 150000000) {
		return 0x0;
	}
	if (rate <= 250000000) {
		return 0x1;
	}
	if (rate <= 300000000) {
		return 0x2;
	}
	if (rate <= 350000000) {
		return 0x3;
	}
	if (rate <= 400000000) {
		return 0x4;
	}
	if (rate <= 500000000) {
		return 0x5;
	}
	if (rate <= 700000000) {
		return 0x6;
	}
	if (rate <= 900000000) {
		return 0x7;
	}

	return 0x7;
}

static int __maybe_unused mdio_xgmac_initialize(const struct device *dev)
{
	const struct mdio_xgmac_dev_config *const cfg = dev->config;
	struct mdio_xgmac_dev_data *const dev_data = dev->data;
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
	dev_data->cr = mdio_xgmac_get_cr(rate);

	return 0;
}

static DEVICE_API(mdio, mdio_xgmac_driver_api) = {
	.read = mdio_xgmac_read,
	.write = mdio_xgmac_write,
	.read_c45 = mdio_xgmac_read_c45,
	.write_c45 = mdio_xgmac_write_c45,
};

#define MDIO_DWC_ETHER_XGMAC_CONFIG(n)                                                            \
	static const struct mdio_xgmac_dev_config mdio_xgmac_dev_config_##n = {                    \
		.base_addr = DT_INST_REG_ADDR(n) - 0x200,                                           \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                   \
		.clk = DT_INST_CLOCKS_CELL(n, id)};

#define MDIO_DWC_ETHER_XGMAC_DEVICE(n)                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	MDIO_DWC_ETHER_XGMAC_CONFIG(n);                                                             \
	static struct mdio_xgmac_dev_data mdio_xgmac_dev_data##n;                                   \
	DEVICE_DT_INST_DEFINE(n, &mdio_xgmac_initialize, NULL, &mdio_xgmac_dev_data##n,            \
			      &mdio_xgmac_dev_config_##n, POST_KERNEL, CONFIG_MDIO_INIT_PRIORITY,   \
			      &mdio_xgmac_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MDIO_DWC_ETHER_XGMAC_DEVICE)
