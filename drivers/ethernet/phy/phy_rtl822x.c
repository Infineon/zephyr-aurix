/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT realtek_rtl822x

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_rtl822x, CONFIG_PHY_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/mdio.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/util.h>
#include <string.h>
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios) || DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
#include <zephyr/drivers/gpio.h>
#endif

struct phy_rtl822x_config {
	uint8_t phy_addr;
	const struct device *const mdio;
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios)
	const struct gpio_dt_spec reset_gpio;
#endif
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
	const struct gpio_dt_spec interrupt_gpio;
#endif

	bool no_reset;
	bool serdes_auto_negotiation;
	bool fixed;
	bool eee_en;
	bool pma_local_loopback;

	uint8_t serdes_interface;
	uint8_t fixed_speed;
};

struct phy_rtl822x_data {
	const struct device *dev;
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
	struct gpio_callback gpio_callback;
#endif
	phy_callback_t cb;
	void *cb_data;
	struct k_work_delayable monitor_work;
	struct phy_link_state state;
	struct k_mutex mutex;
	k_timepoint_t an_timeout;
};

#define REALTEK_OUI_MSB                (0x1CU)
#define PHY_RTL822X_RESET_HOLD_TIME_MS 10

#define FEDCR                   0xA400
#define FEDCR_DUPLEX            BIT(8)
#define GBCR                    0xA412
#define GBCR_1000BASE_T_FULL    BIT(9)
#define GANLPAR                 0xA414
#define GANLPAR_1000BASE_T_FULL BIT(11)
#define PHYSR                   0xA434
#define PHYSR_LINK              BIT(2)
#define PHYSR_SPEED_H           GENMASK(10, 9)
#define PHYSR_SPEED_L           GENMASK(5, 4)
#define INTBCR                  0xD05C
#define INTBCR_PEMB             BIT(0)
#define INER                    0xA4D2
#define INER_LINK_STATUS_CHANGE BIT(4)
#define INSR                    0xA4D4
#define FSS                     0xA5B4

#define MMD_PMAPMD              MDIO_MMD_PMAPMD
#define MMD_PCS                 MDIO_MMD_PCS
#define MMD_AN                  MDIO_MMD_AN
#define MMD_VEND1               MDIO_MMD_VENDOR_SPECIFIC1
#define MMD_VEND2               MDIO_MMD_VENDOR_SPECIFIC2

#define PMAPMD_CONTROL1                MDIO_CTRL1
#define PMAPMD_CONTROL1_LOCAL_LOOPBACK MII_BMCR_LOOPBACK
#define PMAPMD_CONTROL1_SPEED_SELECT   MII_BMCR_SPEED_MASK

#define AN_CONTROL                     MDIO_CTRL1
#define AN_CONTROL_AN_ENABLE           MII_BMCR_AUTONEG_ENABLE
#define AN_CONTROL_RESTART_AN          MII_BMCR_AUTONEG_RESTART
#define AN_STATUS                      MDIO_STAT1
#define AN_STATUS_LINK_STATUS          BIT(2)
#define AN_STATUS_REMOTE_FAULT         BIT(4)
#define AN_STATUS_AN_COMPLETE          BIT(5)
#define AN_ADVERTISEMENT               MII_ANAR
#define AN_ADVERTISEMENT_10BASE_TE_HALF MII_ADVERTISE_10_HALF
#define AN_ADVERTISEMENT_10BASE_TE_FULL MII_ADVERTISE_10_FULL
#define AN_ADVERTISEMENT_100BASE_TX_HALF MII_ADVERTISE_100_HALF
#define AN_ADVERTISEMENT_100BASE_TX_FULL MII_ADVERTISE_100_FULL
#define MULTIG_AN_CONTROL1             0x0020
#define MULTIG_AN_CONTROL1_2_5GBASE_T  BIT(7)
#define EEE_ADVERTISEMENT              MDIO_AN_EEE_ADV
#define EEE_ADVERTISEMENT_100BASE_TX_EEE MDIO_AN_EEE_ADV_100TX
#define EEE_ADVERTISEMENT_1000BASE_T_EEE MDIO_AN_EEE_ADV_1000T
#define EEE_ADVERTISEMENT2             0x003E
#define EEE_ADVERTISEMENT2_2_5GBASE_T_EEE BIT(0)

#define PHY_LINK_IS_SPEED_2500M(x)     ((x) & LINK_FULL_2500BASE)

static inline uint32_t phy_rtl822x_get_speed_selection_bits(enum phy_link_speed speed)
{
	if (PHY_LINK_IS_SPEED_1000M(speed) || PHY_LINK_IS_SPEED_2500M(speed)) {
		return MII_BMCR_SPEED_1000;
	}

	if (PHY_LINK_IS_SPEED_100M(speed)) {
		return MII_BMCR_SPEED_100;
	}

	return MII_BMCR_SPEED_10;
}

static int phy_rtl822x_read(const struct device *dev, uint16_t reg_addr, uint32_t *value)
{
	const struct phy_rtl822x_config *const cfg = dev->config;

	return mdio_read(cfg->mdio, cfg->phy_addr, reg_addr, (uint16_t *)value);
}

static int phy_rtl822x_write(const struct device *dev, uint16_t reg_addr, uint32_t value)
{
	const struct phy_rtl822x_config *const cfg = dev->config;

	return mdio_write(cfg->mdio, cfg->phy_addr, reg_addr, value);
}

static int phy_rtl822x_read_c45(const struct device *dev, uint8_t dev_addr, uint16_t reg_addr,
				uint16_t *value)
{
	const struct phy_rtl822x_config *const cfg = dev->config;
	*value = 0;

	return mdio_read_c45(cfg->mdio, cfg->phy_addr, dev_addr, reg_addr, (uint16_t *)value);
}

static int phy_rtl822x_write_c45(const struct device *dev, uint8_t dev_addr, uint16_t reg_addr,
				 uint16_t value)
{
	const struct phy_rtl822x_config *const cfg = dev->config;

	return mdio_write_c45(cfg->mdio, cfg->phy_addr, dev_addr, reg_addr, value);
}

static int phy_rtl822x_reset(const struct device *dev)
{
	const struct phy_rtl822x_config *config = dev->config;
	uint16_t reg_val;
	int ret;
	k_timepoint_t end;

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios)
	if (config->reset_gpio.port) {
		/* Start reset */
		ret = gpio_pin_set_dt(&config->reset_gpio, 1);
		if (ret) {
			return ret;
		}

		/* Hold reset for the minimum time specified by datasheet */
		k_sleep(K_MSEC(10));

		/* Reset over */
		ret = gpio_pin_set_dt(&config->reset_gpio, 0);
		if (ret) {
			return ret;
		}

		/* Wait for t3 + t5 (5ms + 55 ms) until logic is ready */
		k_sleep(K_MSEC(60));

		goto finalize_reset;
	}
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios) */

	/* Reset PHY using register */
	ret = phy_rtl822x_write_c45(dev, MMD_PMAPMD, 0, 0x8000);
	if (ret) {
		LOG_ERR("Error writing phy (%d) basic control register", config->phy_addr);
		return ret;
	}

	/* Wait for the minimum reset time specified by datasheet */
	k_busy_wait(USEC_PER_MSEC * PHY_RTL822X_RESET_HOLD_TIME_MS);

	/* Wait for the reset to be cleared */
	end = sys_timepoint_calc(K_MSEC(10));
	do {
		ret = phy_rtl822x_read_c45(dev, MMD_PMAPMD, 0, &reg_val);
		if (ret) {
			LOG_ERR("Error reading phy (%d) basic control register", config->phy_addr);
			return ret;
		}
	} while (reg_val & 0x8000 && !sys_timepoint_expired(end));

	if (reg_val & 0x8000) {
		return -EIO;
	}

	goto finalize_reset;

finalize_reset:
	/* Wait until correct data can be read from registers */
	end = sys_timepoint_calc(K_MSEC(10));
	do {
		ret = phy_rtl822x_read_c45(dev, MMD_PMAPMD, 0x2, &reg_val);
		if (ret) {
			LOG_ERR("%s: Error reading phy identifier register 1", dev->name);
			return ret;
		}
	} while (reg_val != REALTEK_OUI_MSB && !sys_timepoint_expired(end));

	if (reg_val != REALTEK_OUI_MSB) {
		return -EIO;
	}

	return 0;
}

static inline int phy_rtl822x_serdes_enable(const struct device *dev)
{
	const struct phy_rtl822x_config *const cfg = dev->config;
	uint16_t reg_val;

	/* Enable serdes*/
	if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x75f3, 0) < 0) {
		return -EIO;
	}
	/* Select serdes interface*/
	if (phy_rtl822x_read_c45(dev, MMD_VEND1, 0x697A, &reg_val)) {
		return -EIO;
	}
	reg_val = (reg_val & ~0x3) | cfg->serdes_interface;
	if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x697A, reg_val)) {
		return -EIO;
	}

	if (cfg->serdes_interface == 0 || cfg->serdes_interface == 2) {
		/* Enable SGMII + 2500Base-X Serdes*/
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6a04, 0x0503)) {
			return -EIO;
		}
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6f10, 0xd455)) {
			return -EIO;
		}
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6f11, 0x8020)) {
			return -EIO;
		}
	} else {
		/* Enable SGMII + HiSGMII*/
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6a04, 0x0503)) {
			return -EIO;
		}
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6f10, 0xd433)) {
			return -EIO;
		}
		if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x6f11, 0x8020)) {
			return -EIO;
		}
	}

	if (phy_rtl822x_read_c45(dev, MMD_VEND2, 0xA400, &reg_val)) {
		return -EIO;
	}
	reg_val |= 0x4000;
	if (phy_rtl822x_write_c45(dev, MMD_VEND2, 0xA400, reg_val)) {
		return -EIO;
	}

	uint32_t timeout = 1000;
	do {
		if (phy_rtl822x_read_c45(dev, MMD_VEND2, 0xA434, &reg_val)) {
			return -EIO;
		}
		if (reg_val & 0x4) {
			break;
		}
		k_sleep(K_MSEC(1));
	} while (timeout > 0);

	if (phy_rtl822x_read_c45(dev, MMD_VEND2, 0xA400, &reg_val)) {
		return -EIO;
	}
	reg_val &= ~0x4000;
	if (phy_rtl822x_write_c45(dev, MMD_VEND2, 0xA400, reg_val)) {
		return -EIO;
	}

	return 0;
}

static inline int phy_rtl822x_serdes_set_an(const struct device *dev)
{
	const struct phy_rtl822x_config *const cfg = dev->config;
	uint16_t an_reg;

	if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x7588, 0x0002) < 0) {
		return -EIO;
	}

	if (cfg->serdes_auto_negotiation) {
		an_reg = 0x70D0;
	} else {
		an_reg = 0x71D0;
	}

	if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x7589, an_reg) < 0) {
		return -EIO;
	}
	if (phy_rtl822x_write_c45(dev, MMD_VEND1, 0x7587, 0x0003) < 0) {
		return -EIO;
	}

	return 0;
}

static inline int phy_rtl822x_force_speed(const struct device *dev, enum phy_link_speed speed)
{
	uint16_t reg_val;

	/* Force speed mode */
	if (phy_rtl822x_write_c45(dev, MMD_VEND2, FSS, 0x8000) < 0) {
		return -EIO;
	}

	/* Set PMA speed */
	if (phy_rtl822x_read_c45(dev, MMD_PMAPMD, PMAPMD_CONTROL1, &reg_val) < 0) {
		return -EIO;
	}
	reg_val = (reg_val & ~PMAPMD_CONTROL1_SPEED_SELECT) |
		  phy_rtl822x_get_speed_selection_bits(speed);
	if (phy_rtl822x_write_c45(dev, MMD_PMAPMD, PMAPMD_CONTROL1, reg_val) < 0) {
		return -EIO;
	}

	/* Disable auto-negotiation */
	if (phy_rtl822x_read_c45(dev, MMD_AN, AN_CONTROL, &reg_val) < 0) {
		return -EIO;
	}
	reg_val &= ~AN_CONTROL_AN_ENABLE;
	if (phy_rtl822x_write_c45(dev, MMD_AN, AN_CONTROL, reg_val) < 0) {
		return -EIO;
	}

	return 0;
}

static inline int phy_rtl822x_an_configure(const struct device *dev, enum phy_link_speed speed)
{
	const struct phy_rtl822x_config *const cfg = dev->config;
	struct phy_rtl822x_data *const data = dev->data;
	uint16_t reg_val, an_ctrl;

	/* Configure EEE Advertisement */
	reg_val = cfg->eee_en ? EEE_ADVERTISEMENT_100BASE_TX_EEE | EEE_ADVERTISEMENT_1000BASE_T_EEE
			      : 0;
	if (phy_rtl822x_write_c45(dev, MMD_AN, EEE_ADVERTISEMENT, reg_val) < 0) {
		return -EIO;
	}
	reg_val = cfg->eee_en ? EEE_ADVERTISEMENT2_2_5GBASE_T_EEE : 0;
	if (phy_rtl822x_write_c45(dev, MMD_AN, EEE_ADVERTISEMENT2, reg_val) < 0) {
		return -EIO;
	}

	/* Configure speed advertisement */
	if (phy_rtl822x_read_c45(dev, MMD_AN, AN_ADVERTISEMENT, &reg_val) < 0) {
		return -EIO;
	}
	reg_val &= ~(AN_ADVERTISEMENT_10BASE_TE_HALF | AN_ADVERTISEMENT_10BASE_TE_FULL |
		     AN_ADVERTISEMENT_100BASE_TX_FULL | AN_ADVERTISEMENT_100BASE_TX_HALF);
	reg_val |= (speed & LINK_HALF_10BASE ? AN_ADVERTISEMENT_10BASE_TE_HALF : 0) |
		   (speed & LINK_FULL_10BASE ? AN_ADVERTISEMENT_10BASE_TE_FULL : 0) |
		   (speed & LINK_HALF_100BASE ? AN_ADVERTISEMENT_100BASE_TX_HALF : 0) |
		   (speed & LINK_FULL_100BASE ? AN_ADVERTISEMENT_100BASE_TX_FULL : 0);
	if (phy_rtl822x_write_c45(dev, MMD_AN, AN_ADVERTISEMENT, reg_val) < 0) {
		return -EIO;
	}

	if (phy_rtl822x_read_c45(dev, MMD_AN, MULTIG_AN_CONTROL1, &reg_val) < 0) {
		return -EIO;
	}
	reg_val &= ~MULTIG_AN_CONTROL1_2_5GBASE_T;
	reg_val |= (speed & LINK_FULL_2500BASE ? MULTIG_AN_CONTROL1_2_5GBASE_T : 0);
	if (phy_rtl822x_write_c45(dev, MMD_AN, MULTIG_AN_CONTROL1, reg_val) < 0) {
		return -EIO;
	}

	if (phy_rtl822x_read_c45(dev, MMD_VEND2, GBCR, &reg_val) < 0) {
		return -EIO;
	}
	reg_val &= ~GBCR_1000BASE_T_FULL;
	reg_val |= (speed & LINK_FULL_1000BASE ? GBCR_1000BASE_T_FULL : 0);
	if (phy_rtl822x_write_c45(dev, MMD_VEND2, GBCR, reg_val) < 0) {
		return -EIO;
	}

	/* Enable and restart auto-negotiation */
	if (phy_rtl822x_read_c45(dev, MMD_AN, AN_CONTROL, &an_ctrl) < 0) {
		return -EIO;
	}
	an_ctrl |= AN_CONTROL_AN_ENABLE | AN_CONTROL_RESTART_AN;

	if (phy_rtl822x_write_c45(dev, MMD_AN, AN_CONTROL, an_ctrl) < 0) {
		return -EIO;
	}
	data->an_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	data->state.is_up = false;

	return 0;
}

static inline int phy_rtl822x_an_is_complete(const struct device *dev)
{
	struct phy_rtl822x_data *const data = dev->data;
	uint16_t an_sts = 0;

	if (phy_rtl822x_read_c45(dev, MMD_AN, AN_STATUS, &an_sts) < 0) {
		LOG_ERR("%s: failed to get autonegotiation status", dev->name);
		return -EIO;
	}
	if (an_sts & AN_STATUS_AN_COMPLETE) {
		LOG_DBG("%s: auto-negotiation complete", dev->name);
		return 1;
	}
	if (sys_timepoint_expired(data->an_timeout)) {
		LOG_DBG("%s: auto-negotiation timed out", dev->name);
		data->an_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	}

	return 0;
}

static int phy_rtl822x_update_link_state(const struct device *dev)
{
	struct phy_rtl822x_data *const data = dev->data;
	struct phy_link_state new_state;
	uint16_t physr, fedcr;

	/* Read Phy registers to determine speed and duplex state */
	if (phy_rtl822x_read_c45(dev, MMD_VEND2, PHYSR, &physr) < 0) {
		return -EIO;
	}
	if (phy_rtl822x_read_c45(dev, MMD_VEND2, FEDCR, &fedcr) < 0) {
		return -EIO;
	}
	new_state.is_up = (physr & PHYSR_LINK) != 0;

	if (new_state.is_up == true) {
		bool full_duplex = (fedcr & FEDCR_DUPLEX) != 0;
		uint32_t speed =
			(FIELD_GET(PHYSR_SPEED_H, physr) << 2) | FIELD_GET(PHYSR_SPEED_L, physr);
		switch (speed) {
		case 0:
			new_state.speed = full_duplex ? LINK_FULL_10BASE : LINK_HALF_10BASE;
			break;
		case 1:
			new_state.speed = full_duplex ? LINK_FULL_100BASE : LINK_HALF_100BASE;
			break;
		case 2:
			new_state.speed = LINK_FULL_1000BASE;
			break;
		case 5:
		case 7:
			new_state.speed = LINK_FULL_2500BASE;
			break;
		}
	}

	if (memcmp(&new_state, &data->state, sizeof(struct phy_link_state)) != 0) {
		data->state = new_state;
		if (new_state.is_up) {
			LOG_INF("%s: Link is up", dev->name);
			LOG_INF("%s: Link speed %s Mb, %s duplex\n", dev->name,
				PHY_LINK_IS_SPEED_2500M(new_state.speed)
					? "2500"
					: (PHY_LINK_IS_SPEED_1000M(new_state.speed)
						   ? "1000"
						   : (PHY_LINK_IS_SPEED_100M(new_state.speed)
							      ? "100"
							      : "10")),
				PHY_LINK_IS_FULL_DUPLEX(new_state.speed) ? "full" : "half");
		} else {
			LOG_INF("%s: Link is down", dev->name);
		}
		return 1;
	}

	return 0;
}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
static int phy_rtl822x_get_interrupts(const struct device *const dev, uint16_t *irqs)
{
	struct phy_rtl822x_data *const data = dev->data;
	int ret;

	/* Lock mutex */
	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY mutex lock error");
		return ret;
	}

	/* Read/clear PHY interrupt status register */
	ret = phy_rtl822x_read_c45(dev, MMD_VEND2, INSR, irqs);
	if (ret) {
		LOG_ERR("%s: Error reading interrupt status register", dev->name);
	}

	/* Unlock mutex */
	(void)k_mutex_unlock(&data->mutex);

	return ret;
}

static void phy_rtl822x_interrupt_handler(const struct device *port, struct gpio_callback *cb,
					  gpio_port_pins_t pins)
{
	struct phy_rtl822x_data *const data =
		CONTAINER_OF(cb, struct phy_rtl822x_data, gpio_callback);
	const struct phy_rtl822x_config *const config = data->dev->config;
	int ret;

	if (pins & (1 << config->interrupt_gpio.pin)) {
		ret = k_work_reschedule(&data->monitor_work, K_NO_WAIT);
		if (ret < 0) {
			LOG_ERR("%s: Failed to schedule monitor_work from ISR", data->dev->name);
		}
	}
}
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios) */

static void phy_rtl822x_monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct phy_rtl822x_data *const data =
		CONTAINER_OF(dwork, struct phy_rtl822x_data, monitor_work);
	const struct device *dev = data->dev;
	const struct phy_rtl822x_config *const config = dev->config;
	int rc = 0;

	k_mutex_lock(&data->mutex, K_FOREVER);

#if CONFIG_PHY_LOG_LEVEL == LOG_LEVEL_DBG
	if (data->state.is_up == false && sys_timepoint_expired(data->an_timeout)) {
		uint32_t reg_val;

		if (phy_rtl822x_read_c45(dev, MMD_PMAPMD, PMAPMD_STATUS1, &reg_val) == 0) {
			LOG_DBG("%s: PMA State Link: %lu Fault: %lu", dev->name,
				FIELD_GET(PMAPMD_STATUS1_LINK_STATUS, reg_val),
				FIELD_GET(PMAPMD_STATUS1_FAULT, reg_val));
		}
		if (phy_rtl822x_read_c45(dev, MMD_PCS, PCS_STATUS1, &reg_val) == 0) {
			LOG_DBG("%s: PCS State Link: %lu Fault: %lu", dev->name,
				FIELD_GET(PCS_STATUS1_LINK_STATUS, reg_val),
				FIELD_GET(PCS_STATUS1_FAULT, reg_val));
		}
		if (phy_rtl822x_read_c45(dev, MMD_AN, AN_STATUS, &reg_val) == 0) {
			LOG_DBG("%s: AN State Link: %lu Remote Fault: %lu", dev->name,
				FIELD_GET(AN_STATUS_LINK_STATUS, reg_val),
				FIELD_GET(AN_STATUS_REMOTE_FAULT, reg_val));
		}
	}
#endif

	/* Check if on going auto negotiation */
	if (data->state.is_up || config->fixed || phy_rtl822x_an_is_complete(dev)) {
		rc = phy_rtl822x_update_link_state(dev);
		if (rc < 0) {
			LOG_ERR("%s: Failed to get link state", dev->name);
		}
	}

	k_mutex_unlock(&data->mutex);

	/* If link state has changed and a callback is set, invoke callback
	 */
	if (rc == 1) {
		/* Invoke callback */
		if (data->cb) {
			data->cb(dev, &data->state, data->cb_data);
		}
		if (!data->state.is_up) {
			data->an_timeout =
				sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
		}
	}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
	if (config->interrupt_gpio.port) {
		/* Clear irqs by reading */
		uint16_t irqs;
		phy_rtl822x_get_interrupts(dev, &irqs);
	} else
#endif
	{
		k_work_reschedule(&data->monitor_work, data->state.is_up
							       ? K_MSEC(CONFIG_PHY_MONITOR_PERIOD)
							       : K_MSEC(10));
	}
}

static int phy_rtl822x_cfg_link(const struct device *dev, enum phy_link_speed adv_speeds,
				enum phy_cfg_link_flag flags)
{

	const struct phy_rtl822x_config *const config = dev->config;
	struct phy_rtl822x_data *const data = dev->data;

	if (flags & PHY_FLAG_AUTO_NEGOTIATION_DISABLED) {
		return -ENOTSUP;
	}

	if (config->fixed) {
		return -ENOSYS;
	}

	k_mutex_lock(&data->mutex, K_FOREVER);

	if (phy_rtl822x_an_configure(dev, adv_speeds) < 0) {
		return -EIO;
	}

	k_mutex_unlock(&data->mutex);

	return 0;
}

static int phy_rtl822x_get_link_state(const struct device *dev, struct phy_link_state *state)
{
	struct phy_rtl822x_data *const data = dev->data;

	k_mutex_lock(&data->mutex, K_FOREVER);

	memcpy(state, &data->state, sizeof(struct phy_link_state));

	k_mutex_unlock(&data->mutex);

	return 0;
}

static int phy_rtl822x_link_cb_set(const struct device *dev, phy_callback_t cb, void *user_data)
{
	struct phy_rtl822x_data *const data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	if (cb != NULL) {
		cb(dev, &data->state, user_data);
	}

	return 0;
}

static int phy_rtl822x_initialize(const struct device *dev)
{
	const struct phy_rtl822x_config *config = dev->config;
	struct phy_rtl822x_data *data = dev->data;
	int ret;

	data->dev = dev;

	ret = k_mutex_init(&data->mutex);
	if (ret) {
		return ret;
	}

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios)
	/* Configure reset pin */
	if (config->reset_gpio.port) {
		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios) */

	if (!config->no_reset) {
		/* Reset PHY */
		ret = phy_rtl822x_reset(dev);
		if (ret) {
			LOG_ERR("%s: Failed to reset phy", dev->name);
			return ret;
		}
	}

	/* Enable SERDES interface with settings */
	if (phy_rtl822x_serdes_enable(dev) < 0) {
		LOG_ERR("%s: Failed to configure SERDES interface", dev->name);
		return -EIO;
	}
	phy_rtl822x_serdes_set_an(dev);

	if (config->pma_local_loopback) {
		uint16_t pma_ctrl;
		ret = phy_rtl822x_read_c45(dev, MMD_PMAPMD, PMAPMD_CONTROL1, &pma_ctrl);
		pma_ctrl |= PMAPMD_CONTROL1_LOCAL_LOOPBACK;
		ret |= phy_rtl822x_write_c45(dev, MMD_PMAPMD, PMAPMD_CONTROL1, pma_ctrl);
		if (ret) {
			LOG_ERR("%s: Error writing local loopback", dev->name);
			return ret;
		}
	}

	/* Set speed configuration */
	if (config->fixed) {
		if (config->pma_local_loopback) {
			phy_rtl822x_force_speed(dev, BIT(config->fixed_speed));
		} else {
			phy_rtl822x_an_configure(dev, BIT(config->fixed_speed));
		}
	} else {
		phy_rtl822x_an_configure(dev, LINK_HALF_10BASE | LINK_FULL_10BASE |
					      LINK_HALF_100BASE | LINK_FULL_100BASE |
					      LINK_FULL_1000BASE | LINK_FULL_2500BASE);
	}

	k_work_init_delayable(&data->monitor_work, phy_rtl822x_monitor_work_handler);

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
	if (!config->interrupt_gpio.port) {
		phy_rtl822x_monitor_work_handler(&data->monitor_work.work);
		goto skip_int_gpio;
	}

	/* Set INTB/PMEB pin to interrupt mode */
	ret = phy_rtl822x_write_c45(dev, MMD_VEND2, INTBCR, 0);
	if (ret) {
		LOG_ERR("%s: Error writing interrupt pin setting", dev->name);
		return ret;
	}

	/* Enable PHY interrupt. */
	ret = phy_rtl822x_write_c45(dev, MMD_VEND2, INER, INER_LINK_STATUS_CHANGE);
	if (ret) {
		LOG_ERR("%s: Error writing interrutp mask", dev->name);
		return ret;
	}

	/* Configure interrupt pin */
	ret = gpio_pin_configure_dt(&config->interrupt_gpio, GPIO_INPUT);
	if (ret) {
		return ret;
	}

	gpio_init_callback(&data->gpio_callback, phy_rtl822x_interrupt_handler,
			   BIT(config->interrupt_gpio.pin));
	ret = gpio_add_callback_dt(&config->interrupt_gpio, &data->gpio_callback);
	if (ret) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&config->interrupt_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		return ret;
	}

	/* Clear irqs by reading */
	uint16_t irqs;
	ret = phy_rtl822x_get_interrupts(dev, &irqs);
	if (ret < 0) {
		LOG_ERR("%s: Failed to read irq status", dev->name);
		return -EIO;
	}

	if ((irqs & INER_LINK_STATUS_CHANGE)) {
		phy_rtl822x_monitor_work_handler(&data->monitor_work.work);
	}

skip_int_gpio:
#else
	phy_rtl822x_monitor_work_handler(&data->monitor_work.work);
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios) */

	return 0;
}

static const struct ethphy_driver_api phy_rtl822x_driver_api = {
	.get_link = phy_rtl822x_get_link_state,
	.cfg_link = phy_rtl822x_cfg_link,
	.link_cb_set = phy_rtl822x_link_cb_set,
	.read = phy_rtl822x_read,
	.write = phy_rtl822x_write,
	.read_c45 = phy_rtl822x_read_c45,
	.write_c45 = phy_rtl822x_write_c45,
};

#define IS_FIXED_LINK(n) DT_INST_NODE_HAS_PROP(n, fixed_link)

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios)
#define RESET_GPIO(n) .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),
#else
#define RESET_GPIO(n)
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios) */

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios)
#define INTERRUPT_GPIO(n) .interrupt_gpio = GPIO_DT_SPEC_INST_GET_OR(n, int_gpios, {0}),
#else
#define INTERRUPT_GPIO(n)
#endif /* DT_ANY_INST_HAS_PROP_STATUS_OKAY(int_gpios) */

#define PHY_RTL822X_CONFIG(n)                                                                      \
	static const struct phy_rtl822x_config phy_rtl822x_config_##n = {                          \
		.mdio = DEVICE_DT_GET(DT_INST_BUS(n)),                                             \
		.phy_addr = DT_INST_REG_ADDR(n),                                                   \
		.no_reset = DT_INST_PROP(n, no_reset),                                             \
		.serdes_interface = DT_INST_ENUM_IDX_OR(n, realtek_serdes_interface, 3),           \
		.serdes_auto_negotiation = DT_INST_PROP(n, realtek_serdes_auto_negotiation),       \
		.fixed = IS_FIXED_LINK(n),                                                         \
		.fixed_speed = DT_INST_ENUM_IDX_OR(n, fixed_link, 0),                              \
		.pma_local_loopback = DT_INST_PROP(n, pma_local_loopback),                         \
		RESET_GPIO(n) INTERRUPT_GPIO(n)};

#define PHY_RTL822X_DEVICE(n)                                                                      \
	PHY_RTL822X_CONFIG(n);                                                                     \
	static struct phy_rtl822x_data phy_rtl822x_data_##n;                                       \
	DEVICE_DT_INST_DEFINE(n, &phy_rtl822x_initialize, NULL, &phy_rtl822x_data_##n,             \
			      &phy_rtl822x_config_##n, POST_KERNEL, CONFIG_PHY_INIT_PRIORITY,      \
			      &phy_rtl822x_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PHY_RTL822X_DEVICE)
