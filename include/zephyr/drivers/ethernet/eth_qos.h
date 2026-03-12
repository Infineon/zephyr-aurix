/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Common API functions for Synposis MAC (Ethernet QoS & XGMAC) devices.
 *
 */

#ifndef ZEPHYR_DRIVERS_ETHERNET_MAC_H
#define ZEPHYR_DRIVERS_ETHERNET_MAC_H

#include <zephyr/device.h>

/** DMAC VLAN filter element */
struct eth_qos_vlan_filter {
	/** VLAN TAG incl. VID, PCF & PCP */
	uint16_t tag;
	/** Target DMA channel */
	uint8_t dma_ch;
	/** Enable filter element */
	bool enable;
	/** Check only VID */
	bool check_vid;
	/** Set DMA channel on match */
	bool set_dma_ch;
};

/** MAC MAC address filter element */
struct eth_qos_mac_filter {
	/** MAC address */
	uint8_t addr[6];
	/** Target DMA channel or one-hot DMA Channel in case of da duplication */
	uint16_t dma_ch;
	/** Byte mask control to mask out bytes during compare */
	uint8_t mask_byte_control;
	/** Enable filter */
	bool enable;
	/** Source address filter */
	bool source;
	/** Set DMA channel on match */
	bool set_dma_ch;
};

/** Add a MAC address filter element to the MAC
 *
 * The function will fill the first free slot with the filter.
 *
 * @param[in] dev mac device
 * @param[out] index Position of the filter element
 * @param[in] filter MAC address filter element
 *
 * @retval -EIO All filter elements are used
 * @retval 0 Successful added filter element
 */
int eth_qos_mac_add_filter(const struct device *dev, uint8_t *index,
			   const struct eth_qos_mac_filter *filter);

/** Set a MAC address filter element in the MAC
 *
 * The filter element inside the DMAC will be overwritten with the
 * new value.
 *
 * @param[in] dev mac device
 * @param[in] index Position of the filter element
 * @param[in] filter MAC address filter element
 *
 * @retval -EINVAL If filter index is out of range
 * @retval 0 Successful set filter element
 */
int eth_qos_mac_set_filter(const struct device *dev, uint8_t index,
			   const struct eth_qos_mac_filter *filter);

/** Get a MAC address filter element in the MAC
 *
 * @param[in] dev mac device
 * @param[in] index Position of the filter element
 * @param[in] filter MAC address filter element
 *
 * @retval -EINVAL If filter index is out of range
 * @retval 0 Successful set filter element
 */
int eth_qos_mac_get_filter(const struct device *dev, uint8_t index,
			   struct eth_qos_mac_filter *filter);

/** Add a VLAN element to the MAC
 *
 * The function will fill the first free slot with the filter.
 *
 * @param[in] dev mac device
 * @param[out] index Position of the filter element
 * @param[in] filter VLAN filter element
 *
 * @retval -EIO All filter elements are used
 * @retval 0 Successful added filter element
 */
int eth_qos_vlan_add_filter(const struct device *dev, uint8_t *index,
			    const struct eth_qos_vlan_filter *filter);

/** Set a VLANA filter element in the MAC
 *
 * The filter element inside the DMAC will be overwritten with the
 * new value.
 *
 * @param[in] dev mac device
 * @param[in] index Position of the filter element
 * @param[in] filter VLAN filter element
 *
 * @retval -EINVAL If filter index is out of range
 * @retval 0 Successful set filter element
 */
int eth_qos_vlan_set_filter(const struct device *dev, uint8_t index,
			    const struct eth_qos_vlan_filter *filter);

/** Get a VLAN filter element in the MAC
 *
 * @param[in] dev mac device
 * @param[in] index Position of the filter element
 * @param[in] filter VLAN filter element
 *
 * @retval -EINVAL If filter index is out of range
 * @retval 0 Successful set filter element
 */
int eth_qos_vlan_get_filter(const struct device *dev, uint8_t index,
			    struct eth_qos_vlan_filter *filter);

#if CONFIG_MAC_TARGET_TIME_CALLBACK
#include <zephyr/net/ptp_time.h>
void eth_qos_pps_set_callback(const struct device *dev, void (*callback)(), void *callback_data);
int eth_qos_pps_set_irq_target_time(const struct device *dev, struct net_ptp_time *ts);
void eth_qos_pps_enable_flexible(const struct device *dev);
int eth_qos_pps_start_single_pulse(const struct device *dev, struct net_ptp_time *ts,
				   uint32_t width);
int eth_qos_pps_single_pulse_irq(const struct device *dev, struct net_ptp_time *ts, uint32_t width);
#endif
#endif
