/*
 * Copyright (c) 2026 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_LIN_LIN_ASCLIN_PRIV_H_
#define ZEPHYR_DRIVERS_LIN_LIN_ASCLIN_PRIV_H_

#include <zephyr/device.h>
#include <zephyr/drivers/lin.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "IfxAsclin_regdef.h"

/* TC4x exposes RXDATA/TXDATA as arrays; TC3x as scalars. Byte-wide
 * accesses use index 0 on TC4x.
 */
#if defined(CONFIG_SOC_SERIES_TC4X)
#define LIN_ASCLIN_RXDATA_READ(base)		((base)->RXDATA[0].U)
#define LIN_ASCLIN_TXDATA_WRITE(base, val)	((base)->TXDATA[0].U = (val))
#else
#define LIN_ASCLIN_RXDATA_READ(base)		((base)->RXDATA.U)
#define LIN_ASCLIN_TXDATA_WRITE(base, val)	((base)->TXDATA.U = (val))
#endif

enum lin_asclin_clock_type {
	LIN_ASCLIN_CLOCK_OFF = 0,
	LIN_ASCLIN_CLOCK_FASCLINS = 4,
	LIN_ASCLIN_CLOCK_FASCLINF = 2,
};

/* RX_HEADER is the responder-side intermediate state between RH and
 * the application calling lin_response() / lin_read().
 */
enum lin_asclin_state {
	LIN_ASCLIN_STATE_IDLE = 0,
	LIN_ASCLIN_STATE_TX_ONGOING = BIT(0),
	LIN_ASCLIN_STATE_RX_ONGOING = BIT(1),
	LIN_ASCLIN_STATE_RX_HEADER = BIT(2),
};

struct lin_asclin_config {
	struct lin_driver_config common; /* must be first */
	Ifx_ASCLIN *base;
	const struct pinctrl_dev_config *pcfg;
	const struct device *clkctrl;
	uint32_t clk; /* clock subsystem id */
	enum lin_asclin_clock_type clk_src;
	uint16_t prescaler;
	uint8_t oversampling;
	uint8_t samplepoint;
	bool median_filter;
	uint32_t brg_numerator;
	uint32_t brg_denominator;
	uint32_t bus_idle_timeout_ms; /* 0 disables wake-up plumbing */
	void (*irq_config_func)(const struct device *dev);
};

struct lin_asclin_data {
	struct lin_driver_data common; /* must be first */
	const struct device *dev;
	struct k_sem submit_sem;
	struct k_work_delayable timeout_work;
	atomic_t state;

	/* Active transaction. user_data points at the caller's msg->data so
	 * the ISR populates it directly when RR fires. The caller must keep
	 * msg alive until the callback fires (same contract as Renesas RA).
	 */
	struct {
		uint8_t pid;
		uint8_t len;
		uint8_t *user_data;
	} cur;

	bool filter_active;
	struct lin_filter filter;

	/* bus_state: 0 = active, 1 = idle / sleep-eligible.
	 * FED is armed only when bus_state == 1 so normal traffic edges are
	 * not mistaken for wake-ups.
	 */
	atomic_t bus_state;
	struct k_work_delayable bus_idle_work;
};

#endif /* ZEPHYR_DRIVERS_LIN_LIN_ASCLIN_PRIV_H_ */
