/*
 * Copyright (c) 2026 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Driver for the Synopsys 10BASE-T1S internal PHY
 */

#define DT_DRV_COMPAT snps_t1s_phy

#include <zephyr/kernel.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/mdio.h>
#include <zephyr/drivers/mdio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_snps_t1s, CONFIG_PHY_LOG_LEVEL);

/* PMD/PCS/PLCA register layout used by IfxLeth_b10t1s_* helpers */
#define LETH_B10T1S_MMD_PMA  0x01U
#define LETH_B10T1S_MMD_PCS  0x03U
#define LETH_B10T1S_MMD_PLCA 0x1FU

#define LETH_B10T1S_MMD_PCS_CTRL       0x08F3U
#define LETH_B10T1S_MMD_PMA_CTRL       0x08F9U
#define LETH_B10T1S_MMD_PMA_VEND_CTRL  0x2000U
#define LETH_B10T1S_MMD_PMA_VEND_CTRL1 0x2001U
#define LETH_B10T1S_MMD_PLCA_CTRL	   0xCA01U
#define LETH_B10T1S_MMD_PLCA_BUF_DEPTH 0xE010U

#define LETH_B10T1S_MMD_PCS_CTRL_DM_OFF  8U
#define LETH_B10T1S_MMD_PCS_CTRL_PCD_OFF 10U

#define LETH_B10T1S_MMD_PMA_CTRL_EBRTH_MSK 0x1FU
#define LETH_B10T1S_MMD_PMA_CTRL_EBRTH_OFF 1U
#define LETH_B10T1S_MMD_PMA_CTRL_MM_OFF    10U

#define LETH_B10T1S_MMD_PMA_VEND_CTRL_DDEZ_OFF      0U
#define LETH_B10T1S_MMD_PMA_VEND_CTRL_EDLGLITCH_MSK 0xFFU
#define LETH_B10T1S_MMD_PMA_VEND_CTRL_EDLGLITCH_OFF 8U

#define LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSASSRT_MSK  0xFFU
#define LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSASSRT_OFF  0U
#define LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSDASSRT_MSK 0xFFU
#define LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSDASSRT_OFF 8U

#define LETH_B10T1S_MMD_PLCA_BUF_DEPTH_PVBD_MSK 0x7FU
#define LETH_B10T1S_MMD_PLCA_BUF_DEPTH_PVBD_OFF 0U

#define LETH_B10T1S_MMD_PLCA_CTRL_RST BIT(14)

#define SNPS_T1S_FLD_PREP(msk, off, val) ((((uint16_t)(val)) & ((uint16_t)(msk))) << (off))

/* PCS status register (IEEE 802.3, 3.1) – used for link monitoring when PLCA
 * is disabled so that CSMA/CD mode still reports a live link.
 */
#define SNPS_T1S_PCS_STS_REG  0x0001U
#define SNPS_T1S_PCS_RCV_LINK BIT(2) /* Receive Link Status */

struct snps_t1s_plca_cfg {
	bool enable;
	uint8_t node_id;
	uint8_t node_count;
	uint8_t burst_count;
	uint8_t burst_timer;
	uint8_t to_timer;
};

struct snps_t1s_config {
	uint8_t phy_addr;
	const struct device *mdio;
	const struct snps_t1s_plca_cfg *plca;
	uint8_t buf_depth;
	bool disable_physical_collision;
};

struct snps_t1s_data {
	const struct device *dev;
	bool link_is_up;
	phy_callback_t cb;
	void *cb_data;
	struct k_work_delayable phy_monitor_work;
	struct k_sem sem;
};

/* --------------------------------------------------------------------------
 * Low-level MDIO helpers
 * -------------------------------------------------------------------------- */

static int snps_t1s_read(const struct device *dev, uint16_t reg, uint32_t *data)
{
	const struct snps_t1s_config *cfg = dev->config;

	*data = 0U;
	return mdio_read(cfg->mdio, cfg->phy_addr, reg, (uint16_t *)data);
}

static int snps_t1s_write(const struct device *dev, uint16_t reg, uint32_t data)
{
	const struct snps_t1s_config *cfg = dev->config;

	return mdio_write(cfg->mdio, cfg->phy_addr, reg, (uint16_t)data);
}

static int snps_t1s_read_c45(const struct device *dev, uint8_t devad, uint16_t reg, uint16_t *val)
{
	const struct snps_t1s_config *cfg = dev->config;

	return mdio_read_c45(cfg->mdio, cfg->phy_addr, devad, reg, val);
}

static int snps_t1s_write_c45(const struct device *dev, uint8_t devad, uint16_t reg, uint16_t val)
{
	const struct snps_t1s_config *cfg = dev->config;

	return mdio_write_c45(cfg->mdio, cfg->phy_addr, devad, reg, val);
}

/* --------------------------------------------------------------------------
 * Link state
 * -------------------------------------------------------------------------- */

static int snps_t1s_get_link(const struct device *dev, struct phy_link_state *state)
{
	struct snps_t1s_data *data = dev->data;

	/* 10BASE-T1S is always half-duplex 10 Mbit/s */
	state->speed = LINK_HALF_10BASE;

	k_sem_take(&data->sem, K_FOREVER);
	state->is_up = data->link_is_up;
	k_sem_give(&data->sem);

	return 0;
}

static void snps_t1s_invoke_link_cb(const struct device *dev)
{
	struct snps_t1s_data *data = dev->data;
	struct phy_link_state state;

	if (data->cb == NULL) {
		return;
	}

	snps_t1s_get_link(dev, &state);
	data->cb(dev, &state, data->cb_data);
}

static void snps_t1s_update_link_state(const struct device *dev)
{
	const struct snps_t1s_config *cfg = dev->config;
	struct snps_t1s_data *data = dev->data;
	bool old_link = data->link_is_up;
	bool new_link;
	int ret;

	ret = genphy_get_plca_sts(dev, &new_link);
	if (ret < 0) {
		LOG_DBG("PLCA status read failed: %d", ret);
		return;
	}

	if (!new_link) {
		/* Fallback: PCS receive link status (MMD 3, reg 0x0001, bit 2) */
		uint16_t val;

		ret = snps_t1s_read_c45(dev, MDIO_MMD_PCS, SNPS_T1S_PCS_STS_REG, &val);
		if (ret < 0) {
			LOG_DBG("PCS status read failed: %d", ret);
			return;
		}
		new_link = (val & SNPS_T1S_PCS_RCV_LINK) != 0;
	}

	k_sem_take(&data->sem, K_FOREVER);
	data->link_is_up = new_link;
	k_sem_give(&data->sem);

	if (old_link != new_link) {
		if (new_link) {
			LOG_INF("PHY (%u) link up, 10BASE-T1S %s", cfg->phy_addr,
				cfg->plca->enable ? "PLCA" : "CSMA/CD");
		} else {
			LOG_INF("PHY (%u) link down", cfg->phy_addr);
		}
		snps_t1s_invoke_link_cb(dev);
	}
}

static int snps_t1s_link_cb_set(const struct device *dev, phy_callback_t cb, void *user_data)
{
	struct snps_t1s_data *data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	snps_t1s_invoke_link_cb(dev);

	return 0;
}

static void phy_monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct snps_t1s_data *const data =
		CONTAINER_OF(dwork, struct snps_t1s_data, phy_monitor_work);

	snps_t1s_update_link_state(data->dev);

	k_work_reschedule(&data->phy_monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

/* --------------------------------------------------------------------------
 * PLCA configuration
 * -------------------------------------------------------------------------- */

static int snps_t1s_apply_dt_plca(const struct device *dev)
{
	const struct snps_t1s_config *cfg = dev->config;
	struct phy_plca_cfg plca_cfg = {
		.enable = cfg->plca->enable,
		.node_id = cfg->plca->node_id,
		.node_count = cfg->plca->node_count,
		.burst_count = cfg->plca->burst_count,
		.burst_timer = cfg->plca->burst_timer,
		.to_timer = cfg->plca->to_timer,
	};

	return genphy_set_plca_cfg(dev, &plca_cfg);
}

static int snps_t1s_pmd_init(const struct device *dev)
{
	const struct snps_t1s_config *cfg = dev->config;
	uint16_t regval;
	int ret;

	ret = snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PLCA, LETH_B10T1S_MMD_PLCA_CTRL,
				 LETH_B10T1S_MMD_PLCA_CTRL_RST);
	if (ret < 0) {
		return 0;
	}
	while (1) {
		ret = snps_t1s_read_c45(dev, LETH_B10T1S_MMD_PLCA, LETH_B10T1S_MMD_PLCA_CTRL, &regval);
		if (ret < 0) {
			return 0;
		}
		if ((regval & LETH_B10T1S_MMD_PLCA_CTRL_RST) == 0) {
			break;
		}
	}

	regval = BIT(LETH_B10T1S_MMD_PCS_CTRL_DM_OFF);
	if (cfg->disable_physical_collision) {
		regval |= BIT(LETH_B10T1S_MMD_PCS_CTRL_PCD_OFF);
	}

	ret = snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PCS, LETH_B10T1S_MMD_PCS_CTRL, regval);
	if (ret < 0) {
		return ret;
	}

	regval = BIT(LETH_B10T1S_MMD_PMA_CTRL_MM_OFF) |
		 SNPS_T1S_FLD_PREP(LETH_B10T1S_MMD_PMA_CTRL_EBRTH_MSK,
				   LETH_B10T1S_MMD_PMA_CTRL_EBRTH_OFF, 2U);
	ret = snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PMA, LETH_B10T1S_MMD_PMA_CTRL, regval);
	if (ret < 0) {
		return ret;
	}

	regval = BIT(LETH_B10T1S_MMD_PMA_VEND_CTRL_DDEZ_OFF) |
		 SNPS_T1S_FLD_PREP(LETH_B10T1S_MMD_PMA_VEND_CTRL_EDLGLITCH_MSK,
				   LETH_B10T1S_MMD_PMA_VEND_CTRL_EDLGLITCH_OFF, 0x1EU);
	ret = snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PMA, LETH_B10T1S_MMD_PMA_VEND_CTRL, regval);
	if (ret < 0) {
		return ret;
	}

	regval = SNPS_T1S_FLD_PREP(LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSASSRT_MSK,
				   LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSASSRT_OFF, 40U) |
		 SNPS_T1S_FLD_PREP(LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSDASSRT_MSK,
				   LETH_B10T1S_MMD_PMA_VEND_CTRL1_CRSDASSRT_OFF, 64U);
	ret = snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PMA, LETH_B10T1S_MMD_PMA_VEND_CTRL1, regval);
	if (ret < 0) {
		return ret;
	}

	regval = SNPS_T1S_FLD_PREP(LETH_B10T1S_MMD_PLCA_BUF_DEPTH_PVBD_MSK,
				   LETH_B10T1S_MMD_PLCA_BUF_DEPTH_PVBD_OFF, cfg->buf_depth);

	return snps_t1s_write_c45(dev, LETH_B10T1S_MMD_PLCA, LETH_B10T1S_MMD_PLCA_BUF_DEPTH,
				  regval);
}

/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

static int snps_t1s_init(const struct device *dev)
{
	const struct snps_t1s_config *cfg = dev->config;
	struct snps_t1s_data *data = dev->data;
	uint32_t phy_id;
	uint32_t val;
	int ret;

	k_sem_init(&data->sem, 1, 1);
	data->dev = dev;

	if (!device_is_ready(cfg->mdio)) {
		LOG_ERR("MDIO bus not ready");
		return -ENODEV;
	}

	ret = mdio_read_c45(cfg->mdio, cfg->phy_addr, LETH_B10T1S_MMD_PCS, MII_PHYID1R,
			    (uint16_t *)&val);
	if (ret < 0) {
		LOG_ERR("PHY ID1 read failed: %d", ret);
		return ret;
	}
	phy_id = (val & 0xffffU) << 16;

	ret = mdio_read_c45(cfg->mdio, cfg->phy_addr, LETH_B10T1S_MMD_PCS, MII_PHYID2R,
			    (uint16_t *)&val);
	if (ret < 0) {
		LOG_ERR("PHY ID2 read failed: %d", ret);
		return ret;
	}
	phy_id |= (val & 0xffffU);

	LOG_INF("Synopsys 10BASE-T1S PHY detected, ID 0x%08x (addr %u)", phy_id, cfg->phy_addr);

	ret = snps_t1s_pmd_init(dev);
	if (ret < 0) {
		LOG_ERR("PMD init sequence failed: %d", ret);
		return ret;
	}

	/* Apply PLCA configuration from device tree */
	ret = snps_t1s_apply_dt_plca(dev);
	if (ret < 0) {
		LOG_ERR("PLCA configuration failed: %d", ret);
		return ret;
	}

	/* Kick off the periodic link monitor */
	k_work_init_delayable(&data->phy_monitor_work, phy_monitor_work_handler);
	phy_monitor_work_handler(&data->phy_monitor_work.work);

	return 0;
}

/* --------------------------------------------------------------------------
 * Driver API
 * -------------------------------------------------------------------------- */

static DEVICE_API(ethphy, snps_t1s_phy_api) = {
	.get_link = snps_t1s_get_link,
	.link_cb_set = snps_t1s_link_cb_set,
	.set_plca_cfg = genphy_set_plca_cfg,
	.get_plca_cfg = genphy_get_plca_cfg,
	.get_plca_sts = genphy_get_plca_sts,
	.read = snps_t1s_read,
	.write = snps_t1s_write,
	.read_c45 = snps_t1s_read_c45,
	.write_c45 = snps_t1s_write_c45,
};

/* --------------------------------------------------------------------------
 * Device instantiation (one instance per DT node with the binding)
 * -------------------------------------------------------------------------- */

#define SNPS_T1S_PHY_INIT(n)                                                                       \
	static struct snps_t1s_plca_cfg snps_t1s_plca_##n##_cfg = {                                \
		.enable = DT_INST_PROP(n, plca_enable),                                            \
		.node_id = DT_INST_PROP_OR(n, plca_node_id, 0),                                    \
		.node_count = DT_INST_PROP_OR(n, plca_node_count, 8),                              \
		.burst_count = DT_INST_PROP_OR(n, plca_burst_count, 0),                            \
		.burst_timer = DT_INST_PROP_OR(n, plca_burst_timer, 128),                          \
		.to_timer = DT_INST_PROP_OR(n, plca_to_timer, 32),                                 \
	};                                                                                         \
                                                                                                   \
	static const struct snps_t1s_config snps_t1s_##n##_config = {                              \
		.phy_addr = DT_INST_REG_ADDR(n),                                                   \
		.mdio = DEVICE_DT_GET(DT_INST_PARENT(n)),                                          \
		.plca = &snps_t1s_plca_##n##_cfg,                                                  \
		.buf_depth = DT_INST_PROP_OR(n, plca_buf_depth, 99),                               \
		.disable_physical_collision = DT_INST_PROP(n, disable_physical_collision),         \
	};                                                                                         \
                                                                                                   \
	static struct snps_t1s_data snps_t1s_##n##_data;                                           \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &snps_t1s_init, NULL, &snps_t1s_##n##_data,                       \
			      &snps_t1s_##n##_config, POST_KERNEL, CONFIG_PHY_INIT_PRIORITY,       \
			      &snps_t1s_phy_api);

DT_INST_FOREACH_STATUS_OKAY(SNPS_T1S_PHY_INIT)
