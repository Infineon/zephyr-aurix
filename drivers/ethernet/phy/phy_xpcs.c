/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT snps_xpcs

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_xpcs, CONFIG_PHY_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mdio.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/util.h>

#include "IfxHsphy_reg.h"

#define VR_XS_PCS_DIG_CTRL1              0x8000
#define VR_XS_PCS_DIG_CTRL1_EN_2_5G_MODE BIT(2)

#define PMAPMD_STATUS1             MDIO_STAT1
#define PMAPMD_STATUS1_LINK_STATUS BIT(2)
#define PMAPMD_STATUS1_FAULT       BIT(7)
#define PCS_STATUS1                MDIO_STAT1
#define PCS_STATUS1_LINK_STATUS    BIT(2)
#define PCS_STATUS1_FAULT          BIT(7)
#define PHY_LINK_IS_SPEED_2500M(x) ((x) & LINK_FULL_2500BASE)
#define PHY_LINK_IS_SPEED_5000M(x) ((x) & LINK_FULL_5000BASE)
#define SR_MII_CTRL                0x0
#define SR_MII_STS                 0x1
#define SR_MII_STS_LINK_UP         BIT(2)
#define SR_MII_STS_REMOTE_FAULT    BIT(4)
#define SR_MII_STS_AN_COMPLETE     BIT(5)
#define VR_MII_AN_CTRL             0x8001
#define VR_MII_AN_CTRL_PCS_MODE    GENMASK(2, 1)
#define VR_MII_AN_INTR_STS         0x8002
#define MII_AN_ADV                 0x4
#define MII_AN_ADV_RF              GENMASK(13, 12)
#define MII_AN_ADV_PAUSE           GENMASK(8, 7)
#define MII_AN_ADV_HD              BIT(6)
#define MII_AN_ADV_FD              BIT(5)

#define MII_CONTROL             0x0
#define MII_CONTROL_SS13        BIT(13)
#define MII_CONTROL_RESTART_AN  BIT(9)
#define MII_CONTROL_DUPLEX_MODE BIT(8)
#define MII_CONTROL_SS6         BIT(6)
#define MII_CONTROL_SS5         BIT(5)

#define MII_STATUS             0x1
#define MII_STATUS_EXT_STS_ABL BIT(8)
#define MII_STATUS_UN_DIR_ABL  BIT(7)
#define MII_STATUS_MF_PRE_SUP  BIT(6)
#define MII_STATUS_AN_CMPL     BIT(5)
#define MII_STATUS_RF          BIT(4)
#define MII_STATUS_AN_ABL      BIT(3)
#define MII_STATUS_LINK_STS    BIT(2)
#define MII_STATUS_EXT_REG_CAP BIT(0)

enum phy_xpcs_pcs {
	XPCS_PCS_SGMII,
	XPCS_PCS_SGMII_2_5G,
	XPCS_PCS_USXGMII_5G
};

struct phy_xpcs_config {
	void *base_addr;
	const struct device *const uplink;
	bool fixed;
	enum phy_link_speed fixed_speed;
};

struct phy_xpcs_data {
	phy_callback_t cb;
	void *user_data;

	struct phy_link_state state;
	struct k_work_delayable monitor_work;
	struct k_mutex mutex;
	const struct device *dev;
	k_timepoint_t an_timeout;

	enum phy_xpcs_pcs pcs;
};

static int phy_xpcs_set_speed(const struct device *dev, enum phy_link_speed speed);
static int phy_xpcs_set_sgmii(const struct device *dev, uint8_t speed);
static int phy_xpcs_set_sgmii2_5(const struct device *dev);

static int phy_xpcs_read(const struct device *dev, uint16_t reg_addr, uint32_t *data)
{
	return -ENOSYS;
}

static int phy_xpcs_write(const struct device *dev, uint16_t reg_addr, uint32_t data)
{
	return -ENOSYS;
}

static int phy_xpcs_read_c45(const struct device *dev, uint8_t dev_addr, uint16_t reg_addr,
			     uint16_t *data)
{
	const struct phy_xpcs_config *cfg = dev->config;

	if (dev_addr > 32) {
		return -EINVAL;
	}

	*data = *((volatile uint32_t *)cfg->base_addr + dev_addr * 0x10000 + reg_addr);

	return 0;
}

static int phy_xpcs_write_c45(const struct device *dev, uint8_t dev_addr, uint16_t reg_addr,
			      uint16_t data)
{
	const struct phy_xpcs_config *cfg = dev->config;

	if (dev_addr > 32) {
		return -EINVAL;
	}

	*((volatile uint32_t *)cfg->base_addr + dev_addr * 0x10000 + reg_addr) = data;

	return 0;
}

static inline enum phy_link_speed xpcs_get_mii_link_speed(uint32_t mii_control)
{
	uint32_t ss = ((mii_control & MII_CONTROL_SS13) >> 13) |
		      ((mii_control & MII_CONTROL_SS6) >> (6 - 1)) |
		      ((mii_control & MII_CONTROL_SS5) >> (5 - 2));
	switch (ss) {
	case 0x5:
		return LINK_FULL_5000BASE;
	case 0x4:
		return LINK_FULL_2500BASE;
	case 0x2:
		return (mii_control & MII_CONTROL_DUPLEX_MODE) ? LINK_FULL_1000BASE
							       : LINK_HALF_1000BASE;
	case 0x1:
		return (mii_control & MII_CONTROL_DUPLEX_MODE) ? LINK_FULL_100BASE
							       : LINK_HALF_100BASE;
	case 0x0:
		return (mii_control & MII_CONTROL_DUPLEX_MODE) ? LINK_FULL_10BASE
							       : LINK_HALF_10BASE;
	default:
		return LINK_HALF_10BASE;
	}
}

static inline int phy_xpcs_restart_an(const struct device *dev)
{
	uint16_t data;
	int ret = 0;
	ret |= phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_CTRL, &data);
	ret |= phy_xpcs_write_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_CTRL,
				  data | MII_CONTROL_RESTART_AN);
	return ret;
}

static inline int phy_xpcs_an_is_complete(const struct device *dev)
{
	const struct phy_xpcs_config *const cfg = dev->config;
	struct phy_xpcs_data *const data = dev->data;
	uint16_t an_sts = 0;

	if (phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_STS, &an_sts) < 0) {
		LOG_ERR("%s: failed to get autonegotiation status", dev->name);
		return -EIO;
	}
	if (an_sts & SR_MII_STS_AN_COMPLETE) {
		LOG_DBG("%s: auto-negotiation complete", dev->name);
		uint16_t an_intr;
		phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, VR_MII_AN_INTR_STS, &an_intr);
		
		if (FIELD_GET(BIT(4), an_intr) == 0) {
			phy_xpcs_restart_an(dev);
			return 0;
		}

		if (!cfg->uplink && data->pcs == XPCS_PCS_SGMII) {
			phy_xpcs_set_speed(dev, 1 << (FIELD_GET(GENMASK(3, 2), an_intr) * 2 +
						      FIELD_GET(BIT(1), an_intr)));
		}
#if CONFIG_PHY_LOG_LEVEL == LOG_LEVEL_DBG
		LOG_DBG("%s: AN result speed: %lx duplex: %lx link: %lx", dev->name,
			FIELD_GET(GENMASK(3, 2), an_intr), FIELD_GET(BIT(1), an_intr),
			FIELD_GET(BIT(4), an_intr));
#endif
		return 1;
	} else if (an_sts & SR_MII_STS_LINK_UP) {
		return 1;
	}

	if (sys_timepoint_expired(data->an_timeout)) {
		LOG_WRN("%s: auto-negotiation timed out", dev->name);
		data->an_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	}

	return 0;
}

static int phy_xpcs_update_link_state(const struct device *dev)
{
	struct phy_xpcs_data *const data = dev->data;
	struct phy_link_state new_state;
	uint16_t mii_ctrl, mii_sts, dig_ctrl;

	/* Read Phy registers to determine speed and duplex state */
	if (phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_STS, &mii_sts) < 0) {
		return -EIO;
	}
	if (phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_CTRL, &mii_ctrl) < 0) {
		return -EIO;
	}
	if (phy_xpcs_read_c45(dev, MDIO_MMD_PCS, VR_XS_PCS_DIG_CTRL1, &dig_ctrl) < 0) {
		return -EIO;
	}
	new_state.is_up = (mii_sts & SR_MII_STS_LINK_UP) != 0;

	if (new_state.is_up == true) {
		new_state.speed = xpcs_get_mii_link_speed(mii_ctrl);
		if (dig_ctrl & VR_XS_PCS_DIG_CTRL1_EN_2_5G_MODE) {
			new_state.speed = LINK_FULL_2500BASE;
		}
	} else {
		new_state.speed = data->state.speed;
	}

	if (memcmp(&new_state, &data->state, sizeof(struct phy_link_state)) != 0) {
		data->state = new_state;
		if (new_state.is_up) {
			LOG_INF("%s: Link is up", dev->name);
			LOG_INF("%s: Link speed %s Mb, %s duplex\n", dev->name,
				PHY_LINK_IS_SPEED_5000M(new_state.speed) ? "5000"
				: PHY_LINK_IS_SPEED_2500M(new_state.speed)
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

static void monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct phy_xpcs_data *const data = CONTAINER_OF(dwork, struct phy_xpcs_data, monitor_work);
	const struct device *dev = data->dev;
	const struct phy_xpcs_config *const config = dev->config;
	int rc = 0;

	struct phy_link_state uplink_state;

	if (config->uplink) {
		phy_get_link_state(config->uplink, &uplink_state);
		k_mutex_lock(&data->mutex, K_FOREVER);
		if (uplink_state.is_up && uplink_state.speed != data->state.speed) {
			if (PHY_LINK_IS_SPEED_2500M(uplink_state.speed)) {
				if (phy_xpcs_set_sgmii2_5(dev) < 0) {
					LOG_ERR("%s: failed to reconfigure to 2.5G SGMII",
						dev->name);
					return;
				}
				LOG_DBG("%s: reconfigure to 2.5G SGMII", dev->name);
			} else if (PHY_LINK_IS_SPEED_2500M(data->state.speed)) {
				if (phy_xpcs_set_sgmii(dev, uplink_state.speed) < 0) {
					LOG_ERR("%s: failed to reconfigure to 1G SGMII", dev->name);
					return;
				}
				LOG_DBG("%s: reconfigure to 1G SGMII", dev->name);
			} else if (data->pcs == XPCS_PCS_SGMII) {
				phy_xpcs_set_speed(dev, uplink_state.speed);
			}
			data->state.speed = uplink_state.speed;
		}
		k_mutex_unlock(&data->mutex);
	}

	k_mutex_lock(&data->mutex, K_FOREVER);

#if CONFIG_PHY_LOG_LEVEL == LOG_LEVEL_DBG
	if (data->state.is_up == false && sys_timepoint_expired(data->an_timeout)) {
		if (phy_xpcs_read_c45(dev, MDIO_MMD_PMAPMD, PMAPMD_STATUS1, &reg_val) == 0) {
			LOG_DBG("%s: PMA State Link: %lu Fault: %lu", dev->name,
				FIELD_GET(PMAPMD_STATUS1_LINK_STATUS, reg_val),
				FIELD_GET(PMAPMD_STATUS1_FAULT, reg_val));
		}
		if (phy_xpcs_read_c45(dev, MDIO_MMD_PCS, PCS_STATUS1, &reg_val) == 0) {
			LOG_DBG("%s: PCS State Link: %lu Fault: %lu", dev->name,
				FIELD_GET(PCS_STATUS1_LINK_STATUS, reg_val),
				FIELD_GET(PCS_STATUS1_FAULT, reg_val));
		}
		if (phy_xpcs_read_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, SR_MII_STS, &reg_val) == 0) {
			LOG_DBG("%s: AN State Link: %lu Remote Fault: %lu", dev->name,
				FIELD_GET(SR_MII_STS_LINK_UP, reg_val),
				FIELD_GET(SR_MII_STS_REMOTE_FAULT, reg_val));
		}
		if (PHY_LINK_IS_SPEED_2500M(data->state.speed)) {
			data->an_timeout =
				sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
		}
	}
#endif

	/* Check if on going auto negotiation */
	if (data->state.is_up || config->fixed || PHY_LINK_IS_SPEED_2500M(uplink_state.speed) || phy_xpcs_an_is_complete(dev)) {
		rc = phy_xpcs_update_link_state(dev);
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
			data->cb(dev, &data->state, data->user_data);
		}
		if (!data->state.is_up && !PHY_LINK_IS_SPEED_2500M(data->state.speed)) {
			data->an_timeout =
				sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
		}
	}

	k_work_reschedule(&data->monitor_work,
			  data->state.is_up ? K_MSEC(CONFIG_PHY_MONITOR_PERIOD) : K_MSEC(10));
}

static int phy_xpcs_get_link_state(const struct device *dev, struct phy_link_state *state)
{
	const struct phy_xpcs_config *const cfg = dev->config;
	struct phy_xpcs_data *const data = dev->data;

	if (cfg->fixed) {
		state->is_up = true;
		state->speed = cfg->fixed_speed;
	} else {
		k_mutex_lock(&data->mutex, K_FOREVER);
		*state = data->state;
		k_mutex_unlock(&data->mutex);
	}
	return 0;
}

static int phy_xpcs_cfg_link(const struct device *dev, enum phy_link_speed adv_speeds,
			     enum phy_cfg_link_flag flags)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(adv_speeds);
	ARG_UNUSED(flags);

	return -ENOSYS;
}

static int phy_xpcs_link_cb_set(const struct device *dev, phy_callback_t cb, void *user_data)
{
	struct phy_xpcs_data *data = dev->data;

	data->cb = cb;
	data->user_data = user_data;

	return 0;
}

static inline void HSPHY_updateTxConfig(Ifx_HSPHY_XPCS *xpcs)
{
	/* Tx req + ack handshake */
	/* Set Request and wait for tx ack = 1*/
	xpcs->PMA.VR_XS_MP_12G_16G_TX_GENCTRL2.B.TX_REQ_0 = 1;
	while (xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_STS.B.TX_ACK_0 == 0)
		;
	/* Release request and wait for tx ack = 0*/
	xpcs->PMA.VR_XS_MP_12G_16G_TX_GENCTRL2.B.TX_REQ_0 = 0;
	while (xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_STS.B.TX_ACK_0 == 1)
		;
}

struct xpcs_settings {
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_VCO_CAL_REF0 vco_cal_ref0;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_REF_CLK_CTRL ref_clk_cltr;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_MPLLA_CTRL0 mplla_ctrl0;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_16G_MPLLA_CTRL1 mplla_ctrl1;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_MPLLA_CTRL2 mplla_ctrl2;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_8G_MPLLA_CTRL6 mplla_ctrl6;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_8G_MPLLA_CTRL7 mplla_ctrl7;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_16G_25G_MISC_CTRL2 misc_ctrl2;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_16G_25G_RX_MISC_CTRL0 rx_misc_ctrl0;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_RX_GENCTRL1 rx_genctrl1;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_RX_GENCTRL2 rx_genctrl2;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_16G_25G_RX_GENCTRL4 rx_genctrl4;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_TX_GENCTRL1 tx_genctrl1;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_TX_GENCTRL2 tx_genctrl2;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_TX_BOOST_CTRL tx_boost_ctrl;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_RX_RATE_CTRL rx_rate_ctrl;
	Ifx_HSPHY_XPCS_PMA_VR_XS_MP_12G_16G_25G_TX_RATE_CTRL tx_rate_ctrl;
};

static const struct xpcs_settings settings[2] = {
	{
		.vco_cal_ref0.B =
			{
				.VCO_REF_LD_0 = 10,
			},
		.ref_clk_cltr.B =
			{
				.REF_MPLLA_DIV2 = 0,
				.REF_CLK_DIV2 = 0,
				.REF_RANGE = 0,
				.REF_USE_PAD = 1,
				.REF_CLK_EN = 1,
			},
		.mplla_ctrl0.B =
			{
				.MPLLA_MULTIPLIER = 50,
			},
		.mplla_ctrl1.U = 0,
		.mplla_ctrl2.B = {.MPLLA_TX_CLK_DIV = 1, .MPLLA_DIV10_CLK_EN = 1},
		.mplla_ctrl6.B = {.CP_INT = 2, .CP_PROP = 12},
		.mplla_ctrl7.B = {.CP_INT_GS = 1, .CP_PROP_GS = 10},
		.misc_ctrl2.B = {.SUP_MISC = 1},
		.rx_misc_ctrl0.B = {.RX0_MISC = 161},
		.rx_genctrl1.B = {.RX_TERM_ACDC_0 = 1},
		.rx_genctrl2.B =
			{
				.RX0_WIDTH = 1,
			},
		.rx_genctrl4.U = 0,
		.tx_genctrl1.U = 0,
		.tx_genctrl2.B =
			{
				.TX0_WIDTH = 1,
			},
		.tx_boost_ctrl.B =
			{
				.TX0_IBOOST = 15,
			},
		.rx_rate_ctrl.B = {.RX0_RATE = 2},
		.tx_rate_ctrl.B = {.TX0_RATE = 2},
	},
	{
		.vco_cal_ref0.B =
			{
				.VCO_REF_LD_0 = 8,
			},
		.ref_clk_cltr.B =
			{
				.REF_MPLLA_DIV2 = 0,
				.REF_CLK_DIV2 = 0,
				.REF_RANGE = 0,
				.REF_USE_PAD = 1,
				.REF_CLK_EN = 1,
			},
		.mplla_ctrl0.B =
			{
				.MPLLA_MULTIPLIER = 125,
			},
		.mplla_ctrl1.U = 0,
		.mplla_ctrl2.B = {.MPLLA_DIV10_CLK_EN = 1},
		.mplla_ctrl6.B = {.CP_INT = 2, .CP_PROP = 12},
		.mplla_ctrl7.B = {.CP_INT_GS = 1, .CP_PROP_GS = 10},
		.misc_ctrl2.B = {.SUP_MISC = 1},
		.rx_misc_ctrl0.B = {.RX0_MISC = 96},
		.rx_genctrl1.B = {.RX_TERM_ACDC_0 = 1},
		.rx_genctrl2.B =
			{
				.RX0_WIDTH = 1,
			},
		.rx_genctrl4.U = 0,
		.tx_genctrl1.U = 0,
		.tx_genctrl2.B =
			{
				.TX0_WIDTH = 1,
			},
		.tx_boost_ctrl.B =
			{
				.TX0_IBOOST = 15,
			},
		.rx_rate_ctrl.B = {.RX0_RATE = 1},
		.tx_rate_ctrl.B = {.TX0_RATE = 1},
	},
};

static int phy_xpcs_set_sgmii2_5(const struct device *dev)
{
	const struct phy_xpcs_config *cfg = dev->config;
	Ifx_HSPHY_XPCS *xpcs = (cfg->base_addr + 0x40000);

	/* PCS Configuration */
	/* 0001: Select 10GBASE-X PCS Type */
	xpcs->PCS.SR_XS_CTRL2.B.PCS_TYPE_SEL = 0x1;
	/* Disable 2.5G SGMII */
	xpcs->PCS.VR_XS_DIG_CTRL1.B.EN_2_5G_MODE = 1;
	/* Set to 1GBASE-X PCS*/
	xpcs->PCS.SR_XS_CTRL1.B.SS13 = 0;
	/* Set MII Interface speed */
	xpcs->MII.CTRL.B = (Ifx_HSPHY_XPCS_MII_CTRL_Bits){
		.AN_ENABLE = 0,
		.DUPLEX_MODE = 1,
		.SS6 = 1,
		.SS13 = 0,
	};

	/* Disable mpll to change the related values. */
	if (xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0) {
		xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0 = 0;
		HSPHY_updateTxConfig(xpcs);
	}

	/* Set reference clock values */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_REF_CLK_CTRL.U = settings[1].ref_clk_cltr.U;
	/* MPLLA: Configuration */
	xpcs->PMA.VR_XS_MP_12G_16G_MPLLA_CTRL0 = settings[1].mplla_ctrl0;
	xpcs->PMA.VR_XS_MP_16G_MPLLA_CTRL1 = settings[1].mplla_ctrl1;
	xpcs->PMA.VR_XS_MP_12G_16G_MPLLA_CTRL2 = settings[1].mplla_ctrl2;
	HSPHY_updateTxConfig(xpcs);

	xpcs->PMA.VR_XS_MP_12G_16G_25G_VCO_CAL_LD0.B.VCO_LD_VAL_0 = 1000;
	xpcs->PMA.VR_XS_MP_12G_VCO_CAL_REF0 = settings[1].vco_cal_ref0;
	xpcs->PMA.VR_XS_MP_8G_MPLLA_CTRL6 = settings[1].mplla_ctrl6;
	xpcs->PMA.VR_XS_MP_8G_MPLLA_CTRL7 = settings[1].mplla_ctrl7;
	xpcs->PMA.VR_XS_MP_16G_25G_MISC_CTRL2 = settings[1].misc_ctrl2;
	xpcs->PMA.VR_XS_MP_16G_25G_RX_MISC_CTRL0 = settings[1].rx_misc_ctrl0;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_RX_GENCTRL1 = settings[1].rx_genctrl1;
	xpcs->PMA.VR_XS_MP_12G_16G_RX_GENCTRL2 = settings[1].rx_genctrl2;
	xpcs->PMA.VR_XS_MP_16G_25G_RX_GENCTRL4 = settings[1].rx_genctrl4;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1 = settings[1].tx_genctrl1;
	xpcs->PMA.VR_XS_MP_12G_16G_TX_GENCTRL2 = settings[1].tx_genctrl2;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_BOOST_CTRL = settings[1].tx_boost_ctrl;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_RX_RATE_CTRL = settings[1].rx_rate_ctrl;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_RATE_CTRL = settings[1].tx_rate_ctrl;

	/* Enable MPLL again */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0 = 1;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1.B.TX_CLK_RDY_0 = 0;

	/* Perform vendor reset to start everything  */
	xpcs->PCS.VR_XS_DIG_CTRL1.B.VR_RST = 1;
	/* Wait for pcs mem to initialize */
	while (xpcs->PMA.VR_XS_MP_12G_16G_25G_SRAM.B.INIT_DN == 0)
		;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_SRAM.B.EXT_LD_DN = 1;
	/* Wait for the reset to complete */
	while (xpcs->PCS.VR_XS_DIG_CTRL1.B.VR_RST == 1)
		;
	/* Enable tx clock */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1.B.TX_CLK_RDY_0 = 1;

	((struct phy_xpcs_data *)dev->data)->pcs = XPCS_PCS_SGMII_2_5G;

	return 0;
}

static int phy_xpcs_set_sgmii(const struct device *dev, uint8_t speed)
{
	const struct phy_xpcs_config *cfg = dev->config;
	Ifx_HSPHY_XPCS *xpcs = (cfg->base_addr + 0x40000);

	/* PCS Configuration */
	/* 0001: Select 10GBASE-X PCS Type */
	xpcs->PCS.SR_XS_CTRL2.B.PCS_TYPE_SEL = 0x1;
	/* Disable 2.5G SGMII */
	xpcs->PCS.VR_XS_DIG_CTRL1.B.EN_2_5G_MODE = 0;
	/* Set to 1GBASE-X PCS*/
	xpcs->PCS.SR_XS_CTRL1.B.SS13 = 0;
	/* Set MII Interface speed */
	xpcs->MII.CTRL.B = (Ifx_HSPHY_XPCS_MII_CTRL_Bits){
		.AN_ENABLE = !cfg->fixed,
		.DUPLEX_MODE = 1,
		.SS6 = PHY_LINK_IS_SPEED_1000M(speed) ? 1 : 0,
		.SS13 = PHY_LINK_IS_SPEED_100M(speed) ? 1 : 0,
	};

	/* Disable mpll to change the related values. */
	if (xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0) {
		xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0 = 0;
		HSPHY_updateTxConfig(xpcs);
	}

	/* Set reference clock values */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_REF_CLK_CTRL.U = settings[0].ref_clk_cltr.U;
	/* MPLLA: Configuration */
	xpcs->PMA.VR_XS_MP_12G_16G_MPLLA_CTRL0.U = settings[0].mplla_ctrl0.U;
	xpcs->PMA.VR_XS_MP_16G_MPLLA_CTRL1.U = settings[0].mplla_ctrl1.U;
	xpcs->PMA.VR_XS_MP_12G_16G_MPLLA_CTRL2.U = settings[0].mplla_ctrl2.U;
	HSPHY_updateTxConfig(xpcs);

	xpcs->PMA.VR_XS_MP_12G_16G_25G_VCO_CAL_LD0.B.VCO_LD_VAL_0 = 1000;
	xpcs->PMA.VR_XS_MP_12G_VCO_CAL_REF0.U = settings[0].vco_cal_ref0.U;
	xpcs->PMA.VR_XS_MP_8G_MPLLA_CTRL6.U = settings[0].mplla_ctrl6.U;
	xpcs->PMA.VR_XS_MP_8G_MPLLA_CTRL7.U = settings[0].mplla_ctrl7.U;
	xpcs->PMA.VR_XS_MP_16G_25G_MISC_CTRL2.U = settings[0].misc_ctrl2.U;
	xpcs->PMA.VR_XS_MP_16G_25G_RX_MISC_CTRL0.U = settings[0].rx_misc_ctrl0.U;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_RX_GENCTRL1.U = settings[0].rx_genctrl1.U;
	xpcs->PMA.VR_XS_MP_12G_16G_RX_GENCTRL2.U = settings[0].rx_genctrl2.U;
	xpcs->PMA.VR_XS_MP_16G_25G_RX_GENCTRL4.U = settings[0].rx_genctrl4.U;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1.U = settings[0].tx_genctrl1.U;
	xpcs->PMA.VR_XS_MP_12G_16G_TX_GENCTRL2.U = settings[0].tx_genctrl2.U;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_BOOST_CTRL.U = settings[0].tx_boost_ctrl.U;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_RX_RATE_CTRL.U = settings[0].rx_rate_ctrl.U;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_RATE_CTRL.U = settings[0].tx_rate_ctrl.U;

	/* Enable MPLL again */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_MPLL_CMN_CTRL.B.MPLL_EN_0 = 1;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1.B.TX_CLK_RDY_0 = 0;

	/* Perform vendor reset to start everything  */
	xpcs->PCS.VR_XS_DIG_CTRL1.B.VR_RST = 1;
	/* Wait for pcs mem to initialize */
	while (xpcs->PMA.VR_XS_MP_12G_16G_25G_SRAM.B.INIT_DN == 0)
		;
	xpcs->PMA.VR_XS_MP_12G_16G_25G_SRAM.B.EXT_LD_DN = 1;
	/* Wait for the reset to complete */
	while (xpcs->PCS.VR_XS_DIG_CTRL1.B.VR_RST == 1)
		;
	/* Enable tx clock */
	xpcs->PMA.VR_XS_MP_12G_16G_25G_TX_GENCTRL1.B.TX_CLK_RDY_0 = 1;

	((struct phy_xpcs_data *)dev->data)->pcs = XPCS_PCS_SGMII;
	return 0;
}

static int phy_xpcs_set_speed(const struct device *dev, enum phy_link_speed speed)
{
	const struct phy_xpcs_config *const cfg = dev->config;
	Ifx_HSPHY_XPCS *const xpcs = (cfg->base_addr + 0x40000);
	xpcs->MII.CTRL.B = (Ifx_HSPHY_XPCS_MII_CTRL_Bits){
		.AN_ENABLE = !cfg->fixed,
		.DUPLEX_MODE = PHY_LINK_IS_FULL_DUPLEX(speed) ? 1 : 0,
		.SS6 = PHY_LINK_IS_SPEED_1000M(speed) ? 1 : 0,
		.SS13 = PHY_LINK_IS_SPEED_100M(speed) ? 1 : 0,
	};

	return 0;
}

static int phy_xpcs_initialize(const struct device *dev)
{
	const struct phy_xpcs_config *cfg = dev->config;
	struct phy_xpcs_data *data = dev->data;
	int ret;

	data->dev = dev;

	if (cfg->fixed) {
		if (PHY_LINK_IS_SPEED_2500M(cfg->fixed_speed)) {
			ret = phy_xpcs_set_sgmii2_5(dev);
		} else {
			ret = phy_xpcs_set_sgmii(dev, cfg->fixed_speed);
		}
		data->state.speed = cfg->fixed_speed;
	} else {
		ret = phy_xpcs_set_sgmii(dev, LINK_FULL_1000BASE);
		data->state.speed = LINK_FULL_1000BASE;
	}
	if (ret) {
		return ret;
	}

	phy_xpcs_write_c45(dev, MDIO_MMD_VENDOR_SPECIFIC2, VR_MII_AN_CTRL,
			   FIELD_PREP(VR_MII_AN_CTRL_PCS_MODE, 2));
	data->an_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));

	k_mutex_init(&data->mutex);
	k_work_init_delayable(&data->monitor_work, monitor_work_handler);

	monitor_work_handler(&data->monitor_work.work);

	return 0;
}

static const struct ethphy_driver_api phy_xpcs_driver_api = {
	.get_link = phy_xpcs_get_link_state,
	.cfg_link = phy_xpcs_cfg_link,
	.link_cb_set = phy_xpcs_link_cb_set,
	.read = phy_xpcs_read,
	.write = phy_xpcs_write,
	.read_c45 = phy_xpcs_read_c45,
	.write_c45 = phy_xpcs_write_c45,
};

#define PHY_XPCS_CONFIG(n)                                                                         \
	static const struct phy_xpcs_config phy_xpcs_config_##n = {                                \
		.base_addr = (void *)DT_INST_REG_ADDR(n),                                          \
		.uplink = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(n, phy_handle)),                   \
		.fixed = IS_FIXED_LINK(n),                                                         \
		.fixed_speed = BIT(DT_INST_ENUM_IDX_OR(n, fixed_link, 0)),                         \
	};

#define IS_FIXED_LINK(n) DT_INST_NODE_HAS_PROP(n, fixed_link)

#define PHY_XPCS_DEVICE(n)                                                                         \
	PHY_XPCS_CONFIG(n);                                                                        \
	static struct phy_xpcs_data phy_xpcs_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, &phy_xpcs_initialize, NULL, &phy_xpcs_data_##n,                   \
			      &phy_xpcs_config_##n, POST_KERNEL, CONFIG_PHY_INIT_PRIORITY,         \
			      &phy_xpcs_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PHY_XPCS_DEVICE)
