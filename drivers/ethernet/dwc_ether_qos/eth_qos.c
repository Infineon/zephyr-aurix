/*
 * Copyright 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT snps_dwc_ether_qos

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(eth_qos, CONFIG_ETHERNET_LOG_LEVEL);

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ethernet/eth_qos.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net_buf.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>

#include <ethernet/eth_stats.h>
#include "eth_qos_reg.h"
#include "eth_qos_priv.h"

static inline bool eth_qos_vlan_check_index(const struct device *dev, uint8_t index,
					    bool *ext_filter)
{
	const struct eth_qos_config *cfg = dev->config;
	uint32_t filter_type =
		FIELD_GET(MAC_HW_FEATURE3_NRVF, sys_read32(cfg->base + MAC_HW_FEATURE3));
	switch (filter_type) {
	case 0:
		*ext_filter = false;
		return index == 0;
	case 1:
		*ext_filter = true;
		return index < 4;
	case 2:
		*ext_filter = true;
		return index < 8;
	case 3:
		*ext_filter = true;
		return index < 16;
	case 4:
		*ext_filter = true;
		return index < 24;
	case 5:
		*ext_filter = true;
		return index < 23;
	default:
		*ext_filter = false;
		return false;
	}
}

int eth_qos_vlan_set_filter(const struct device *dev, uint8_t index,
			    const struct eth_qos_vlan_filter *filter)
{
	const struct eth_qos_config *cfg = dev->config;
	bool ext_filter = false;

	if (!eth_qos_vlan_check_index(dev, index, &ext_filter)) {
		return -EINVAL;
	}

	if (ext_filter) {
		sys_write32(FIELD_PREP(MAC_VLAN_TAG_DATA_VID, filter->tag) |
				    (filter->enable ? MAC_VLAN_TAG_DATA_VEN : 0) |
				    (filter->check_vid ? MAC_VLAN_TAG_DATA_ETV : 0) |
				    FIELD_PREP(MAC_VLAN_TAG_DATA_DMACHN, filter->dma_ch) |
				    MAC_VLAN_TAG_DATA_DMACHEN,
			    cfg->base + MAC_VLAN_TAG_DATA);
		sys_write32((sys_read32(cfg->base + MAC_VLAN_TAG_CTRL) & ~MAC_VLAN_TAG_CTRL_CT &
			     ~MAC_VLAN_TAG_CTRL_OFS) |
				    MAC_VLAN_TAG_CTRL_OB | FIELD_PREP(MAC_VLAN_TAG_CTRL_OFS, index),
			    cfg->base + MAC_VLAN_TAG_CTRL);

		if (!WAIT_FOR((sys_read32(cfg->base + MAC_VLAN_TAG_CTRL) & MAC_VLAN_TAG_CTRL_OB) ==
				      0,
			      10, k_busy_wait(1))) {
			return -EIO;
		}
	} else {
		sys_write32((sys_read32(cfg->base + MAC_VLAN_TAG) & ~MAC_VLAN_TAG_VL &
			     ~MAC_VLAN_TAG_ETV) |
				    (filter->check_vid ? MAC_VLAN_TAG_ETV : 0) |
				    FIELD_PREP(MAC_VLAN_TAG_VL, filter->enable ? filter->tag : 0),
			    cfg->base + MAC_VLAN_TAG);
	}

	return 0;
}

static inline bool eth_qos_mac_check_index(const struct device *dev, uint8_t index)
{
	const struct eth_qos_config *cfg = dev->config;
	uint32_t max_index =
		FIELD_GET(MAC_HW_FEATURE0_ADDMACADRSEL, sys_read32(cfg->base + MAC_HW_FEATURE0));

	return index <= max_index;
}

int eth_qos_mac_set_filter(const struct device *dev, uint8_t index,
			   const struct eth_qos_mac_filter *filter)
{
	const struct eth_qos_config *cfg = dev->config;
	uint32_t reg_val;

	if (!eth_qos_mac_check_index(dev, index)) {
		return -EINVAL;
	}

	reg_val = (filter->addr[5] << 8) | filter->addr[4];
	sys_write32(reg_val | (filter->enable ? MAC_ADDRESSi_HIGH_AE : 0) |
			    (filter->set_dma_ch ? FIELD_PREP(MAC_ADDRESSi_HIGH_DCS, filter->dma_ch)
						: 0),
		    cfg->base + MAC_ADDRESSi_HIGH(index));
	reg_val = (filter->addr[3] << 24) | (filter->addr[2] << 16) | (filter->addr[1] << 8) |
		  filter->addr[0];
	sys_write32(reg_val, cfg->base + MAC_ADDRESSi_LOW(index));

	/* Programm indirect dma sel register if duplication is enabled*/
	if (cfg->da_duplication) {
		sys_write32(filter->dma_ch, cfg->base + MAC_INDIR_ACCESS_CTRL);
		sys_write32(FIELD_PREP(MAC_INDIR_ACCESS_CTRL_MSEL, 0) |
				    FIELD_PREP(MAC_INDIR_ACCESS_CTRL_AOFF, index) |
				    MAC_INDIR_ACCESS_CTRL_OB,
			    cfg->base + MAC_INDIR_ACCESS_CTRL);
		if (!WAIT_FOR((sys_read32(cfg->base + MAC_INDIR_ACCESS_CTRL) &
			       MAC_INDIR_ACCESS_CTRL_OB) == 0,
			      10, k_busy_wait(1))) {
			return -EIO;
		}
	}

	return 0;
}

#if CONFIG_DWMAC_TARGET_TIME_CALLBACK
int eth_qos_pps_set_irq_target_time(const struct device *dev, struct net_ptp_time *ts)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	if ((sys_read32(cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS) &
	     MAC_PPS0_TARGET_TIME_NANOSECONDS_TRGTBUSY0) != 0) {
		return -EIO;
	}

	sys_write32(ts->nanosecond, cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS);
	sys_write32(ts->_sec.low, cfg->base + MAC_PPS0_TARGET_TIME_SECONDS);
	sys_write32((sys_read32(cfg->base + MAC_PPS_CONTROL) & ~MAC_PPS_CONTROL_TRGTMODSEL0) |
			    FIELD_PREP(MAC_PPS_CONTROL_TRGTMODSEL0, 0),
		    cfg->base + MAC_PPS_CONTROL);

	return 0;
}
void eth_qos_pps_set_callback(const struct device *dev, void (*callback)(), void *callback_data)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	data->ts_callback = callback;
	data->ts_callback_data = callback_data;
}
void eth_qos_pps_enable_flexible(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32((sys_read32(cfg->base + MAC_PPS_CONTROL) & ~MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0) |
			    MAC_PPS_CONTROL_PPSEN0,
		    cfg->base + MAC_PPS_CONTROL);
}
int eth_qos_pps_start_single_pulse(const struct device *dev, struct net_ptp_time *ts,
				   uint32_t width)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	if ((sys_read32(cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS) &
	     MAC_PPS0_TARGET_TIME_NANOSECONDS_TRGTBUSY0) != 0) {
		return -EIO;
	}

	uint32_t pps_control =
		(sys_read32(cfg->base + MAC_PPS_CONTROL) & ~MAC_PPS_CONTROL_TRGTMODSEL0) |
		FIELD_PREP(MAC_PPS_CONTROL_TRGTMODSEL0, 3);

	if (FIELD_GET(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0, pps_control) != 0) {
		return -EIO;
	}

	sys_write32(pps_control, cfg->base + MAC_PPS_CONTROL);
	sys_write32(width, cfg->base + MAC_PPS0_WIDTH);
	sys_write32(ts->nanosecond, cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS);
	sys_write32(ts->_sec.low, cfg->base + MAC_PPS0_TARGET_TIME_SECONDS);
	sys_write32(pps_control | FIELD_PREP(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0, 1),
		    cfg->base + MAC_PPS_CONTROL);

	if (!WAIT_FOR(FIELD_GET(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0,
				sys_read32(cfg->base + MAC_PPS_CONTROL)) == 0 &&
			      FIELD_GET(MAC_PPS0_TARGET_TIME_NANOSECONDS_TRGTBUSY0,
					sys_read32(cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS)) ==
				      0,
		      1, k_busy_wait(1))) {
		return -EIO;
	}

	return 0;
}

uint32_t ts_check;
int eth_qos_pps_single_pulse_irq(const struct device *dev, struct net_ptp_time *ts, uint32_t width)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	if ((sys_read32(cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS) &
	     MAC_PPS0_TARGET_TIME_NANOSECONDS_TRGTBUSY0) != 0) {
		return -EIO;
	}

	uint32_t pps_control = sys_read32(cfg->base + MAC_PPS_CONTROL);
	if (FIELD_GET(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0, pps_control) != 0) {
		return -EIO;
	}

	sys_write32((pps_control & ~MAC_PPS_CONTROL_TRGTMODSEL0) |
			    FIELD_PREP(MAC_PPS_CONTROL_TRGTMODSEL0, 0x3),
		    cfg->base + MAC_PPS_CONTROL);
	sys_write32(width, cfg->base + MAC_PPS0_WIDTH);
	sys_write32(ts->nanosecond, cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS);
	sys_write32(ts->_sec.low, cfg->base + MAC_PPS0_TARGET_TIME_SECONDS);
	sys_write32((pps_control & ~MAC_PPS_CONTROL_TRGTMODSEL0) |
			    FIELD_PREP(MAC_PPS_CONTROL_TRGTMODSEL0, 0x2) |
			    FIELD_PREP(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0, 1),
		    cfg->base + MAC_PPS_CONTROL);

	if (!WAIT_FOR(FIELD_GET(MAC_PPS_CONTROL_PPSCTRL0_PPSCMD0,
				sys_read32(cfg->base + MAC_PPS_CONTROL)) == 0 &&
			      FIELD_GET(MAC_PPS0_TARGET_TIME_NANOSECONDS_TRGTBUSY0,
					sys_read32(cfg->base + MAC_PPS0_TARGET_TIME_NANOSECONDS)) ==
				      0,
		      1, k_busy_wait(1))) {
		return -EIO;
	}

	return 0;
}
#endif

static struct k_spinlock net_pkt_slist_lock;
void eth_qos_net_pkt_slist_put(sys_slist_t *list, struct net_pkt *pkt)
{
	k_spinlock_key_t key;

	__ASSERT_NO_MSG(list);
	__ASSERT_NO_MSG(pkt);

	key = k_spin_lock(&net_pkt_slist_lock);
	sys_slist_append(list, &pkt->next);
	k_spin_unlock(&net_pkt_slist_lock, key);
}

struct net_pkt *eth_qos_net_pkt_slist_get(sys_slist_t *list)
{
	sys_snode_t *node;
	struct net_pkt *pkt;
	k_spinlock_key_t key;

	__ASSERT_NO_MSG(list);

	key = k_spin_lock(&net_pkt_slist_lock);
	node = sys_slist_get(list);
	k_spin_unlock(&net_pkt_slist_lock, key);

	pkt = SYS_SLIST_CONTAINER(node, pkt, next);

	return pkt;
}

static void eth_qos_dma_init(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	uint8_t ch;

	if (!cfg->dma_only) {
		eth_qos_dma_set_sysbus(dev);
	}

	for (ch = 0; ch < cfg->dma_tx_channel; ch++) {
		eth_qos_dma_tx_init(dev, ch);
		eth_qos_dma_tx_irq_enable(dev, ch);
	}
	for (ch = 0; ch < cfg->dma_rx_channel; ch++) {
		eth_qos_dma_rx_init(dev, ch);
		eth_qos_dma_rx_irq_enable(dev, ch);
	}
}

static inline void eth_qos_dma_rx_data_init(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_rx[dma_ch];
	struct eth_qos_dma_rx_ch_data *dma_data = &data->dma_rx[dma_ch];

#if CONFIG_MMU
	dma_data->descs_phys = tx_descs_phys + sizeof(struct dwmac_dma_desc) * dma_ch * NB_TX_DESCS;
#endif
	dma_data->head = 0;
	dma_data->tail = 0;
	dma_data->packet = NULL;
	k_sem_init(&dma_data->desc_used, dma_cfg->descs_count - 1, dma_cfg->descs_count - 1);
	sys_slist_init(&dma_data->frags);
}

static void eth_qos_dma_rx_fill_desc(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_rx[dma_ch];
	struct eth_qos_dma_rx_ch_data *dma_data = &data->dma_rx[dma_ch];
	struct net_buf *frag;
	const uint32_t desc_free = k_sem_count_get(&dma_data->desc_used);
	uint32_t descs;
	int ret;

	for (descs = 0; descs < desc_free; descs++) {
		ret = k_sem_take(&dma_data->desc_used, K_NO_WAIT);
		if (ret) {
			break;
		}

		__ASSERT(eth_qos_rdes_own((void *)&dma_cfg->descs[dma_data->tail]) == false,
			 "desc[%d]=0x%x: still owned by HW", dma_data->tail,
			 dma_cfg->descs[dma_data->tail][3]);

		/* Reserve new rx data */
		frag = net_pkt_get_reserve_rx_data(CONFIG_NET_BUF_DATA_SIZE, K_NO_WAIT);
		if (!frag) {
			LOG_DBG("%s: failed to reserve rx data", dev->name);
			k_sem_give(&dma_data->desc_used);
			break;
		}

		net_buf_slist_put(&dma_data->frags, frag);
		eth_qos_rdes_rd_set((void *)&dma_cfg->descs[dma_data->tail], frag);

		dma_data->tail = (dma_data->tail + 1) % (dma_data->desc_used.limit + 1);
	}

	/* Update tail pointer for new descriptors */
	if (descs != 0) {
		eth_qos_dma_rx_set_tail(dev, dma_ch);
	}

	sys_write32(DMA_CHi_STATUS_RBU, cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_rx[dma_ch].nr));
}

static void eth_qos_dma_rx_process(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_rx[dma_ch];
	struct eth_qos_dma_rx_ch_data *dma_data = &data->dma_rx[dma_ch];
	struct net_pkt *pkt = dma_data->packet;
	struct net_buf *frag;
	void *desc;
	int ret;

	while (dma_data->head != dma_data->tail) {
		desc = &dma_cfg->descs[dma_data->head];
		eth_qos_dma_rx_clear_irq(dev, dma_ch);
		/* stop here if hardware still owns it */
		if (eth_qos_rdes_own(desc)) {
			break;
		}

		/* a packet's first descriptor: */
		if (eth_qos_rdes_first(desc)) {
			__ASSERT(pkt == NULL, "Packet should be received before new packet");
			pkt = net_pkt_rx_alloc_on_iface(data->iface, K_NO_WAIT);
			if (!pkt) {
				LOG_DBG("%s: failed to alloc rx packet", dev->name);
				eth_stats_update_errors_rx(data->iface);
			}
		}

		/* retrieve current fragment */
		frag = net_buf_slist_get(&dma_data->frags);

		/* Check for valid packet */
		if (!pkt) {
			net_buf_unref(frag);
			goto next;
		}

		/* Check for descriptor error */
		if (eth_qos_rdes_desc_error(desc)) {
			net_buf_unref(frag);
			if (pkt) {
				net_pkt_unref(pkt);
				pkt = NULL;
			}
			LOG_ERR("%s: Descriptor Definition Error", dev->name);
			goto next;
		}

		/* Handle buffer fragment */
		if (eth_qos_rdes_context(desc)) {
			net_buf_unref(frag);
		} else {
			size_t add_len = eth_qos_rdes_last(desc)
						 ? eth_qos_rdes_packet_length(desc) -
							   net_pkt_get_len(pkt)
						 : CONFIG_NET_BUF_DATA_SIZE;
			size_t room = net_buf_tailroom(frag);

			/*
			 * Clamp to the fragment capacity. A corrupt or error
			 * descriptor can report a length that exceeds the buffer
			 * (or, via unsigned wrap when the reported total is below
			 * what was already received, a huge value); writing it
			 * would overrun the RX buffer pool and corrupt unrelated
			 * memory. The packet error checks below then drop it.
			 */
			if (add_len > room) {
				add_len = room;
			}

			net_buf_add(frag, add_len);
			net_pkt_frag_add(pkt, frag);
			sys_cache_data_invd_range(frag->data, frag->size);
		}

#if defined(CONFIG_NET_PKT_TIMESTAMP)
		/* Handle timestamp from context descriptor */
		if (eth_qos_rdes_context(desc) && eth_qos_rdes_ts_valid(desc)) {
			struct net_ptp_time ts;
			eth_qos_rdes_get_ts(desc, &ts);
			ts._sec.high = eth_qos_ts_get_high(dev);
			net_pkt_set_timestamp(pkt, &ts);
		}
#endif

#if defined(CONFIG_NET_VLAN)
		if (eth_qos_rdes_last(desc) && eth_qos_rdes_vlan_valid(desc)) {
			net_pkt_set_vlan_tci(pkt, eth_qos_rdes_get_tci(desc));
			net_pkt_set_priority(pkt, net_vlan2priority(net_pkt_vlan_priority(pkt)));
		}
#endif

		/* Packet reception error */
		if (eth_qos_rdes_pkt_error(desc)) {
			eth_stats_update_errors_rx(data->iface);
			net_pkt_unref(pkt);
			pkt = NULL;
			goto next;
		}

		if (eth_qos_rdes_pkt_finish(desc)) {
			ret = net_recv_data(data->iface, pkt);
			if ((ret < 0)) {
				LOG_ERR("%s: Failed to receive packet: %d", dev->name, ret);
				net_pkt_unref(pkt);
			}
			pkt = NULL;
			goto next;
		}

next:
		k_sem_give(&dma_data->desc_used);
		dma_data->head = (dma_data->head + 1) % dma_cfg->descs_count;
	}

	/* Store partly processed packet*/
	dma_data->packet = pkt;
}

static void eth_qos_dma_tx_process(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	struct net_buf *frag;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_tx[dma_ch];
	struct eth_qos_dma_tx_ch_data *dma_data = &data->dma_tx[dma_ch];

	while (dma_data->head != dma_data->tail) {
		eth_qos_dma_tx_clear_irq(dev, dma_ch);
		/* stop here if hardware still owns it */
		if (eth_qos_tdes_own((void *)&dma_cfg->descs[dma_data->head])) {
			break;
		}

		/* We are done using the buf */
		frag = net_buf_slist_get(&dma_data->frags);
		net_buf_unref(frag);

		/* last packet descriptor: */
		if (eth_qos_tdes_last((void *)&dma_cfg->descs[dma_data->head])) {
			/* log any errors */
			if (eth_qos_tdes_error((void *)&dma_cfg->descs[dma_data->head])) {
				LOG_ERR("%s: dma error while transmission", dev->name);
				eth_stats_update_errors_tx(data->iface);
			}
#if IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP)
			else {
				eth_qos_dma_ts_handle(dev, dma_ch);
			}
#endif
		}

		dma_data->head = (dma_data->head + 1) % dma_cfg->descs_count;
		k_sem_give(&dma_data->desc_used);
	}
}

static inline void eth_qos_dma_tx_data_init(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_tx[dma_ch];
	struct eth_qos_data *data = dev->data;
	struct eth_qos_dma_tx_ch_data *dma_data = &data->dma_tx[dma_ch];
#if CONFIG_MMU
	dma_data->descs_phys = tx_descs_phys + sizeof(struct dwmac_dma_desc) * dma_ch * NB_TX_DESCS;
#endif
	dma_data->head = 0;
	dma_data->tail = 0;
	k_mutex_init(&dma_data->lock);
	k_sem_init(&dma_data->desc_used, dma_cfg->descs_count - 1,
		   cfg->dma_tx[dma_ch].descs_count - 1);
	sys_slist_init(&dma_data->frags);
}

static inline void eth_qos_mac_init(const struct device *dev)
{
	eth_qos_mac_config(dev);
}

static inline void eth_qos_mtl_init(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	uint32_t queue;

	for (queue = 0; queue < cfg->mtl_rx_queues; queue++) {
		eth_qos_mtl_rxq_init(dev, queue);
	}

	for (queue = 0; queue < cfg->mtl_tx_queues; queue++) {
		eth_qos_mtl_txq_init(dev, queue);
	}

	eth_qos_mtl_set_rxq_ctrl(dev);

	/* set up MTL */
	eth_qos_mtl_set_mode(dev, 0);
}

static void eth_qos_set_link_state(const struct device *dev, struct phy_link_state *link_state)
{
	const struct eth_qos_config *cfg = dev->config;
	uint8_t dma_ch;
	uint8_t ss = eth_qos_mac_get_speed(link_state->speed);
	bool fd = PHY_LINK_IS_FULL_DUPLEX(link_state->speed);

	/* Lock against unwanted changes */
	unsigned int key = arch_irq_lock();

	/* Check if link state changed already. */
	if (eth_qos_mac_rx_state(dev) == link_state->is_up ||
	    eth_qos_mac_tx_state(dev) == link_state->is_up) {
		arch_irq_unlock(key);
		return;
	}

	if (link_state->is_up) {
		/*  Program new phy state*/
		eth_qos_mac_set_link(dev, ss, fd);
	}
	/* Operate TX DMA if clock doesn't stop */
	if (!cfg->clocks_stop) {
		if (link_state->is_up) {
			for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
				eth_qos_dma_tx_start(dev, dma_ch);
			}
		} else {
			/* Disable DMA*/
			for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
				eth_qos_dma_tx_stop(dev, dma_ch);
			}
			/*  Wait for queue emtpy or flush */
			for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
				eth_qos_dma_tx_queue_flush(dev, dma_ch);
			}
		}
	}
	/* Disable or enable receiver*/
	eth_qos_mac_rx_set_state(dev, link_state->is_up);
	/* Disable or enable transmitter and set mac configuration */
	eth_qos_mac_tx_set_state(dev, link_state->is_up);
	arch_irq_unlock(key);
}

static void eth_qos_link_state_changed(const struct device *phy_dev,
				       struct phy_link_state *link_state, void *user_data)
{
	const struct device *dev = user_data;
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	if (link_state->is_up) {
		if (cfg->clocks_stop) {
			eth_qos_mac_wait_idle(dev);
		} else {
			/* TODO: ?*/
		}
		eth_qos_set_link_state(dev, link_state);
		net_if_carrier_on(data->iface);
		LOG_DBG("%s: Link up", dev->name);
	} else {
		eth_qos_set_link_state(dev, link_state);
		net_if_carrier_off(data->iface);
		LOG_DBG("%s: Link down", dev->name);
	}
}

void eth_qos_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;

	data->iface = iface;
	data->started = false;

	if ((data->mac_addr[0] | data->mac_addr[1] | data->mac_addr[2] | data->mac_addr[3] |
	     data->mac_addr[4] | data->mac_addr[5]) == 0U) {
		sys_rand_get(data->mac_addr, sizeof(data->mac_addr));
		data->mac_addr[0] |= 0x02U;
		data->mac_addr[0] &= (uint8_t)~0x01U;
	}

	net_if_set_link_addr(iface, data->mac_addr, sizeof(data->mac_addr), NET_LINK_ETHERNET);
	if (!cfg->dma_only) {
		struct eth_qos_mac_filter filter = {.addr = {0},
						    cfg->da_duplication ? (1 << cfg->dma_rx->nr)
									: cfg->dma_rx->nr,
						    0,
						    true,
						    false,
						    true};
		memcpy(filter.addr, data->mac_addr, 6);
		eth_qos_mac_set_filter(dev, 0, &filter);
	}

	ethernet_init(iface);

	if (cfg->phy) {
		net_if_carrier_off(iface);
		phy_link_callback_set(cfg->phy, &eth_qos_link_state_changed, (void *)dev);
	} else {
		net_if_carrier_on(iface);
	}
}

int eth_qos_start(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	struct phy_link_state state;
	uint8_t dma_ch;

	/* Check if the device is initialized successfully. */
	if (!device_is_ready(dev)) {
		return -EIO;
	}

	/* Start RX DMA operation this is always running. */
	for (dma_ch = 0; dma_ch < cfg->dma_rx_channel; dma_ch++) {
		eth_qos_dma_rx_fill_desc(dev, dma_ch);
		eth_qos_dma_rx_start(dev, dma_ch);
	}
	/* Start Tx operation only if clock stops or slave mode */
	if (cfg->dma_only || cfg->clocks_stop) {
		for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
			eth_qos_dma_tx_start(dev, dma_ch);
		}
	}

	data->started = true;

	/* Check for link status with phy */
	if (cfg->phy) {
		phy_get_link_state(cfg->phy, &state);

		/* If link is up, start initial operation. Afterwards operation is
		 * started and stoped through the link state callback according
		 * to clock requirements. */
		if (state.is_up) {
			net_if_carrier_on(data->iface);
			eth_qos_set_link_state(dev, &state);
		}
	}

	LOG_DBG("%s: Device started", dev->name);

	return 0;
}

static int eth_qos_send(const struct device *dev, struct net_pkt *pkt)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	uint8_t dma_ch = 0;
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_tx[dma_ch];
	struct eth_qos_dma_tx_ch_data *dma_data = &data->dma_tx[dma_ch];
	struct net_buf *prev_frag;
	struct net_buf *frag;
	const size_t pkt_len = net_pkt_get_len(pkt);
	void *tdes;
	size_t frag_count = 0;

	if (!pkt || !pkt->frags) {
		LOG_ERR("%s: cannot TX, invalid argument", dev->name);
		return -EINVAL;
	}

	if (pkt_len == 0) {
		LOG_ERR("%s cannot TX, zero packet length", dev->name);
		// UPDATE_ETH_STATS_TX_ERROR_PKT_CNT(dev_data, 1u);
		return -EINVAL;
	}

	/* Lock DMA Channel */
	k_mutex_lock(&dma_data->lock, K_FOREVER);

#if IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP)
	if (net_pkt_is_tx_timestamping(pkt) || net_pkt_is_ptp(pkt)) {
		/* Reserve tx timestamp if needed */
		if (eth_qos_mac_reserve_tx_ts(dev, dma_ch, pkt, K_MSEC(1000)) != 0) {
			k_mutex_unlock(&dma_data->lock);
			return -ETIMEDOUT;
		}
		net_pkt_ref(pkt);
	}
#endif

	/* Map packet fragments */
	frag = pkt->frags;
	prev_frag = SYS_SLIST_CONTAINER(sys_slist_peek_tail(&dma_data->frags), prev_frag, node);

	while (frag != NULL) {
		/* Check for zero length frags, which would freeze the DMA.*/
		if (frag->len == 0) {
			LOG_WRN("%s: skipping zero-length fragment", dev->name);
			frag = frag->frags;
			continue;
		}

		/* Reserve a free descriptor  */
		if (k_sem_take(&dma_data->desc_used, K_MSEC(1000)) != 0) {
			LOG_ERR("%s: DMA CH%d: Timeout waiting for free TX", dev->name, dma_ch);
			goto abort_frag;
		}

		tdes = &cfg->dma_tx[dma_ch].descs[dma_data->tail];
		eth_qos_tdes_rd_set_frag(tdes, frag);
		eth_qos_tdes_rd_set(
			tdes, frag_count == 0, !frag->frags, pkt_len, data->txcoe_available,
			COND_CODE_1(IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP), (pkt->tx_timestamping || pkt->ptp_pkt), (0)));
		sys_cache_data_flush_range(frag->data, frag->len);
		frag_count++;
		/* Reference frags for DMA use */
		net_buf_slist_put(&data->dma_tx[dma_ch].frags, net_buf_ref(frag));

		dma_data->tail = (dma_data->tail + 1) % (dma_cfg->descs_count);

		frag = frag->frags;
	}

	barrier_dsync_fence_full();

	/* Notify the hardware */
	eth_qos_dma_tx_set_tail(dev, dma_ch);

	/* Unlock DMA Channel */
	k_mutex_unlock(&dma_data->lock);

	return 0;

abort_frag:
	/* Reverse packet fragment mapping */
	dma_data->tail =
		(dma_data->tail - frag_count + dma_cfg->descs_count) % (dma_cfg->descs_count);

	struct net_buf *frag_to_free = pkt->frags;
	while (frag_to_free != frag) {
		k_sem_give(&dma_data->desc_used);
		net_buf_unref(frag_to_free);
		net_buf_slist_remove(&dma_data->frags, prev_frag, frag_to_free);
		frag_to_free = frag_to_free->frags;
	}

	/* Unmap packet */
	net_pkt_unref(pkt);

	/* Unlock DMA Channel */
	k_mutex_unlock(&dma_data->lock);

	return -ETIMEDOUT;
}

#if CONFIG_PTP_CLOCK
const struct device *eth_qos_get_ptp_clock(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	return cfg->ptp_clock;
}
#endif

static const struct device *eth_qos_get_phy(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	return cfg->phy;
}

void eth_qos_common_isr(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	ARG_UNUSED(data);
	uint32_t dma_status = sys_read32(cfg->DMA_BASE + DMA_INTERRUPT_STATUS) & 0xFFFF;
	uint32_t mac_status = sys_read32(cfg->base + MAC_INTERRUPT_STATUS);
	uint32_t mtl_status = sys_read32(cfg->base + MTL_INTERRUPT_STATUS);

	if (mac_status & MAC_INTERRUPT_STATUS_TSIS) {
		uint32_t ts_status;
		do {
			ts_status = sys_read32(cfg->base + MAC_TIMESTAMP_STATUS);
			if (ts_status & MAC_TIMESTAMP_STATUS_TSTARGT0) {
#if CONFIG_DWMAC_TARGET_TIME_CALLBACK
				if (data->ts_callback) {
					data->ts_callback(data->ts_callback_data);
				}
#endif
			}
#if IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP)
			if (ts_status & MAC_TIMESTAMP_STATUS_TXTSSIS) {
				eth_qos_mac_handle_ts(dev);
			}
#endif
		} while (ts_status & MAC_TIMESTAMP_STATUS_TXTSSIS);
	}
	if (dma_status) {
		uint8_t dma_ch;
		for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
			if ((dma_status & (1 << cfg->dma_tx[dma_ch].nr)) == 0) {
				continue;
			}
			uint32_t dma_ch_status =
				sys_read32(cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_tx[dma_ch].nr));
			if (dma_ch_status & DMA_CHi_STATUS_TI) {
				eth_qos_dma_tx_process(dev, dma_ch);
			}
			sys_write32(DMA_CHi_STATUS_NIS | DMA_CHi_STATUS_ETI | DMA_CHi_STATUS_TBU,
				    cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_tx[dma_ch].nr));
		}
		for (dma_ch = 0; dma_ch < cfg->dma_rx_channel; dma_ch++) {
			if ((dma_status & (1 << cfg->dma_rx[dma_ch].nr)) == 0) {
				continue;
			}
			uint32_t dma_ch_status =
				sys_read32(cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_rx[dma_ch].nr));
			if (dma_ch_status & DMA_CHi_STATUS_RI) {
				/* Process descriptors */
				eth_qos_dma_rx_process(dev, dma_ch);
				/* Fill up descriptors again */
				eth_qos_dma_rx_fill_desc(dev, dma_ch);
			}
			sys_write32(DMA_CHi_STATUS_NIS | DMA_CHi_STATUS_ERI,
				    cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_rx[dma_ch].nr));
		}
	}
	if (mtl_status) {
	}
}

#define TX_IRQ(n, _)                                                                               \
	static void __attribute__((used)) eth_qos_tx_ch##n##_isr(const struct device *dev)         \
	{                                                                                          \
		eth_qos_dma_tx_process(dev, n);                                                    \
	}
LISTIFY(8, TX_IRQ, ())

#define RX_IRQ(n, _)                                                                               \
	static void __attribute__((used)) eth_qos_rx_ch##n##_isr(const struct device *dev)         \
	{                                                                                          \
		/* Process descriptors */                                                          \
		eth_qos_dma_rx_process(dev, n);                                                    \
		/* Fill up descriptors again */                                                    \
		eth_qos_dma_rx_fill_desc(dev, n);                                                  \
	}
LISTIFY(8, RX_IRQ, ())

#if defined(CONFIG_NET_STATISTICS_ETHERNET)
struct net_stats_eth *eth_qos_get_stats(const struct device *dev)
{
}
#endif

int eth_qos_stop(const struct device *dev)
{
	return 0;
}

enum ethernet_hw_caps eth_qos_get_capabilities(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	const uint32_t hw_feature0 = sys_read32(cfg->base + MAC_HW_FEATURE0);
	uint32_t caps = 0;

	/* Default settings */
	caps |= ETHERNET_HW_VLAN | ETHERNET_PROMISC_MODE;

	/* Check for slow link capabilites */
	if ((hw_feature0 & MAC_HW_FEATURE0_MIISEL)) {
		caps |= ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE;
	}
	/* Check for 1000 Mbit capability */
	if ((hw_feature0 & MAC_HW_FEATURE0_GMIISEL)) {
		caps |= ETHERNET_LINK_1000BASE;
	}
	if ((hw_feature0 & MAC_HW_FEATURE0_PCSSEL)) {
		caps |= ETHERNET_LINK_2500BASE;
	}
	/* Check for offloading */
	caps |= (hw_feature0 & MAC_HW_FEATURE0_RXCOESEL) ? ETHERNET_HW_RX_CHKSUM_OFFLOAD : 0;
	caps |= (hw_feature0 & MAC_HW_FEATURE0_TXCOESEL) ? ETHERNET_HW_TX_CHKSUM_OFFLOAD : 0;
	/* Check for duplex */
	// caps |= (hw_feature0 & MAC_HW_FEATURE0_HDSEL) ? ETHERNET_DUPLEX : 0;
	/* Check for PTP */
	caps |= (hw_feature0 & MAC_HW_FEATURE0_TSSEL) ? ETHERNET_PTP : 0;

	return caps;
}

int eth_qos_set_config(const struct device *dev, enum ethernet_config_type type,
		       const struct ethernet_config *config)
{
	return 0;
}

int eth_qos_get_config(const struct device *dev, enum ethernet_config_type type,
		       struct ethernet_config *config)
{
	struct eth_qos_data *data = dev->data;

	switch (type) {
	case ETHERNET_CONFIG_TYPE_TX_CHECKSUM_SUPPORT:
		config->chksum_support =
			data->txcoe_available
				? (ETHERNET_CHECKSUM_SUPPORT_IPV4_HEADER |
				   ETHERNET_CHECKSUM_SUPPORT_IPV4_ICMP |
				   ETHERNET_CHECKSUM_SUPPORT_IPV6_HEADER |
				   ETHERNET_CHECKSUM_SUPPORT_IPV6_ICMP |
				   ETHERNET_CHECKSUM_SUPPORT_TCP | ETHERNET_CHECKSUM_SUPPORT_UDP)
				: 0;
	case ETHERNET_CONFIG_TYPE_RX_CHECKSUM_SUPPORT:
		config->chksum_support =
			data->rxcoe_available
				? (ETHERNET_CHECKSUM_SUPPORT_IPV4_HEADER |
				   ETHERNET_CHECKSUM_SUPPORT_IPV4_ICMP |
				   ETHERNET_CHECKSUM_SUPPORT_IPV6_HEADER |
				   ETHERNET_CHECKSUM_SUPPORT_IPV6_ICMP |
				   ETHERNET_CHECKSUM_SUPPORT_TCP | ETHERNET_CHECKSUM_SUPPORT_UDP)
				: 0;
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}

#if defined(CONFIG_NET_VLAN)
int eth_qos_vlan_setup(const struct device *dev, struct net_if *iface, uint16_t tag, bool enable)
{
	return 0;
}
#endif /* CONFIG_NET_VLAN */

int eth_qos_init(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	uint32_t dma_ch;

	if (cfg->skip_init) {
		return 0;
	}

	if (!cfg->dma_only && cfg->phy != NULL && !device_is_ready(cfg->phy)) {
		LOG_ERR("%s: PHY device is not ready", dev->name);
		return -EFAULT;
	}

	if (cfg->do_reset) {
		sys_write32(DMA_MODE_SWR, cfg->DMA_BASE + DMA_MODE);
	}
	if (!WAIT_FOR((sys_read32(cfg->DMA_BASE + DMA_MODE) & DMA_MODE_SWR) == 0, 1000,
		      k_busy_wait(10))) {
		LOG_ERR("Failed to reset mac");
		return -EIO;
	}

	cfg->init();

	uint32_t hw_feature0 = sys_read32(cfg->base + MAC_HW_FEATURE0);
	data->rxcoe_available = (hw_feature0 & MAC_HW_FEATURE0_RXCOESEL) != 0;
	data->txcoe_available = (hw_feature0 & MAC_HW_FEATURE0_TXCOESEL) != 0;

	sys_write32(sys_read32(cfg->base + MAC_INTERRUPT_ENABLE) | MAC_INTERRUPT_ENABLE_TSIE,
		    cfg->base + MAC_INTERRUPT_ENABLE);

	for (dma_ch = 0; dma_ch < cfg->dma_tx_channel; dma_ch++) {
		eth_qos_dma_tx_data_init(dev, dma_ch);
	}

	for (dma_ch = 0; dma_ch < cfg->dma_rx_channel; dma_ch++) {
		eth_qos_dma_rx_data_init(dev, dma_ch);
	}

#if IS_ENABLED(CONFIG_PTP_CLOCK)
	k_sem_init(&data->ptp_pkt_sem, 1, 1);
#endif

	if (!cfg->dma_only) {
		sys_write32(0xFFFFFFFF, cfg->base + MMC_FPE_RX_INTERRUPT_MASK);
		sys_write32(0xFFFFFFFF, cfg->base + MMC_FPE_TX_INTERRUPT_MASK);
		sys_write32(0xFFFFFFFF, cfg->base + MMC_IPC_RX_INTERRUPT_MASK);

		eth_qos_mac_init(dev);
		eth_qos_mtl_init(dev);
	}
	eth_qos_dma_init(dev);

	return 0;
}

struct ethernet_api eth_qos_api = {
	.iface_api.init = eth_qos_iface_init,
	.start = eth_qos_start,
	.stop = eth_qos_stop,
	.get_config = eth_qos_get_config,
	.get_capabilities = eth_qos_get_capabilities,
	.set_config = eth_qos_set_config,
#if defined(CONFIG_NET_VLAN)
	.vlan_setup = eth_qos_vlan_setup,
#endif
#if defined(CONFIG_PTP_CLOCK)
	.get_ptp_clock = eth_qos_get_ptp_clock,
#endif
	.get_phy = eth_qos_get_phy,
	.send = eth_qos_send,
};

#define ETH_QOS_INIT(n)                                                                            \
	ETH_QOS_INIT_FUNC(n)                                                                       \
	ETH_QOS_DMA_INIT(n);                                                                       \
	ETH_QOS_MTL_INIT(n);                                                                       \
	sys_slist_t eth_qos_ptp_pkts_##n[ETH_QOS_TX_CHANNELS_TO_USE(n)];                           \
	static struct eth_qos_data eth_qos_data_##n = ETH_QOS_DATA(n);                             \
	static struct eth_qos_config eth_qos_config_##n = ETH_QOS_CONFIG(n);                       \
                                                                                                   \
	ETH_NET_DEVICE_DT_INST_DEFINE(n, eth_qos_init, NULL, &eth_qos_data_##n,                    \
				      &eth_qos_config_##n, CONFIG_ETH_INIT_PRIORITY, &eth_qos_api, \
				      NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(ETH_QOS_INIT)
