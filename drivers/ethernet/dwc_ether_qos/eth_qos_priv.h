/*
 * Copyright 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ETH_QOS_PRIH_H
#define ETH_QOS_PRIH_H

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "eth_qos_reg.h"
#include "zephyr/devicetree.h"

struct eth_qos_dma_tx_ch_data {
	/** Descriptor used semphore */
	struct k_sem desc_used;
	/** Head index */
	uint16_t head;
	/** Tail index */
	uint16_t tail;
	/** List of frags in DMA Channel */
	sys_slist_t frags;
#if CONFIG_MMU
	/** Physical address to descriptor ring */
	uintptr_t *descs_phys;
#endif
	/** Channel mutext */
	struct k_mutex lock;
};

struct eth_qos_dma_rx_ch_data {
	/** Descriptor used semphore */
	struct k_sem desc_used;
	/** Head index */
	uint16_t head;
	/** Tail index */
	uint16_t tail;
	/** List of frags in DMA Channel */
	sys_slist_t frags;
#if CONFIG_MMU
	/** Physical address to descriptor ring */
	uintptr_t *descs_phys;
#endif
	/** Currently processed packet */
	struct net_pkt *packet;
	/** Owning device, for deferred refill work */
	const struct device *dev;
	/** DMA channel index, for deferred refill work */
	uint8_t ch;
	/** Serializes refill between ISR and refill work */
	struct k_spinlock lock;
	/** Deferred refill when RX buffers are momentarily exhausted */
	struct k_work_delayable refill_work;
};

typedef uint32_t __attribute__((aligned(16))) eth_qos_desc[4];
struct eth_qos_dma_ch_config {
	/** Pointer to descriptor ring */
	eth_qos_desc *descs;
	/** Descriptor count */
	uint16_t descs_count;
	/** DMA Channel Number */
	uint8_t nr;
};

struct eth_qos_rx_queue {
	uint16_t size;
	uint8_t nr;
	uint8_t priority;
	uint8_t dma_channel;
	uint8_t threshold;

	/* Queue config flags*/
	uint8_t sf: 1;
	uint8_t dynamic_dma_channel: 1;
	uint8_t av_queue: 1;

	/* Queue routing flags */
	uint8_t route_multi_broad: 1;
};

struct eth_qos_tx_queue {
	uint16_t size;
	uint8_t nr;
	uint8_t priority;
	uint8_t threshold;

	/** Operate on second frame flag */
	uint8_t osf: 1;

	uint8_t sf: 1;
};

void eth_qos_net_pkt_slist_put(sys_slist_t *list, struct net_pkt *pkt);
struct net_pkt *eth_qos_net_pkt_slist_get(sys_slist_t *list);

#if CONFIG_ETH_QOS_SHARED_DMA
#define DMA_BASE dma_base
#else
#define DMA_BASE base
#endif

struct eth_qos_config {
	/* Device address */
	/** MAC Base address */
	mm_reg_t base;
#if CONFIG_ETH_QOS_SHARED_DMA
	mm_reg_t dma_base;
#endif

	/* Device nodes */
	/** Phy deivce */
	const struct device *phy;
#if CONFIG_PTP_CLOCK
	/** PTP Clock Device */
	const struct device *ptp_clock;
#endif

	/* Init parameters */
	/** Interface stops RX and TX clocks during link changes */
	bool clocks_stop;
	/** skip init for MAC and MTL */
	bool dma_only;
	/** Skip init completely */
	bool skip_init;
	/** Perform reset during init */
	bool do_reset;
	/** Destination address based duplication */
	bool da_duplication;
	/** MAC loopback */
	bool loopback;
	/** Drop TX Status in MTL */
	bool drop_tx_status;

	/** Number of RX DMA Channels used */
	uint8_t dma_rx_channel;
	/** Number of TX DMA Channel used */
	uint8_t dma_tx_channel;
	uint8_t mtl_rx_queues;
	uint8_t mtl_tx_queues;
	/** RX DMA Channel configurations */
	struct eth_qos_dma_ch_config *dma_rx;
	/** TX DMA Channel configurations  */
	struct eth_qos_dma_ch_config *dma_tx;
	struct eth_qos_rx_queue *mtl_rx;
	struct eth_qos_tx_queue *mtl_tx;

	/** Device specific configuration init function */
	void (*init)();

	/** AXI Burst Length enable bit field */
	uint8_t blen;

	/** Address aligned beats */
	bool aal;
	/** First burst length */
	bool fb;
};

struct eth_qos_data {
	struct net_if *iface;

	struct eth_qos_dma_rx_ch_data *dma_rx;
	struct eth_qos_dma_tx_ch_data *dma_tx;

	struct k_mutex cfg_mutex;

#if CONFIG_ETH_QOS_TARGET_TIME_CALLBACK
	void (*ts_callback)();
	void *ts_callback_data;
#endif

	uint8_t mac_addr[6];

	bool started;
	bool txcoe_available;
	bool rxcoe_available;

#if IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP)
	struct k_sem ptp_pkt_sem;
	struct net_pkt *ptp_pkt;
	sys_slist_t *ptp_pkts;
#endif
};

struct eth_qos_rdes_rd {
	struct eth_qos_rdes_rd0 {
		uint32_t buf1ap;
	} rdes0;
	struct eth_qos_rdes_rd1 {
		uint32_t buf1ap_high;
	} rdes1;
	struct eth_qos_rdes_rd2 {
		uint32_t buf2ap;
	} rdes2;
	struct eth_qos_rdes_rd3 {
		uint32_t: 24;
		uint32_t buf1v: 1;
		uint32_t buf2v: 1;
		uint32_t: 4;
		uint32_t ioc: 1;
		uint32_t own: 1;
	} rdes3;
} __attribute__((aligned(16)));

struct eth_qos_rdes_wb {
	struct eth_qos_rdes_wb0 {
		uint32_t ovt: 16;
		uint32_t ivt: 16;
	} rdes0;
	struct eth_qos_rdes_wb1 {
		uint32_t pt: 3;
		uint32_t iphe: 1;
		uint32_t ipv4: 1;
		uint32_t ipv6: 1;
		uint32_t ipcb: 1;
		uint32_t ipce: 1;
		uint32_t pmt: 4;
		uint32_t pft: 1;
		uint32_t pv: 1;
		uint32_t tsa: 1;
		uint32_t td: 1;
		uint32_t opc: 16;
	} rdes1;
	struct eth_qos_rdes_wb2 {
		uint32_t hl: 10;
		uint32_t arpnr: 1;
		uint32_t ios: 3;
		uint32_t its: 1;
		uint32_t ots: 1;
		uint32_t saf: 1;
		uint32_t daf: 1;
		uint32_t hf: 1;
		uint32_t madrm: 8;
		uint32_t l3fm: 1;
		uint32_t l4fm: 1;
		uint32_t l3l4fm: 3;
	} rdes2;
	struct eth_qos_rdes_wb3 {
		uint32_t pl: 15;
		uint32_t es: 1;
		uint32_t lt: 3;
		uint32_t de: 1;
		uint32_t re: 1;
		uint32_t oe: 1;
		uint32_t rwt: 1;
		uint32_t gp: 1;
		uint32_t ce: 1;
		uint32_t rs0v: 1;
		uint32_t rs1v: 1;
		uint32_t rs2v: 1;
		uint32_t ld: 1;
		uint32_t fd: 1;
		uint32_t ctxt: 1;
		uint32_t own: 1;
	} rdes3;
} __attribute__((aligned(16)));

struct eth_qos_rdes_ctx {
	uint32_t rdes0;
	uint32_t rdes1;
	uint32_t rdes2;
	struct eth_qos_rdes_ctx3 {
		uint32_t: 29;
		uint32_t de: 1;
		uint32_t ctxt: 1;
		uint32_t own: 1;
	} rdes3;
} __attribute__((aligned(16)));

union eth_qos_rdes {
	struct eth_qos_rdes_rd rd;
	struct eth_qos_rdes_wb wb;
	struct eth_qos_rdes_ctx ctx;
};

static inline bool eth_qos_rdes_own(struct eth_qos_rdes_rd *rdes)
{
	return rdes->rdes3.own == 1;
}

static inline bool eth_qos_rdes_first(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.fd == 1;
}

static inline bool eth_qos_rdes_last(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.ld == 1;
}

static inline bool eth_qos_rdes_context(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.ctxt == 1;
}

static inline bool eth_qos_rdes_pkt_error(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.ld == 1 & rdes->rdes3.es == 1;
}

static inline bool eth_qos_rdes_pkt_finish(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.ctxt == 1 ||
	       (rdes->rdes3.ld == 1 && (!IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP) ||
					!(rdes->rdes3.rs1v == 1 && rdes->rdes1.tsa == 1)));
}

static inline bool eth_qos_rdes_desc_error(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.fd == 1 && rdes->rdes3.ctxt == 1;
}

static inline uint16_t eth_qos_rdes_packet_length(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes3.pl;
}

static inline bool eth_qos_rdes_vlan_valid(struct eth_qos_rdes_wb *rdes)
{
	/* Only check c-tag and double tagged with inner c-tag*/
	return rdes->rdes3.rs0v == 1 && rdes->rdes3.rs2v == 1 &&
	       (rdes->rdes2.ots == 1 || rdes->rdes2.its == 1);
}

static inline uint16_t eth_qos_rdes_get_tci(struct eth_qos_rdes_wb *rdes)
{
	return rdes->rdes2.its == 1 ? rdes->rdes0.ivt : rdes->rdes0.ovt;
}

#if defined(CONFIG_NET_PKT_TIMESTAMP)
static inline bool eth_qos_rdes_ts_valid(struct eth_qos_rdes_ctx *rdes)
{
	return rdes->rdes3.ctxt == 1 && rdes->rdes3.de == 0 && rdes->rdes0 != 0xFFFFFFFF &&
	       rdes->rdes1 != 0xFFFFFFFF;
}

static inline void eth_qos_rdes_get_ts(struct eth_qos_rdes_ctx *rdes, struct net_ptp_time *ts)
{
	ts->nanosecond = rdes->rdes0;
	ts->second = rdes->rdes1;
}
#endif

static inline void eth_qos_rdes_rd_set(struct eth_qos_rdes_rd *rdes, struct net_buf *frag)
{
	rdes->rdes0.buf1ap = (uint32_t)POINTER_TO_UINT(frag->data);
#if UINTPTR_MAX == UINT64_MAX
	rdes->rdes1.buf2ap = (uint32_t)(POINTER_TO_UINT(frag->data) >> 32u);
#endif
	rdes->rdes3 = (struct eth_qos_rdes_rd3){.ioc = 1, .own = 1, .buf1v = 1};
}

static inline void eth_qos_dma_rx_set_tail(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	void *const tail_ptr = &cfg->dma_rx[dma_ch].descs[data->dma_rx[dma_ch].tail];

	sys_write32((uint32_t)POINTER_TO_UINT(tail_ptr),
		    cfg->DMA_BASE + DMA_CHi_RXDESC_TAIL_POINTER(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_rx_init(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;

#if UINTPTR_MAX != UINT32_MAX
	sys_write32((uint32_t)(POINTER_TO_UINT(cfg->dma_rx[dma_ch].descs) >> 32),
		    cfg->DMA_BASE + DMA_CHi_RXDESC_LIST_HADDRESS(cfg->dma_tx[dma_ch].nr));
#endif
	sys_write32((uint32_t)POINTER_TO_UINT(cfg->dma_rx[dma_ch].descs),
		    cfg->DMA_BASE + DMA_CHi_RXDESC_LIST_ADDRESS(cfg->dma_tx[dma_ch].nr));
	sys_write32(FIELD_PREP(DMA_CHi_RX_CONTROL_RBSZ, CONFIG_NET_BUF_DATA_SIZE),
		    cfg->DMA_BASE + DMA_CHi_RX_CONTROL(cfg->dma_tx[dma_ch].nr));
	sys_write32(
		FIELD_PREP(DMA_CHi_RXDESC_RING_LENGTH_RDRL, cfg->dma_rx[dma_ch].descs_count - 1),
		cfg->DMA_BASE + DMA_CHi_RXDESC_RING_LENGTH(cfg->dma_tx[dma_ch].nr));
	sys_write32((uint32_t)POINTER_TO_UINT(cfg->dma_rx[dma_ch].descs),
		    cfg->DMA_BASE + DMA_CHi_RXDESC_TAIL_POINTER(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_rx_start(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32((sys_read32(cfg->DMA_BASE + DMA_CHi_RX_CONTROL(cfg->dma_tx[dma_ch].nr))) |
			    DMA_CHi_RX_CONTROL_SR,
		    cfg->DMA_BASE + DMA_CHi_RX_CONTROL(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_rx_irq_enable(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32(sys_read32(cfg->DMA_BASE + DMA_CHi_INTERRUPT_ENABLE(cfg->dma_tx[dma_ch].nr)) |
#if !IS_ENABLED(CONFIG_ETH_DWC_ETHER_QOS_DMA_PER_CH_IRQ)
			    DMA_CHi_INTERRUPT_ENABLE_RIE |
#endif
			    DMA_CHi_INTERRUPT_ENABLE_NIE | DMA_CHi_INTERRUPT_ENABLE_FBEE |
			    DMA_CHi_INTERRUPT_ENABLE_CDEE | DMA_CHi_INTERRUPT_ENABLE_AIE |
			    DMA_CHi_INTERRUPT_ENABLE_RBUE,
		    cfg->DMA_BASE + DMA_CHi_INTERRUPT_ENABLE(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_rx_clear_irq(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32(DMA_CHi_STATUS_RI, cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_rx[dma_ch].nr));
}

struct eth_qos_tdes_rd {
	struct eth_qos_tdes_rd0 {
		uint32_t buf1ap;
	} tdes0;
	struct eth_qos_tdes_rd1 {
		uint32_t buf2ap;
	} tdes1;
	struct eth_qos_tdes_rd2 {
		uint32_t b1l: 14;
		uint32_t vtir: 2;
		uint32_t b2l: 14;
		uint32_t ttse: 1;
		uint32_t ioc: 1;
	} tdes2;
	struct eth_qos_tdes_rd3 {
		uint32_t fl: 15;
		uint32_t tpl: 1;
		uint32_t cic: 2;
		uint32_t tse: 1;
		uint32_t slotnum: 4;
		uint32_t saic: 3;
		uint32_t cpc: 2;
		uint32_t ld: 1;
		uint32_t fd: 1;
		uint32_t ctxt: 1;
		uint32_t own: 1;
	} tdes3;
} __attribute__((aligned(16)));

struct eth_qos_tdes_wb {
	struct eth_qos_tdes_wb0 {
		uint32_t ttsl;
	} tdes0;
	struct eth_qos_tdes_wb1 {
		uint32_t ttsh;
	} tdes1;
	struct eth_qos_tdes_wb2 {
		uint32_t: 32;
	} tdes2;
	struct eth_qos_tdes_wb3 {
		uint32_t ihe: 1;
		uint32_t db: 1;
		uint32_t uf: 1;
		uint32_t ed: 1;
		uint32_t cc: 4;
		uint32_t ec: 1;
		uint32_t lc: 1;
		uint32_t nc: 1;
		uint32_t loc: 1;
		uint32_t pce: 1;
		uint32_t ff: 1;
		uint32_t jt: 1;
		uint32_t es: 1;
		uint32_t: 1;
		uint32_t ttss: 1;
		uint32_t: 5;
		uint32_t derr: 1;
		uint32_t: 4;
		uint32_t ld: 1;
		uint32_t fd: 1;
		uint32_t ctxt: 1;
		uint32_t own: 1;
	} tdes3;
} __attribute__((aligned(16)));

union eth_qos_tdes {
	struct eth_qos_tdes_rd rd;
	struct eth_qos_tdes_wb wb;
};

static inline void eth_qos_tdes_rd_set(struct eth_qos_tdes_rd *tdes, bool fd, bool ld, uint16_t len,
				       bool txcoe, bool ts)
{
	tdes->tdes3 = (struct eth_qos_tdes_rd3){
		.fd = fd, .ld = ld, .own = 1, .fl = len, .cic = txcoe ? 0x3 : 0};
	tdes->tdes2.ioc = ld;
	tdes->tdes2.ttse = ts;
}

static inline void eth_qos_tdes_rd_set_frag(struct eth_qos_tdes_rd *tdes, struct net_buf *frag)
{
	tdes->tdes2.b1l = frag->len;
	tdes->tdes2.b2l = 0;
	tdes->tdes0.buf1ap = (uint32_t)POINTER_TO_UINT(frag->data);
#if UINTPTR_MAX == UINT64_MAX
	tdes->tdes1.buf2ap = (uint32_t)(POINTER_TO_UINT(frag->data) >> 32u);
#endif
}

static inline bool eth_qos_tdes_own(void *tdes)
{
	return ((struct eth_qos_tdes_rd *)tdes)->tdes3.own;
}

static inline bool eth_qos_tdes_last(void *tdes)
{
	return ((struct eth_qos_tdes_wb *)tdes)->tdes3.ld;
}

static inline bool eth_qos_tdes_error(void *tdes)
{
	return ((struct eth_qos_tdes_wb *)tdes)->tdes3.derr;
}

static inline bool eth_qos_dma_tdes_has_ts(void *tdes)
{
	return ((struct eth_qos_tdes_wb *)tdes)->tdes3.ld &&
	       ((struct eth_qos_tdes_wb *)tdes)->tdes3.ttss;
}

void eth_qos_dma_tdes_get_ts(void *tdes, struct net_ptp_time *ts)
{
	struct eth_qos_tdes_wb *tdes_wb = (struct eth_qos_tdes_wb *)tdes;
	ts->nanosecond = tdes_wb->tdes0.ttsl;
	ts->second = tdes_wb->tdes1.ttsh;
}

static inline void eth_qos_dma_set_sysbus(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32((UINTPTR_MAX != UINT32_MAX ? DMA_SYSBUS_MODE_EAME : 0) |
			    (cfg->aal ? DMA_SYSBUS_MODE_AAL : 0) |
			    (cfg->fb ? DMA_SYSBUS_MODE_FB : 0) | cfg->blen << 1,
		    cfg->DMA_BASE + DMA_SYSBUS_MODE);
}

#if IS_ENABLED(CONFIG_NET_PKT_TIMESTAMP)
static inline struct net_pkt *eth_qos_dma_ts_get_pkt(const struct device *dev, uint8_t dma_ch)
{
	struct eth_qos_data *data = dev->data;
	sys_snode_t *node = sys_slist_get(&data->ptp_pkts[dma_ch]);
	struct net_pkt *pkt;

	pkt = SYS_SLIST_CONTAINER(node, pkt, next);

	return pkt;
}

static inline void eth_qos_dma_ts_handle(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	struct eth_qos_dma_tx_ch_data *dma_data = &data->dma_tx[dma_ch];
	const struct eth_qos_dma_ch_config *dma_cfg = &cfg->dma_tx[dma_ch];

	if (eth_qos_dma_tdes_has_ts((void *)&dma_cfg->descs[dma_data->head])) {
		struct net_pkt *pkt = eth_qos_dma_ts_get_pkt(dev, dma_ch);
		if (pkt) {
			struct net_ptp_time ts;
			eth_qos_dma_tdes_get_ts((void *)&dma_cfg->descs[dma_data->head], &ts);
			net_pkt_set_timestamp(pkt, &ts);
			net_if_add_tx_timestamp(pkt);
		} else {
			LOG_ERR("%s: failed to get timestamp packet", dev->name);
		}
		net_pkt_unref(pkt);
	}
}
#endif

static inline void eth_qos_dma_tx_set_tail(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	struct eth_qos_data *data = dev->data;
	void *const tail_ptr = &cfg->dma_tx[dma_ch].descs[data->dma_tx[dma_ch].tail];

	sys_write32((uint32_t)POINTER_TO_UINT(tail_ptr),
		    cfg->DMA_BASE + DMA_CHi_TXDESC_TAIL_POINTER(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_queue_flush(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32((sys_read32(cfg->base + MTL_TXQi_OPERATION_MODE(cfg->dma_tx[dma_ch].nr))) |
			    MTL_TXQi_OPERATION_MODE_TQS,
		    cfg->base + MTL_TXQi_OPERATION_MODE(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_init(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;

#if UINTPTR_MAX != UINT32_MAX
	sys_write32((uint32_t)(POINTER_TO_UINT(cfg->dma_tx[dma_ch].descs) >> 32),
		    cfg->DMA_BASE + DMA_CHi_TXDESC_LIST_HADDRESS(cfg->dma_tx[dma_ch].nr));
#endif
	sys_write32((uint32_t)POINTER_TO_UINT(cfg->dma_tx[dma_ch].descs),
		    cfg->DMA_BASE + DMA_CHi_TXDESC_LIST_ADDRESS(cfg->dma_tx[dma_ch].nr));
	sys_write32(
		FIELD_PREP(DMA_CHi_TXDESC_RING_LENGTH_TDRL, cfg->dma_tx[dma_ch].descs_count - 1),
		cfg->DMA_BASE + DMA_CHi_TXDESC_RING_LENGTH(cfg->dma_tx[dma_ch].nr));
	sys_write32((uint32_t)POINTER_TO_UINT(cfg->dma_tx[dma_ch].descs),
		    cfg->DMA_BASE + DMA_CHi_TXDESC_TAIL_POINTER(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_start(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	const struct eth_qos_tx_queue *tx_queue = &cfg->mtl_tx[dma_ch];

	sys_write32((sys_read32(cfg->DMA_BASE + DMA_CHi_TX_CONTROL(cfg->dma_tx[dma_ch].nr))) |
			    (tx_queue->osf ? DMA_CHi_TX_CONTROL_OSF : 0) | DMA_CHi_TX_CONTROL_ST,
		    cfg->DMA_BASE + DMA_CHi_TX_CONTROL(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_stop(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32((sys_read32(cfg->DMA_BASE + DMA_CHi_TX_CONTROL(cfg->dma_tx[dma_ch].nr)) &
		     ~DMA_CHi_TX_CONTROL_ST),
		    cfg->DMA_BASE + DMA_CHi_TX_CONTROL(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_irq_enable(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32(sys_read32(cfg->DMA_BASE + DMA_CHi_INTERRUPT_ENABLE(cfg->dma_tx[dma_ch].nr)) |
#if !IS_ENABLED(CONFIG_ETH_DWC_ETHER_QOS_DMA_PER_CH_IRQ)
			    DMA_CHi_INTERRUPT_ENABLE_TIE |
#endif
			    DMA_CHi_INTERRUPT_ENABLE_NIE | DMA_CHi_INTERRUPT_ENABLE_FBEE |
			    DMA_CHi_INTERRUPT_ENABLE_CDEE | DMA_CHi_INTERRUPT_ENABLE_AIE,
		    cfg->DMA_BASE + DMA_CHi_INTERRUPT_ENABLE(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_dma_tx_clear_irq(const struct device *dev, uint8_t dma_ch)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32(DMA_CHi_STATUS_TI, cfg->DMA_BASE + DMA_CHi_STATUS(cfg->dma_tx[dma_ch].nr));
}

static inline void eth_qos_mac_config(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	sys_write32(MAC_CONFIGURATION_IPC | MAC_CONFIGURATION_CST | MAC_CONFIGURATION_ACS |
			    (cfg->loopback ? MAC_CONFIGURATION_LM : 0),
		    cfg->base + MAC_CONFIGURATION);
	sys_write32(MAC_PACKET_FILTER_PM, cfg->base + MAC_PACKET_FILTER);
	sys_write32((cfg->da_duplication ? MAC_EXT_CONFIGURATION_PDC : 0),
		    cfg->base + MAC_EXT_CONFIGURATION);

	/* TODO: Handle VLAN */
	/* TODO: Handle L3, L4 */
#if 0
	/* Insert default filter all vlan filter */
	if (p->feature3 & MAC_HW_FEATURE3_NRVF) {
		dwmac_vlan_write_ext_filter(dev, 0, true, 0x3ff);
	} else {
		REG_WRITE(MAC_VLAN_TAG, MAC_VLAN_TAG_CTRL_EVLRXS |
						FIELD_PREP(MAC_VLAN_TAG_CTRL_EVLS, 3) |
						FIELD_PREP(MAC_VLAN_TAG_CTRL_VL, 0x3ff));
	}
#endif
}

static inline void eth_qos_mac_set_link(const struct device *dev, uint32_t ss, bool fd)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32((sys_read32(cfg->base + MAC_CONFIGURATION) &
		     ~(MAC_CONFIGURATION_DM | MAC_CONFIGURATION_FES | MAC_CONFIGURATION_PS)) |
			    (fd ? MAC_CONFIGURATION_DM : 0) |
			    FIELD_PREP(MAC_CONFIGURATION_FES | MAC_CONFIGURATION_PS, ss),
		    cfg->base + MAC_CONFIGURATION);
}

static inline bool eth_qos_mac_rx_state(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	return (sys_read32(cfg->base + MAC_CONFIGURATION) & MAC_CONFIGURATION_RE) != 0;
}

static inline void eth_qos_mac_rx_set_state(const struct device *dev, bool up)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32((sys_read32(cfg->base + MAC_CONFIGURATION) & ~MAC_CONFIGURATION_RE) |
			    (up ? MAC_CONFIGURATION_RE : 0),
		    cfg->base + MAC_CONFIGURATION);
}

static inline bool eth_qos_mac_tx_state(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	return (sys_read32(cfg->base + MAC_CONFIGURATION) & MAC_CONFIGURATION_TE) != 0;
}

static inline void eth_qos_mac_tx_set_state(const struct device *dev, bool up)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32((sys_read32(cfg->base + MAC_CONFIGURATION) & ~MAC_CONFIGURATION_TE) |
			    (up ? MAC_CONFIGURATION_TE : 0),
		    cfg->base + MAC_CONFIGURATION);
}

static inline uint8_t eth_qos_mac_get_speed(enum phy_link_speed link_speed)
{
	switch (link_speed) {
	case LINK_HALF_10BASE:
	case LINK_FULL_10BASE:
		return 0x2;
	case LINK_HALF_100BASE:
	case LINK_FULL_100BASE:
		return 0x3;
	case LINK_HALF_1000BASE:
	case LINK_FULL_1000BASE:
		return 0x0;
	case LINK_FULL_2500BASE:
		return 0x1;
	default:
		return 0;
	}
}

static inline void eth_qos_mac_wait_idle(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	WAIT_FOR(sys_read32(cfg->base + MAC_DEBUG) == 0, 1000, k_busy_wait(100));
}

#if defined(CONFIG_NET_PKT_TIMESTAMP)
static inline int eth_qos_mac_reserve_tx_ts(const struct device *dev, uint8_t dma_ch,
					    struct net_pkt *pkt, k_timeout_t timeout)
{
	struct eth_qos_data *data = dev->data;

	if (0) {
		if (k_sem_take(&data->ptp_pkt_sem, timeout) != 0) {
			return -ETIMEDOUT;
		}

		data->ptp_pkt = pkt;
	} else {
		sys_slist_append(&data->ptp_pkts[dma_ch], &pkt->next);
	}

	return 0;
}

static inline void eth_qos_mac_get_ts(const struct device *dev, struct net_ptp_time *ts)
{
	const struct eth_qos_config *cfg = dev->config;

	ts->nanosecond = FIELD_GET(MAC_TX_TIMESTAMP_STATUS_NANOSECONDS_TXTSSLO,
				   sys_read32(cfg->base + MAC_TX_TIMESTAMP_STATUS_NANOSECONDS));
	ts->_sec.low = sys_read32(cfg->base + MAC_TX_TIMESTAMP_STATUS_SECONDS);
	ts->_sec.high = sys_read32(cfg->base + MAC_SYSTEM_TIME_HIGHER_WORD_SECONDS);
}

static inline void eth_qos_mac_handle_ts(const struct device *dev)
{
	struct eth_qos_data *data = dev->data;
	struct net_pkt *pkt = data->ptp_pkt;
	struct net_ptp_time ts;

	__ASSERT_NO_MSG(pkt);
	eth_qos_mac_get_ts(dev, &ts);
	net_pkt_set_timestamp(pkt, &ts);
	net_if_add_tx_timestamp(pkt);
	k_sem_give(&data->ptp_pkt_sem);

	net_pkt_unref(pkt);
}

static inline uint16_t eth_qos_ts_get_high(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;

	return sys_read32(cfg->base + MAC_SYSTEM_TIME_HIGHER_WORD_SECONDS);
}
#endif

static inline void eth_qos_mtl_set_mode(const struct device *dev, uint8_t schalg)
{
	const struct eth_qos_config *cfg = dev->config;
	sys_write32(FIELD_PREP(MTL_OPERATION_MODE_SCHALG, schalg), cfg->base + MTL_OPERATION_MODE);
}

static inline void eth_qos_mtl_rxq_init(const struct device *dev, uint8_t queue)
{
	const struct eth_qos_config *cfg = dev->config;
	const struct eth_qos_rx_queue *qcfg = &cfg->mtl_rx[queue];

	sys_write32(FIELD_PREP(MTL_RXQi_OPERATION_MODE_RQS, qcfg->size / 256) |
			    (qcfg->sf ? MTL_RXQi_OPERATION_MODE_RSF
				      : (qcfg->threshold << MTL_RXQi_OPERATION_MODE_RTC)),
		    cfg->base + MTL_RXQi_OPERATION_MODE(qcfg->nr));
}

static inline void eth_qos_mtl_set_rxq_ctrl(const struct device *dev)
{
	const struct eth_qos_config *cfg = dev->config;
	uint8_t queue;
	uint32_t rxq_ctrl0 = 0;
	uint32_t rxq_ctrl1 = 0;
	uint32_t rxq_ctrl2[2] = {0};
	uint32_t rxq_dma_map[2] = {0};
	for (queue = 0; queue < cfg->mtl_rx_queues; queue++) {
		const struct eth_qos_rx_queue *qcfg = &cfg->mtl_rx[queue];
		rxq_ctrl0 |= ((qcfg->av_queue ? 0x1 : 0x2) << (2 * qcfg->nr));
		rxq_ctrl2[qcfg->nr / 4] |= (qcfg->priority << (qcfg->nr % 4) * 8);
		if (qcfg->route_multi_broad) {
			rxq_ctrl1 |=
				MAC_RXQ_CTRL1_MCBCQEN | FIELD_PREP(MAC_RXQ_CTRL1_MCBCQ, qcfg->nr);
		}
		rxq_dma_map[qcfg->nr / 4] |=
			(qcfg->dma_channel << (qcfg->nr % 4) * 8) |
			(qcfg->dynamic_dma_channel << ((qcfg->nr % 4) * 8 + 7));
	}
	sys_write32(rxq_dma_map[0], cfg->base + MTL_RXQ_DMA_MAP0);
	sys_write32(rxq_dma_map[1], cfg->base + MTL_RXQ_DMA_MAP1);
	sys_write32(rxq_ctrl0, cfg->base + MAC_RXQ_CTRL0);
	sys_write32(rxq_ctrl1, cfg->base + MAC_RXQ_CTRL1);
	sys_write32(rxq_ctrl2[0], cfg->base + MAC_RXQ_CTRL2);
	sys_write32(rxq_ctrl2[1], cfg->base + MAC_RXQ_CTRL3);
}

static inline void eth_qos_mtl_txq_init(const struct device *dev, uint8_t queue)
{
	const struct eth_qos_config *cfg = dev->config;
	const struct eth_qos_tx_queue *qcfg = &cfg->mtl_tx[queue];

	sys_write32(FIELD_PREP(MTL_TXQi_OPERATION_MODE_TQS, (qcfg->size + 255) / 256 - 1) |
			    (qcfg->sf ? MTL_TXQi_OPERATION_MODE_TSF
				      : FIELD_PREP(MTL_TXQi_OPERATION_MODE_TTC, qcfg->threshold)) |
			    MTL_TXQi_OPERATION_MODE_TXQEN,
		    cfg->base + MTL_TXQi_OPERATION_MODE(queue));
	/* TODO: CFG field */
	sys_write32(1, cfg->base + MTL_TXQi_QUANTUM_WEIGHT(queue));
}

static inline void eth_qos_set_mac_addr(const struct device *dev, uint8_t nr, uint8_t *mac_addr)
{
	const struct eth_qos_config *cfg = dev->config;
	uint32_t reg_val;

	reg_val = (mac_addr[5] << 8) | mac_addr[4];
	sys_write32(reg_val | MAC_ADDRESSi_HIGH_AE, cfg->base + MAC_ADDRESSi_HIGH(nr));
	reg_val = (mac_addr[3] << 24) | (mac_addr[2] << 16) | (mac_addr[1] << 8) | mac_addr[0];
	sys_write32(reg_val, cfg->base + MAC_ADDRESSi_LOW(nr));
}

/*
 * IRQs
 */
#define ETH_QOS_IRQ_INIT(node_id, prop, idx)                                                       \
	_ETH_QOS_IRQ_INIT(node_id, DT_STRING_TOKEN_BY_IDX(node_id, prop, idx))
#define _ETH_QOS_IRQ_INIT(node, name) ETH_QOS_IRQ_INIT_BY_NAME(node, name)
#define ETH_QOS_IRQ_INIT_BY_NAME(node_id, name)                                                    \
	extern void eth_qos_##name##_isr(const struct device *dev);                                \
	IRQ_CONNECT(DT_IRQ_BY_NAME(node_id, name, irq), DT_IRQ_BY_NAME(node_id, name, priority),   \
		    eth_qos_##name##_isr, DEVICE_DT_GET(node_id), 0)                               \
	irq_enable(DT_IRQ_BY_NAME(node_id, name, irq));

#define MAC_PREFIX_FROM_HEX(hex) ((hex >> 16) & 0xFF), ((hex >> 8) & 0xFF), ((hex) & 0xFF)

#define ETH_QOS_INIT_FUNC(n)                                                                         \
	static void eth_qos##n##_init()                                                              \
	{                                                                                            \
		const struct device *dev = DEVICE_DT_INST_GET(n);                                    \
		const __unused struct eth_qos_config *cfg = dev->config;                             \
		__unused struct eth_qos_data *data = dev->data;                                      \
                                                                                                     \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(n, interrupt_names), (DT_INST_FOREACH_PROP_ELEM(n, interrupt_names, ETH_QOS_IRQ_INIT)))                                                                           \
		COND_CODE_1(                                                                       \
		DT_INST_NODE_HAS_PROP(n, local_mac_address),                               \
		(const uint8_t dwmac_mac_addr[6] = DT_INST_PROP(n, local_mac_address);     \
			memcpy(data->mac_addr, dwmac_mac_addr, 6);),                                 \
		(COND_CODE_1(DT_INST_PROP(n, zephyr_random_mac_address),                   \
				(gen_random_mac(data->mac_addr,                                  \
						MAC_PREFIX_FROM_HEX(                          \
							CONFIG_ETH_QOS_RANDOM_MAC_PREFIX));),   \
				()))) \
	}

/*
 * Basic values
 */
#define ETH_QOS_TXQ_CONFIG(n) DT_INST_CHILD(n, txq_config)
#define ETH_QOS_TX_QUEUES_TO_USE(n)                                                                \
	COND_CODE_1(DT_NODE_EXISTS(ETH_QOS_TXQ_CONFIG(n)), (DT_CHILD_NUM(ETH_QOS_TXQ_CONFIG(n))), (1))
#define ETH_QOS_RXQ_CONFIG(n) DT_INST_CHILD(n, rxq_config)
#define ETH_QOS_RX_QUEUES_TO_USE(n)                                                                \
	COND_CODE_1(DT_NODE_EXISTS(ETH_QOS_RXQ_CONFIG(n)), (DT_CHILD_NUM(ETH_QOS_RXQ_CONFIG(n))), (1))
#define ETH_QOS_TX_CHANNELS_TO_USE(n) ETH_QOS_TX_QUEUES_TO_USE(n)
#define ETH_QOS_RX_CHANNELS_TO_USE(n) DT_INST_PROP_OR(n, snps_rx_channels_to_use, 1)
#define ETH_QOS_RX_DESCRIPTORS(n, ch)                                                              \
	COND_CODE_1(DT_INST_HAS_PROP(n, snps_rx_descriptors),                                      \
	(DT_INST_PROP_BY_IDX(n, snps_rx_descriptors, ch)),                                        \
	(CONFIG_ETH_QOS_NUM_RX_DMA_DESCRIPTORS))
#define ETH_QOS_TX_DESCRIPTORS(n, ch)                                                              \
	COND_CODE_1(DT_INST_HAS_PROP(n, snps_tx_descriptors),                                      \
	(DT_INST_PROP_BY_IDX(n, snps_tx_descriptors, ch)),                                        \
	(CONFIG_ETH_QOS_NUM_TX_DMA_DESCRIPTORS))
#define ETH_QOS_RX_CHANNEL_NR(n, ch)                                                               \
	COND_CODE_1(                                                                               \
		DT_INST_NODE_HAS_PROP(n, snps_channel_nr),                                         \
		(DT_INST_PROP_BY_IDX(n, snps_channel_nr, ch)),                                    \
		(ch)                                                                              \
	)
#define __ETH_QOS_GET_TX_QUEUE_NR(n, ch)                                                           \
	IF_ENABLED(IS_EQ(DT_PROP_OR(n, reg, DT_NODE_CHILD_IDX(n)), ch),                             \
		  (DT_PROP_OR(n, reg, DT_NODE_CHILD_IDX(n))))
#define ETH_QOS_TX_CHANNEL_NR_BY_QUEUE(n, ch)                                                      \
	COND_CODE_1(                                                                               \
		DT_NODE_EXISTS(ETH_QOS_TXQ_CONFIG(n)),                                             \
		(DT_FOREACH_CHILD_SEP_VARGS(ETH_QOS_TXQ_CONFIG(n), __ETH_QOS_GET_TX_QUEUE_NR, (,)), ch),                                                                            \
		(ch)                                                                               \
	)
#define ETH_QOS_TX_CHANNEL_NR(n, ch)                                                               \
	COND_CODE_1(                                                                               \
		CONFIG_ETH_QOS_SHARED_DMA,                                                         \
		(ETH_QOS_RX_CHANNEL_NR(n, ch)),                                                    \
		(ETH_QOS_TX_CHANNEL_NR_BY_QUEUE(n, ch))                                            \
	)

/*
 * DMA
 */
#define __ETH_QOS_DMA_RX_DESCS(idx, n)                                                             \
	union eth_qos_rdes eth_qos##n##dma_rx##idx##_descs[ETH_QOS_RX_DESCRIPTORS(n, idx)]         \
		__aligned(16)
#define __ETH_QOS_DMA_TX_DESCS(idx, n)                                                             \
	union eth_qos_tdes eth_qos##n##dma_tx##idx##_descs[ETH_QOS_TX_DESCRIPTORS(n, idx)]         \
		__aligned(16)
#define ETH_QOS_DMA_DESCS(n)                                                                       \
	LISTIFY(ETH_QOS_RX_CHANNELS_TO_USE(n),__ETH_QOS_DMA_RX_DESCS, (;), n);                       \
	LISTIFY(ETH_QOS_TX_CHANNELS_TO_USE(n),__ETH_QOS_DMA_TX_DESCS, (;), n);

#define __ETH_QOS_DMA_RX_CONFIG(idx, n)                                                            \
	{                                                                                          \
		.descs = (void *)eth_qos##n##dma_rx##idx##_descs,                                  \
		.descs_count = ETH_QOS_RX_DESCRIPTORS(n, idx),                                     \
		.nr = ETH_QOS_RX_CHANNEL_NR(n, idx),                                               \
	}
#define __ETH_QOS_DMA_TX_CONFIG(idx, n)                                                            \
	{                                                                                          \
		.descs = (void *)eth_qos##n##dma_tx##idx##_descs,                                  \
		.descs_count = ETH_QOS_TX_DESCRIPTORS(n, idx),                                     \
		.nr = ETH_QOS_TX_CHANNEL_NR(n, idx),                                               \
	}

#define ETH_QOS_DMA_INIT(n)                                                                        \
	ETH_QOS_DMA_DESCS(n);                                                                      \
	static struct eth_qos_dma_rx_ch_data                                                       \
		eth_qos##n##_dma_rx_data[ETH_QOS_RX_CHANNELS_TO_USE(n)];                           \
	static struct eth_qos_dma_tx_ch_data                                                       \
		eth_qos##n##_dma_tx_data[ETH_QOS_TX_CHANNELS_TO_USE(n)];                           \
	static struct eth_qos_dma_ch_config eth_qos##n##_dma_rx_config[ETH_QOS_RX_CHANNELS_TO_USE( \
		n)] = {LISTIFY(1,__ETH_QOS_DMA_RX_CONFIG, (,), n) };                                 \
	static struct eth_qos_dma_ch_config eth_qos##n##_dma_tx_config[ETH_QOS_TX_CHANNELS_TO_USE( \
		n)] = {LISTIFY(1,__ETH_QOS_DMA_TX_CONFIG, (,), n) }

/*
 * Priority Queues
 */

#define ETH_QOS_RX_QUEUE_CONFIG(n)                                                                 \
	{                                                                                          \
		.nr = DT_PROP_OR(n, reg, DT_NODE_CHILD_IDX(n)),                                    \
		.size = ROUND_UP(DT_PROP_OR(n, snps_size, NET_ETH_MAX_FRAME_SIZE), 256),           \
		.priority = DT_PROP_OR(n, snps_priority, 0),                                       \
		.sf = !DT_PROP(n, snps_cut_through),                                               \
		.dma_channel = DT_PROP_OR(n, snps_dma_channel, 0),                                 \
		.dynamic_dma_channel = !DT_NODE_HAS_PROP(n, snps_dma_channel),                     \
		.av_queue = DT_PROP_OR(n, snps_avb_algorithm, 0),                                  \
		.route_multi_broad = DT_PROP(n, snps_route_multi_broad),                           \
	}
#define ETH_QOS_RX_QUEUE_CONFIG_DEFAULT                                                            \
	{                                                                                          \
		.nr = 0,                                                                           \
		.size = ROUND_UP(NET_ETH_MAX_FRAME_SIZE, 256),                                     \
		.priority = 0xFF,                                                                  \
		.sf = true,                                                                        \
	}
#define ETH_QOS_RX_QUEUE_CONFIGS(n)                                                                \
	{                                                                                          \
		COND_CODE_1(DT_NODE_EXISTS(DT_INST_CHILD(n, rxq_config)),                          \
			    (DT_FOREACH_CHILD_SEP(DT_INST_CHILD(n, rxq_config),                    \
						  ETH_QOS_RX_QUEUE_CONFIG, (, ))),                   \
			    (ETH_QOS_RX_QUEUE_CONFIG_DEFAULT)),                  \
			}
#define ETH_QOS_RX_QUEUE_CONFIG_CHECK(q, n)                                                        \
	BUILD_ASSERT(DT_PROP(q, snps_cut_through) ||                                               \
			     DT_PROP_OR(q, snps_size, 256) >= NET_ETH_MAX_FRAME_SIZE,              \
		     "Queue to small for store and forward");                                      \
	COND_CODE_1(DT_NODE_HAS_PROP(q, snps_map_to_dma_channel),                                  \
		    (BUILD_ASSERT(DT_PROP(q, snps_map_to_dma_channel) <                            \
					  DT_INST_PROP(n, snps_rx_channel_to_use),                 \
				  "Channel not enabled")),                                         \
		    ());

#define ETH_QOS_TX_QUEUE_CONFIG(n)                                                                 \
	{                                                                                          \
		.nr = DT_PROP_OR(n, reg, DT_NODE_CHILD_IDX(n)),                                    \
		.size = ROUND_UP(NET_ETH_MAX_FRAME_SIZE, 256),                                     \
		.priority = DT_PROP_OR(n, snps_priority, 0),                                       \
		.sf = !DT_PROP(n, snps_cut_through),                                               \
		.threshold = DT_ENUM_IDX_OR(n, snps_threshold, 0),                                 \
	}
#define ETH_QOS_TX_QUEUE_CONFIG_DEFAULT                                                            \
	{.nr = 0, .size = ROUND_UP(NET_ETH_MAX_FRAME_SIZE, 256), .sf = true}
#define ETH_QOS_TX_QUEUE_CONFIGS(n)                                                                \
	{                                                                                          \
		COND_CODE_1(               \
		DT_NODE_EXISTS(DT_INST_CHILD(n, txq_config)),                                      \
		(DT_FOREACH_CHILD_SEP(DT_INST_CHILD(n, txq_config), ETH_QOS_TX_QUEUE_CONFIG, (, ))), \
		(ETH_QOS_TX_QUEUE_CONFIG_DEFAULT)),                  \
			};
#define ETH_QOS_TX_QUEUE_CONFIG_CHECK(q, n)                                                        \
	BUILD_ASSERT(DT_PROP(q, snps_cut_through) ||                                               \
			     DT_PROP_OR(q, snps_size, 256) >= NET_ETH_MAX_FRAME_SIZE,              \
		     "Queue to small for store and forward");

#define ETH_QOS_MTL_CONFIG(n)                                                                      \
	struct eth_qos_rx_queue eth_qos##n##_mtl_rx_config[ETH_QOS_RX_QUEUES_TO_USE(n)] =          \
		ETH_QOS_RX_QUEUE_CONFIGS(n);                                                       \
	struct eth_qos_tx_queue eth_qos##n##_mtl_tx_config[ETH_QOS_TX_QUEUES_TO_USE(n)] =          \
		ETH_QOS_TX_QUEUE_CONFIGS(n)

#define ETH_QOS_MTL_INIT(n) ETH_QOS_MTL_CONFIG(n);

#define ETH_QOS_CONFIG(n)                                                                          \
	{.init = eth_qos##n##_init,                                                                \
	 .base = DT_INST_REG_ADDR(n),                                                              \
	 .phy = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(n, phy_handle)),                             \
	 .dma_rx_channel = ETH_QOS_RX_CHANNELS_TO_USE(n),                                          \
	 .dma_tx_channel = ETH_QOS_TX_CHANNELS_TO_USE(n),                                          \
	 .dma_rx = eth_qos##n##_dma_rx_config,                                                     \
	 .dma_tx = eth_qos##n##_dma_tx_config,                                                     \
	 .mtl_rx_queues = ETH_QOS_RX_QUEUES_TO_USE(n),                                             \
	 .mtl_tx_queues = ETH_QOS_TX_QUEUES_TO_USE(n),                                             \
	 .mtl_rx = eth_qos##n##_mtl_rx_config,                                                     \
	 .mtl_tx = eth_qos##n##_mtl_tx_config,                                                     \
	 .do_reset = DT_INST_PROP(n, snps_do_reset),                                               \
	 .skip_init = DT_INST_PROP(n, snps_skip_init),                                             \
	 .dma_only = DT_INST_PROP(n, snps_dma_only),                                               \
	 .da_duplication = DT_INST_PROP(n, snps_da_duplication),                                   \
	 .loopback = DT_INST_PROP(n, snps_loopback),                                               \
	 IF_ENABLED(CONFIG_ETH_QOS_SHARED_DMA, (.dma_base = DT_REG_ADDR(DT_INST_PHANDLE(n, snps_dma)) - DMA_MODE,))                                                                                \
			     IF_ENABLED(CONFIG_PTP_CLOCK, (.ptp_clock = DEVICE_DT_GET(DT_INST_PHANDLE(n, ptp_clock)),))}

#define ETH_QOS_DMA_DATA(n)                                                                        \
	struct eth_qos_dma_rx_ch_data eth_qos##n##_dma_rx_data[ETH_QOS_RX_CHANNELS_TO_USE(n)];     \
	struct eth_qos_dma_tx_ch_data eth_qos##n##_dma_tx_data[ETH_QOS_TX_CHANNELS_TO_USE(n)];
#define ETH_QOS_DATA(n)                                                                            \
	{.dma_rx = eth_qos##n##_dma_rx_data,                                                       \
	 .dma_tx = eth_qos##n##_dma_tx_data,                                                       \
	 IF_ENABLED(CONFIG_NET_PKT_TIMESTAMP, (.ptp_pkts = eth_qos_ptp_pkts_##n,))}

#endif
