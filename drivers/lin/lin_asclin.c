/*
 * Copyright (c) 2026 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_asclin_lin

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/lin.h>
#include <zephyr/drivers/lin/transceiver.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <soc.h>
#include "IfxAsclin_regdef.h"
#include "lin_asclin_priv.h"

static inline void lin_asclin_clear_all_flags(Ifx_ASCLIN *base)
{
	base->FLAGSCLEAR.U = 0xFFFFFFFFU;
}

static inline void lin_asclin_disable_all_flags(Ifx_ASCLIN *base)
{
	base->FLAGSENABLE.U = 0;
}

static inline void lin_asclin_flush_fifos(Ifx_ASCLIN *base)
{
	base->TXFIFOCON.B.FLUSH = 1;
	base->RXFIFOCON.B.FLUSH = 1;
}

/* Flush both FIFOs, then set TX outlet / RX inlet enables. Flush must precede
 * any DATLEN change (TC4Dx ASCLIN 25.3.7).
 */
static inline void lin_asclin_prepare_fifos(Ifx_ASCLIN *base, bool tx_enable, bool rx_enable)
{
	base->TXFIFOCON.B.FLUSH = 1;
	base->TXFIFOCON.B.ENO = tx_enable ? 1 : 0;
	base->RXFIFOCON.B.FLUSH = 1;
	base->RXFIFOCON.B.ENI = rx_enable ? 1 : 0;
}

/* One FLAGSSET store: separate RMWs would let the LIN engine advance between
 * request bits. Write-1-to-set, so a zero base is correct (iLLD sendFrame).
 */
static inline void lin_asclin_flagsset(Ifx_ASCLIN *base, Ifx_ASCLIN_FLAGSSET req)
{
	base->FLAGSSET.U = req.U;
}

static inline int lin_asclin_set_clk(Ifx_ASCLIN *base, enum lin_asclin_clock_type clk)
{
	bool want_on = (clk != LIN_ASCLIN_CLOCK_OFF);

	base->CSR.U = (uint32_t)clk;
	return WAIT_FOR(base->CSR.B.CON == want_on, 1000, k_busy_wait(1)) ? 0 : -ETIMEDOUT;
}

/* BRG NUMERATOR/DENOMINATOR for fOVS = fpd * NUM/DEN; fpd = fA / (PRESCALER+1). */
static void lin_asclin_set_baudrate(Ifx_ASCLIN *base, uint32_t baudrate, uint32_t fpd,
				    uint8_t oversampling)
{
	const uint32_t fovs = baudrate * oversampling;
	float div = (float)fovs / (float)fpd;
	int32_t m[2][2] = { { 1, 0 }, { 0, 1 } };
	int32_t ai;

	while (m[1][0] * (ai = (int32_t)div) + m[1][1] <= 4095) {
		int32_t t;

		t = m[0][0] * ai + m[0][1];
		m[0][1] = m[0][0];
		m[0][0] = t;
		t = m[1][0] * ai + m[1][1];
		m[1][1] = m[1][0];
		m[1][0] = t;
		div = 1.0f / (div - (float)ai);
	}

	base->BRG.B = (Ifx_ASCLIN_BRG_Bits) { .NUMERATOR = m[0][0], .DENOMINATOR = m[1][0] };
}

/* DATCON store: avoid intermediate DATLEN/CSM visibility. */
static inline void lin_asclin_program_datcon(Ifx_ASCLIN *base, const struct lin_msg *msg)
{
	Ifx_ASCLIN_DATCON dc = { .U = base->DATCON.U };

	dc.B.HO = 0;
	dc.B.CSM = (msg->checksum_type == LIN_CHECKSUM_ENHANCED) ? 1 : 0;
	dc.B.DATLEN = (msg->data_len > 0) ? (msg->data_len - 1) : 0;
	base->DATCON.U = dc.U;
}

/* Pass if frame id matches primary or secondary under mask (parity ignored). */
static inline bool lin_asclin_filter_pass(const struct lin_filter *f, uint8_t pid)
{
	uint8_t id = lin_get_frame_id(pid);
	uint8_t m = f->mask & LIN_ID_MASK;

	return (((id ^ f->primary_pid) & m) == 0) ||
	       (((id ^ f->secondary_pid) & m) == 0);
}

static int lin_asclin_hw_configure(const struct device *dev, const struct lin_config *cfg)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	uint32_t fa;
	uint32_t fpd;
	int ret;

	ret = clock_control_get_rate(config->clkctrl,
				     (clock_control_subsys_t)&config->clk, &fa);
	if (ret < 0) {
		return ret;
	}
	if (fa == 0 || config->prescaler == 0) {
		return -EINVAL;
	}
	fpd = fa / config->prescaler;

	/* Stop the module before reconfiguring. */
	ret = lin_asclin_set_clk(base, LIN_ASCLIN_CLOCK_OFF);
	if (ret < 0) {
		return ret;
	}
	base->FRAMECON.U = 0;

	/* Bit timing: prescaler / oversampling / sample point / median filter. */
	base->BITCON.B = (Ifx_ASCLIN_BITCON_Bits) {
		.PRESCALER = (uint16_t)(config->prescaler - 1),
		.OVERSAMPLING = (uint8_t)(config->oversampling - 1),
		.SAMPLEPOINT = config->samplepoint,
		.SM = config->median_filter ? 1 : 0,
	};

	/* Baud-rate fractional divider. */
	if (config->brg_numerator && config->brg_denominator) {
		base->BRG.B = (Ifx_ASCLIN_BRG_Bits) {
			.NUMERATOR = config->brg_numerator,
			.DENOMINATOR = config->brg_denominator,
		};
	} else {
		lin_asclin_set_baudrate(base, cfg->baudrate, fpd, config->oversampling);
	}

	/* LIN mode, LSB first, no parity, optional collision detect.
	 * IDLE=2/LEAD=1 per iLLD (0 starves the header-only response pump).
	 */
	base->FRAMECON.B = (Ifx_ASCLIN_FRAMECON_Bits) {
		.MODE = 3, /* LIN */
		.MSB = 0,
		.PEN = 0,
		.CEN = (cfg->flags & LIN_BUS_CONFLICT_DETECTION) ? 1 : 0,
		.STOP = 1,
		.IDLE = 2,
		.LEAD = 1,
	};

	/* Defaults; DATLEN/CSM reprogrammed per transaction. */
	base->DATCON.B = (Ifx_ASCLIN_DATCON_Bits) {
		.DATLEN = 0,
		.HO = 0,
		.RM = 0,	/* frame timeout */
		.CSM = 0,	/* classic by default */
		.RESPONSE = 0xFF,
	};

	/* commander/responder, HW checksum on, no injection, optional autobaud. */
	base->LIN.CON.B = (Ifx_ASCLIN_LIN_CON_Bits) {
		.MS = (cfg->mode == LIN_MODE_COMMANDER) ? 1 : 0,
		.CSEN = 1,
		.CSI = 0,
		.ABD = (cfg->flags & LIN_BUS_AUTO_SYNC) ? 1 : 0,
	};

	/* Break length: TRM LINBTIMER.BREAK is 6-bit; clamp. */
	base->LIN.BTIMER.B = (Ifx_ASCLIN_LIN_BTIMER_Bits) {
		.BREAK = MIN((uint32_t)cfg->break_len, (uint32_t)0x3F),
	};

	/* Header timeout: max application owns per-frame timeout via k_work. */
	base->LIN.HTIMER.B = (Ifx_ASCLIN_LIN_HTIMER_Bits) { .HEADER = 0xFF };

	/* FIFOs: byte-wide, single-byte writes; flush both. */
	base->TXFIFOCON.B = (Ifx_ASCLIN_TXFIFOCON_Bits) {
		.FLUSH = 1, .ENO = 0, .FM = 0, .INW = 1,
	};
	base->RXFIFOCON.B = (Ifx_ASCLIN_RXFIFOCON_Bits) {
		.FLUSH = 1, .ENI = 0, .FM = 0, .OUTW = 1, .BUF = 0,
	};

	lin_asclin_disable_all_flags(base);
	lin_asclin_clear_all_flags(base);

	/* Restart clock to whichever source was configured. */
	ret = lin_asclin_set_clk(base, config->clk_src);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

/* Baseline IRQ-enable set: responder always listens for RH and errors. */
static void lin_asclin_arm_idle(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	struct lin_asclin_data *data = dev->data;
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };

	if (data->common.config.mode == LIN_MODE_RESPONDER) {
		en.B.RHE = 1;

		/* HO=1 prevents phantom response from stale DATLEN after prior RX. */
		base->DATCON.B.HO = 1;

		/* Flush TX echoes from RX FIFO so next PID isn't queued behind stale data. */
		base->RXFIFOCON.B.FLUSH = 1;
	}

	/* BDE off: every frame starts with a break, so it would fire every frame. */
	en.B.HTE = 1;
	en.B.RTE = 1;
	en.B.LCE = 1;
	en.B.LPE = 1;
	en.B.LAE = 1;
	en.B.CEE = 1;
	en.B.FEE = 1;
	en.B.RFOE = 1;
	en.B.TFOE = 1;

	lin_asclin_clear_all_flags(base);
	base->FLAGSENABLE.U = en.U;
}

static void lin_asclin_commander_send_header_only(const struct device *dev, uint8_t pid)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };

	/* RX inlet stays enabled during TX: the LIN engine uses the loopback path. */
	base->DATCON.B.HO = 1;
	lin_asclin_prepare_fifos(base, true, true);

	lin_asclin_clear_all_flags(base);
	LIN_ASCLIN_TXDATA_WRITE(base, pid);

	en.B.THE = 1;
	en.B.HTE = 1;
	en.B.LPE = 1;
	en.B.CEE = 1;
	en.B.FEE = 1;
	base->FLAGSENABLE.U = en.U;

	lin_asclin_flagsset(base, (Ifx_ASCLIN_FLAGSSET) { .B.THRQS = 1 });
}

static void lin_asclin_commander_send_full(const struct device *dev,
					   const struct lin_msg *msg, uint8_t pid)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };
	size_t i;

	/* RX loopback as in send_header_only; flush before DATLEN change. */
	lin_asclin_prepare_fifos(base, true, true);
	lin_asclin_program_datcon(base, msg);

	lin_asclin_clear_all_flags(base);

	LIN_ASCLIN_TXDATA_WRITE(base, pid);
	for (i = 0; i < msg->data_len; i++) {
		LIN_ASCLIN_TXDATA_WRITE(base, msg->data[i]);
	}

	en.B.THE = 1;
	en.B.TRE = 1;
	en.B.HTE = 1;
	en.B.RTE = 1;
	en.B.LPE = 1;
	en.B.CEE = 1;
	en.B.FEE = 1;
	en.B.BDE = 1;
	en.B.TFOE = 1;
	base->FLAGSENABLE.U = en.U;

	lin_asclin_flagsset(base, (Ifx_ASCLIN_FLAGSSET) { .B.THRQS = 1, .B.TRRQS = 1 });
}

static void lin_asclin_commander_request_response(const struct device *dev,
						  const struct lin_msg *msg, uint8_t pid)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };

	/* RX loopback as in send_header_only; flush before DATLEN change. */
	lin_asclin_prepare_fifos(base, true, true);
	lin_asclin_program_datcon(base, msg);

	lin_asclin_clear_all_flags(base);
	LIN_ASCLIN_TXDATA_WRITE(base, pid);

	en.B.THE = 1;
	en.B.RRE = 1;
	en.B.HTE = 1;
	en.B.RTE = 1;
	en.B.LCE = 1;
	en.B.LPE = 1;
	en.B.CEE = 1;
	en.B.FEE = 1;
	en.B.RFOE = 1;
	en.B.BDE = 1;
	base->FLAGSENABLE.U = en.U;

	lin_asclin_flagsset(base, (Ifx_ASCLIN_FLAGSSET) { .B.THRQS = 1 });
}

static void lin_asclin_responder_send_response(const struct device *dev, const struct lin_msg *msg)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };
	size_t i;

	/* RX loopback counts TX bytes for TR; flush before DATLEN change. */
	lin_asclin_prepare_fifos(base, true, true);
	lin_asclin_program_datcon(base, msg);

	/* Clear stale flags before TXFIFO write to avoid a premature tx_isr. */
	lin_asclin_clear_all_flags(base);

	for (i = 0; i < msg->data_len; i++) {
		LIN_ASCLIN_TXDATA_WRITE(base, msg->data[i]);
	}

	/* Mirror commander error mask so failed responses surface as LIN_EVT_ERR. */
	en.B.TRE = 1;
	en.B.HTE = 1;
	en.B.RTE = 1;
	en.B.LPE = 1;
	en.B.LCE = 1;
	en.B.CEE = 1;
	en.B.FEE = 1;
	en.B.BDE = 1;
	en.B.TFOE = 1;
	base->FLAGSENABLE.U = en.U;

	lin_asclin_flagsset(base, (Ifx_ASCLIN_FLAGSSET) { .B.TRRQS = 1 });
}

static void lin_asclin_responder_read_response(const struct device *dev, const struct lin_msg *msg)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	/* Clean enable mask (see responder_send_response). */
	Ifx_ASCLIN_FLAGSENABLE en = { .U = 0 };

	/* Flush before DATLEN change; RX inlet on, TX outlet off for a pure read. */
	lin_asclin_prepare_fifos(base, false, true);
	lin_asclin_program_datcon(base, msg);

	en.B.RRE = 1;
	en.B.RFOE = 1;
	en.B.LCE = 1;
	en.B.RTE = 1;
	en.B.CEE = 1;
	base->FLAGSENABLE.U = en.U;
}

/* After bus-idle timeout, arm FED to catch a wake-up falling edge. */
static void lin_asclin_bus_idle_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct lin_asclin_data *data = CONTAINER_OF(dwork, struct lin_asclin_data, bus_idle_work);
	const struct device *dev = data->dev;
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;

	atomic_set(&data->bus_state, 1);

	/* Clear stale FED, then arm it so a falling edge fires LIN_EVT_RX_WAKEUP. */
	base->FLAGSCLEAR.B.FEDC = 1;
	base->FLAGSENABLE.B.FEDE = 1;
}

static inline void lin_asclin_rearm_bus_idle(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	Ifx_ASCLIN *base = config->base;

	if (config->bus_idle_timeout_ms == 0) {
		return;
	}

	/* Saw activity -> back to "awake" and re-arm the timer. */
	if (atomic_cas(&data->bus_state, 1, 0)) {
		/* We were idle; disarm FED now that we're awake again. */
		base->FLAGSENABLE.B.FEDE = 0;
	}
	(void)k_work_reschedule(&data->bus_idle_work,
				K_MSEC(config->bus_idle_timeout_ms));
}

static void lin_asclin_complete(const struct device *dev, struct lin_event *event)
{
	struct lin_asclin_data *data = dev->data;

	atomic_set(&data->state, LIN_ASCLIN_STATE_IDLE);
	(void)k_work_cancel_delayable(&data->timeout_work);
	k_sem_give(&data->submit_sem);

	if (data->common.callback) {
		data->common.callback(dev, event, data->common.callback_data);
	}

	/* Re-arm baseline IRQs for the next frame. */
	lin_asclin_arm_idle(dev);

	/* Activity seen -> bus is awake; re-arm the idle monitor. */
	lin_asclin_rearm_bus_idle(dev);
}

static void lin_asclin_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct lin_asclin_data *data = CONTAINER_OF(dwork, struct lin_asclin_data, timeout_work);
	const struct device *dev = data->dev;
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	atomic_val_t prev = atomic_get(&data->state);
	struct lin_event event;

	if (prev == LIN_ASCLIN_STATE_IDLE) {
		return; /* spurious */
	}

	/* Abort: flush + clear + reset enable mask. */
	lin_asclin_flush_fifos(base);
	lin_asclin_clear_all_flags(base);

	event.type = LIN_EVT_ERR;
	event.status = -EAGAIN;
	event.error_flags = LIN_ERR_COUNTER_OVERFLOW;
	lin_asclin_complete(dev, &event);
}

static void lin_asclin_tx_isr(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGS flags = { .U = base->FLAGS.U };
	struct lin_event event = { 0 };
	bool deliver = false;

	if (flags.B.TR) {
		base->FLAGSCLEAR.B.TRC = 1;
		event.type = LIN_EVT_TX_DATA;
		event.data.pid = data->cur.pid;
		event.status = 0;
		deliver = true;
	} else if (flags.B.TH) {
		base->FLAGSCLEAR.B.THC = 1;
		/* Header-only done; if a response phase is pending, wait for TR/RR. */
		if (!(base->FLAGSENABLE.B.TRE || base->FLAGSENABLE.B.RRE)) {
			event.type = LIN_EVT_TX_HEADER;
			event.header.pid = data->cur.pid;
			event.status = 0;
			deliver = true;
		}
	}

	if (deliver) {
		lin_asclin_complete(dev, &event);
	}
}

static void lin_asclin_rx_isr(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGS flags = { .U = base->FLAGS.U };
	struct lin_event event = { 0 };
	bool deliver = false;

	/* FED: wake-up when bus_state == 1. RH/RR overwrite event before use. */
	if (flags.B.FED) {
		base->FLAGSCLEAR.B.FEDC = 1;
		base->FLAGSENABLE.B.FEDE = 0;
		if (atomic_cas(&data->bus_state, 1, 0)) {
			event.type = LIN_EVT_RX_WAKEUP;
			event.status = 0;
			if (data->common.callback) {
				data->common.callback(dev, &event,
						      data->common.callback_data);
			}
			lin_asclin_rearm_bus_idle(dev);
		}
	}

	/* RH: responder gets the header, commander gets the PID echo. Drain and
	 * fall through to RR (may share this ISR window).
	 */
	if (flags.B.RH) {
		base->FLAGSCLEAR.B.RHC = 1;

		if (base->RXFIFOCON.B.FILL) {
			uint8_t pid = (uint8_t)LIN_ASCLIN_RXDATA_READ(base);

			data->cur.pid = pid;

			if (data->common.config.mode == LIN_MODE_RESPONDER) {
				if (data->filter_active &&
				    !lin_asclin_filter_pass(&data->filter, pid)) {
					/* Not for us: re-arm header-only, wait for next break. */
					base->DATCON.B.HO = 1;
					return;
				}

				atomic_set(&data->state, LIN_ASCLIN_STATE_RX_HEADER);
				event.type = LIN_EVT_RX_HEADER;
				event.header.pid = pid;
				event.status = 0;
				/* Re-arm idle before the callback so a fast app path
				 * sees fresh state.
				 */
				lin_asclin_rearm_bus_idle(dev);
				/* State left armed for the callback's lin_response()/lin_read(). */
				if (data->common.callback) {
					data->common.callback(dev, &event,
							      data->common.callback_data);
				}
				return;
			}
			/* commander: PID echo drained, fall through to RR */
		}
	}

	if (flags.B.RR) {
		size_t i = 0;
		uint8_t expected = data->cur.len;

		base->FLAGSCLEAR.B.RRC = 1;
		if (data->cur.user_data != NULL) {
			while (base->RXFIFOCON.B.FILL && i < expected && i < LIN_MAX_DLEN) {
				data->cur.user_data[i++] =
					(uint8_t)LIN_ASCLIN_RXDATA_READ(base);
			}
		} else {
			while (base->RXFIFOCON.B.FILL && i < expected && i < LIN_MAX_DLEN) {
				(void)LIN_ASCLIN_RXDATA_READ(base);
				i++;
			}
		}

		event.type = LIN_EVT_RX_DATA;
		event.data.pid = data->cur.pid;
		event.data.bytes_received = i;
		event.data.checksum = 0;
		event.status = 0;
		deliver = true;
	}

	if (deliver) {
		lin_asclin_complete(dev, &event);
	}
}

static void lin_asclin_err_isr(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	Ifx_ASCLIN *base = config->base;
	Ifx_ASCLIN_FLAGS flags = { .U = base->FLAGS.U };
	struct lin_event event = { 0 };
	lin_error_flags_t err = 0;

	if (flags.B.LC) {
		err |= LIN_ERR_INVALID_CHECKSUM;
		base->FLAGSCLEAR.B.LCC = 1;
	}
	if (flags.B.LP) {
		err |= LIN_ERR_PARITY;
		base->FLAGSCLEAR.B.LPC = 1;
	}
	if (flags.B.CE) {
		err |= LIN_ERR_BUS_COLLISION;
		base->FLAGSCLEAR.B.CEC = 1;
	}
	if (flags.B.FE) {
		err |= LIN_ERR_FRAMING;
		base->FLAGSCLEAR.B.FEC = 1;
	}
	if (flags.B.RFO) {
		err |= LIN_ERR_OVERRUN;
		base->FLAGSCLEAR.B.RFOC = 1;
	}
	if (flags.B.HT) {
		err |= LIN_ERR_COUNTER_OVERFLOW;
		base->FLAGSCLEAR.B.HTC = 1;
	}
	if (flags.B.RT) {
		err |= LIN_ERR_COUNTER_OVERFLOW;
		base->FLAGSCLEAR.B.RTC = 1;
	}
	if (flags.B.TFO) {
		err |= LIN_ERR_OVERRUN;
		base->FLAGSCLEAR.B.TFOC = 1;
	}
	if (flags.B.BD) {
		base->FLAGSCLEAR.B.BDC = 1;
	}
	if (flags.B.LA) {
		base->FLAGSCLEAR.B.LAC = 1;
	}

	if (err == 0) {
		return;
	}

	lin_asclin_flush_fifos(base);

	event.type = LIN_EVT_ERR;
	event.error_flags = err;
	event.status = -EIO;
	lin_asclin_complete(dev, &event);
}

static int lin_asclin_start(const struct device *dev)
{
	struct lin_asclin_data *data = dev->data;
	const struct device *phy = lin_get_transceiver(dev);
	int ret;

	if (data->common.started) {
		return -EALREADY;
	}

	if (phy != NULL) {
		ret = lin_transceiver_enable(phy, 0);
		if (ret < 0) {
			return ret;
		}
	}

	lin_asclin_arm_idle(dev);
	data->common.started = true;

	/* Start the bus-idle countdown; on timeout the handler arms FED for wake-up. */
	lin_asclin_rearm_bus_idle(dev);

	return 0;
}

static int lin_asclin_stop(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	const struct device *phy = lin_get_transceiver(dev);
	int ret;

	if (!data->common.started) {
		return -EALREADY;
	}

	(void)k_work_cancel_delayable(&data->timeout_work);
	(void)k_work_cancel_delayable(&data->bus_idle_work);
	lin_asclin_disable_all_flags(config->base);
	lin_asclin_flush_fifos(config->base);
	atomic_set(&data->state, LIN_ASCLIN_STATE_IDLE);
	atomic_set(&data->bus_state, 0);
	k_sem_give(&data->submit_sem);

	if (phy != NULL) {
		ret = lin_transceiver_disable(phy);
		if (ret < 0) {
			return ret;
		}
	}

	data->common.started = false;
	return 0;
}

static int lin_asclin_configure(const struct device *dev, const struct lin_config *cfg)
{
	struct lin_asclin_data *data = dev->data;
	int ret;

	if (data->common.started) {
		return -EBUSY;
	}

	ret = lin_asclin_hw_configure(dev, cfg);
	if (ret < 0) {
		return ret;
	}

	memcpy(&data->common.config, cfg, sizeof(*cfg));
	return 0;
}

static int lin_asclin_get_config(const struct device *dev, struct lin_config *cfg)
{
	struct lin_asclin_data *data = dev->data;

	if (cfg == NULL) {
		return -EINVAL;
	}
	memcpy(cfg, &data->common.config, sizeof(*cfg));
	return 0;
}

static int lin_msg_validate(const struct lin_msg *msg)
{
	if (msg == NULL) {
		return -EINVAL;
	}
	if ((msg->id & ~LIN_ID_MASK) != 0) {
		return -EINVAL;
	}
	if (msg->data_len > LIN_MAX_DLEN) {
		return -EINVAL;
	}
	if (msg->checksum_type != LIN_CHECKSUM_CLASSIC &&
	    msg->checksum_type != LIN_CHECKSUM_ENHANCED) {
		return -EINVAL;
	}
	return 0;
}

static void lin_asclin_arm_timeout(struct lin_asclin_data *data, k_timeout_t timeout)
{
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT) && !K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		(void)k_work_reschedule(&data->timeout_work, timeout);
	}
}

static int lin_asclin_send(const struct device *dev, const struct lin_msg *msg,
			   k_timeout_t timeout)
{
	struct lin_asclin_data *data = dev->data;
	int ret;
	uint8_t pid;

	ret = lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (data->common.config.mode != LIN_MODE_COMMANDER) {
		return -EPERM;
	}
	if (!data->common.started) {
		return -EIO;
	}

	if (k_sem_take(&data->submit_sem, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	pid = lin_compute_pid(msg->id);
	data->cur.pid = pid;
	data->cur.len = msg->data_len;

	atomic_set(&data->state, LIN_ASCLIN_STATE_TX_ONGOING);

	if (msg->data_len == 0) {
		lin_asclin_commander_send_header_only(dev, pid);
	} else {
		lin_asclin_commander_send_full(dev, msg, pid);
	}

	lin_asclin_arm_timeout(data, timeout);
	return 0;
}

static int lin_asclin_receive(const struct device *dev, struct lin_msg *msg,
			      k_timeout_t timeout)
{
	struct lin_asclin_data *data = dev->data;
	int ret;
	uint8_t pid;

	ret = lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (data->common.config.mode != LIN_MODE_COMMANDER) {
		return -EPERM;
	}
	if (!data->common.started) {
		return -EIO;
	}

	if (k_sem_take(&data->submit_sem, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	pid = lin_compute_pid(msg->id);
	data->cur.pid = pid;
	data->cur.len = msg->data_len;
	data->cur.user_data = msg->data;

	atomic_set(&data->state, LIN_ASCLIN_STATE_RX_ONGOING);
	lin_asclin_commander_request_response(dev, msg, pid);
	lin_asclin_arm_timeout(data, timeout);
	return 0;
}

static int lin_asclin_response(const struct device *dev, const struct lin_msg *msg,
			       k_timeout_t timeout)
{
	struct lin_asclin_data *data = dev->data;
	int ret;

	ret = lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}

	if (!atomic_cas(&data->state, LIN_ASCLIN_STATE_RX_HEADER, LIN_ASCLIN_STATE_TX_ONGOING)) {
		return -EFAULT;
	}

	if (k_sem_take(&data->submit_sem, K_NO_WAIT) != 0) {
		atomic_set(&data->state, LIN_ASCLIN_STATE_IDLE);
		return -EBUSY;
	}

	data->cur.len = msg->data_len;

	lin_asclin_responder_send_response(dev, msg);
	lin_asclin_arm_timeout(data, timeout);
	return 0;
}

static int lin_asclin_read(const struct device *dev, struct lin_msg *msg, k_timeout_t timeout)
{
	struct lin_asclin_data *data = dev->data;
	int ret;

	ret = lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}

	if (!atomic_cas(&data->state, LIN_ASCLIN_STATE_RX_HEADER, LIN_ASCLIN_STATE_RX_ONGOING)) {
		return -EFAULT;
	}

	if (k_sem_take(&data->submit_sem, K_NO_WAIT) != 0) {
		atomic_set(&data->state, LIN_ASCLIN_STATE_IDLE);
		return -EBUSY;
	}

	data->cur.len = msg->data_len;
	data->cur.user_data = msg->data;
	lin_asclin_responder_read_response(dev, msg);
	lin_asclin_arm_timeout(data, timeout);
	return 0;
}

static int lin_asclin_wakeup_send(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	Ifx_ASCLIN *base = config->base;
	struct lin_event event = { 0 };

	if (!data->common.started) {
		return -EIO;
	}

	/* 0x80 = 7 zero bits LSB-first (~250-450 us at 19200 Bd); set TWRQS.
	 * Disable FEDE first to suppress a self-triggered wake-up.
	 */
	base->FLAGSENABLE.B.FEDE = 0;
	base->FLAGSCLEAR.B.FEDC = 1;

	base->TXFIFOCON.B.FLUSH = 1;
	base->TXFIFOCON.B.ENO = 1;
	LIN_ASCLIN_TXDATA_WRITE(base, 0x80);
	lin_asclin_flagsset(base, (Ifx_ASCLIN_FLAGSSET) { .B.TWRQS = 1 });

	/* HW clears TWRQ when the pulse starts (not when it completes). Poll for
	 * that hand-off; WAIT_FOR caps at 2 ms so a stuck chip can't deadlock.
	 */
	if (!WAIT_FOR(base->FLAGS.B.TWRQ == 0, 2000, k_busy_wait(10))) {
		return -ETIMEDOUT;
	}

	event.type = LIN_EVT_TX_WAKEUP;
	event.status = 0;
	if (data->common.callback) {
		data->common.callback(dev, &event, data->common.callback_data);
	}

	/* Re-arm idle so subsequent falling edge isn't misread as another wake-up. */
	lin_asclin_rearm_bus_idle(dev);

	return 0;
}

static int lin_asclin_set_callback(const struct device *dev, lin_event_callback_t cb,
				   void *user_data)
{
	struct lin_asclin_data *data = dev->data;
	int key = irq_lock();

	data->common.callback = cb;
	data->common.callback_data = user_data;

	irq_unlock(key);
	return 0;
}

static int lin_asclin_set_rx_filter(const struct device *dev, const struct lin_filter *filter)
{
	struct lin_asclin_data *data = dev->data;

	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}

	if (filter == NULL) {
		data->filter_active = false;
		return 0;
	}

	data->filter = *filter;
	data->filter_active = true;
	return 0;
}

static DEVICE_API(lin, lin_asclin_driver_api) = {
	.start = lin_asclin_start,
	.stop = lin_asclin_stop,
	.configure = lin_asclin_configure,
	.get_config = lin_asclin_get_config,
	.send = lin_asclin_send,
	.receive = lin_asclin_receive,
	.response = lin_asclin_response,
	.read = lin_asclin_read,
	.wakeup_send = lin_asclin_wakeup_send,
	.set_callback = lin_asclin_set_callback,
	.set_rx_filter = lin_asclin_set_rx_filter,
};

static int lin_asclin_init(const struct device *dev)
{
	const struct lin_asclin_config *config = dev->config;
	struct lin_asclin_data *data = dev->data;
	int ret;

	data->dev = dev;
	k_sem_init(&data->submit_sem, 1, 1);
	k_work_init_delayable(&data->timeout_work, lin_asclin_timeout_handler);
	k_work_init_delayable(&data->bus_idle_work, lin_asclin_bus_idle_handler);
	atomic_set(&data->state, LIN_ASCLIN_STATE_IDLE);
	atomic_set(&data->bus_state, 0);
	data->filter_active = false;

	if (!device_is_ready(config->clkctrl)) {
		return -EIO;
	}

	ret = clock_control_on(config->clkctrl, (clock_control_subsys_t)&config->clk);
	if (ret < 0) {
		return ret;
	}

	if (!aurix_enable_clock((uintptr_t)&config->base->CLC, 1000)) {
		return -EIO;
	}

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	ret = lin_asclin_hw_configure(dev, &data->common.config);
	if (ret < 0) {
		return ret;
	}

	config->irq_config_func(dev);
	return 0;
}

#define LIN_ASCLIN_CLOCK_ID(n)									\
	COND_CODE_1(DT_INST_CLOCKS_HAS_NAME(n, fasclinf),					\
		(DT_INST_CLOCKS_CELL_BY_NAME(n, fasclinf, id)),					\
		(DT_INST_CLOCKS_CELL_BY_NAME(n, fasclins, id)))

#define LIN_ASCLIN_CLOCK_SRC(n)									\
	COND_CODE_1(DT_INST_CLOCKS_HAS_NAME(n, fasclinf),					\
		(LIN_ASCLIN_CLOCK_FASCLINF),							\
		(LIN_ASCLIN_CLOCK_FASCLINS))

#define LIN_ASCLIN_INIT(n)									\
												\
	PINCTRL_DT_INST_DEFINE(n);								\
												\
	static void lin_asclin_irq_config_##n(const struct device *dev)				\
	{											\
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, tx, irq),					\
			DT_INST_IRQ_BY_NAME(n, tx, priority),					\
			lin_asclin_tx_isr, DEVICE_DT_INST_GET(n), 0);				\
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, rx, irq),					\
			DT_INST_IRQ_BY_NAME(n, rx, priority),					\
			lin_asclin_rx_isr, DEVICE_DT_INST_GET(n), 0);				\
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, err, irq),					\
			DT_INST_IRQ_BY_NAME(n, err, priority),					\
			lin_asclin_err_isr, DEVICE_DT_INST_GET(n), 0);				\
		irq_enable(DT_INST_IRQ_BY_NAME(n, tx, irq));					\
		irq_enable(DT_INST_IRQ_BY_NAME(n, rx, irq));					\
		irq_enable(DT_INST_IRQ_BY_NAME(n, err, irq));					\
	}											\
												\
	static const struct lin_asclin_config lin_asclin_cfg_##n = {				\
		.common = LIN_DT_DRIVER_CONFIG_INST_GET(n, 1000, 20000),			\
		.base = (Ifx_ASCLIN *)DT_INST_REG_ADDR(n),					\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),					\
		.clkctrl = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),				\
		.clk = LIN_ASCLIN_CLOCK_ID(n),							\
		.clk_src = LIN_ASCLIN_CLOCK_SRC(n),						\
		.oversampling = DT_INST_PROP_OR(n, oversampling, 16),				\
		.samplepoint = DT_INST_PROP_OR(n, samplepoint, 8),				\
		.median_filter = DT_INST_PROP(n, median_filter),				\
		.prescaler = DT_INST_PROP_OR(n, prescaler, 1),					\
		.brg_numerator = DT_INST_PROP_OR(n, brg_numerator, 0),				\
		.brg_denominator = DT_INST_PROP_OR(n, brg_denominator, 0),			\
		.bus_idle_timeout_ms = DT_INST_PROP_OR(n, bus_idle_timeout_ms, 4000),		\
		.irq_config_func = lin_asclin_irq_config_##n,					\
	};											\
												\
	static struct lin_asclin_data lin_asclin_data_##n = {					\
		.common.config = {								\
			.mode = DT_INST_PROP(n, commander)					\
				? LIN_MODE_COMMANDER : LIN_MODE_RESPONDER,			\
			.baudrate = DT_INST_PROP_OR(n, bitrate, CONFIG_LIN_DEFAULT_BITRATE),	\
			.break_len = DT_INST_PROP(n, break_len),				\
			.break_delimiter_len = DT_INST_PROP(n, break_delimiter),		\
			.flags = (DT_INST_PROP(n, auto_sync) ? LIN_BUS_AUTO_SYNC : 0) |		\
				 (DT_INST_PROP(n, conflict_detection) ?				\
					 LIN_BUS_CONFLICT_DETECTION : 0),			\
		},										\
	};											\
												\
	DEVICE_DT_INST_DEFINE(n, lin_asclin_init, NULL,						\
			&lin_asclin_data_##n, &lin_asclin_cfg_##n,				\
			POST_KERNEL, CONFIG_LIN_INIT_PRIORITY,					\
			&lin_asclin_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LIN_ASCLIN_INIT)
