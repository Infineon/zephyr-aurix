/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/watchdog.h>

#include <math.h>
#include <soc.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wdt_aurix, CONFIG_WDT_LOG_LEVEL);

#define DT_DRV_COMPAT infineon_aurix_watchdog

struct wdt_aurix_config {
	mm_reg_t base;
	uint16_t password;
};

struct wdt_aurix_data {
	uint16_t reload;
	uint8_t ifs;
};

#if CONFIG_SOC_SERIES_TC3X
#include <IfxScu_reg.h>
#define WDT_CTRLA(base) base + offsetof(Ifx_SCU_WDTCPU, CON0)
#define WDT_CTRLB(base) base + offsetof(Ifx_SCU_WDTCPU, CON1)
#define WDT_STAT(base)  base + offsetof(Ifx_SCU_WDTCPU, SR)
#define Ifx_WTU_CTRLA   Ifx_SCU_WDTCPU_CON0
#define Ifx_WTU_CTRLB   Ifx_SCU_WDTCPU_CON1
#define Ifx_WTU_STAT    Ifx_SCU_WDTCPU_SR
#elif CONFIG_SOC_SERIES_TC4X
#include <IfxWtu_regdef.h>
#define WDT_CTRLA(base) base + offsetof(Ifx_WTU_WDTCPU, CTRLA)
#define WDT_CTRLB(base) base + offsetof(Ifx_WTU_WDTCPU, CTRLB)
#define WDT_STAT(base)  base + offsetof(Ifx_WTU_WDTCPU, STAT)
#endif

static inline void wdt_aurix_unlock(const struct device *dev)
{
	const struct wdt_aurix_config *config = dev->config;
	Ifx_WTU_CTRLA wtu_ctrla;

	wtu_ctrla.U = sys_read32(WDT_CTRLA(config->base));
	if (wtu_ctrla.B.LCK) {
		wtu_ctrla.B.LCK = 0;
#if CONFIG_SOC_SERIES_TC3X
		wtu_ctrla.B.PW = config->password;
		wtu_ctrla.B.ENDINIT = 1;
#elif CONFIG_SOC_SERIES_TC4X
		
		wtu_ctrla.U ^= (0x7FU << 1);
#endif
		sys_write32(wtu_ctrla.U, WDT_CTRLA(config->base));
	}

#if CONFIG_SOC_SERIES_TC3X
	wtu_ctrla.B.LCK = 1;
	wtu_ctrla.B.ENDINIT = 0;
	sys_write32(wtu_ctrla.U, WDT_CTRLA(config->base));
#endif
}

static inline void wdt_aurix_lock(const struct device *dev)
{
	const struct wdt_aurix_config *config = dev->config;
#if CONFIG_SOC_SERIES_TC3X
	const struct wdt_aurix_data *data = dev->data;
#endif
	Ifx_WTU_CTRLA wtu_ctrla;

#if CONFIG_SOC_SERIES_TC3X
	wtu_ctrla.U = sys_read32(WDT_CTRLA(config->base));
	if (wtu_ctrla.B.LCK) {
		wtu_ctrla.B.LCK = 0;
		wtu_ctrla.B.PW = config->password;
		wtu_ctrla.B.ENDINIT = 1;
		sys_write32(wtu_ctrla.U, WDT_CTRLA(config->base));
	}

	wtu_ctrla = (Ifx_WTU_CTRLA){
		.B.ENDINIT = 1, .B.LCK = 1, .B.REL = data->reload, .B.PW = config->password};
	sys_write32(wtu_ctrla.U, WDT_CTRLA(config->base));
#elif CONFIG_SOC_SERIES_TC4X
	wtu_ctrla.U = sys_read32(WDT_CTRLA(config->base));
	wtu_ctrla.B.LCK = 1;
	sys_write32(wtu_ctrla.U, WDT_CTRLA(config->base));
#endif
}

static inline void wdt_aurix_configure(const struct device *dev, bool enable)
{
	const struct wdt_aurix_config *config = dev->config;
	struct wdt_aurix_data *data = dev->data;
	Ifx_WTU_CTRLB ctrlb = {0};

	ctrlb.B.DR = !enable;
#if CONFIG_SOC_SERIES_TC3X
	ctrlb.B.IR0 = data->ifs & 1;
	ctrlb.B.IR1 = (data->ifs >> 1) & 1;
#elif CONFIG_SOC_SERIES_TC4X
	ctrlb.B.TIMR = data->reload;
	ctrlb.B.IFSR = data->ifs;
#endif

	sys_write32(ctrlb.U, WDT_CTRLB(config->base));
}

static int wdt_aurix_disable(const struct device *dev)
{
	const struct wdt_aurix_config *config = dev->config;

	wdt_aurix_unlock(dev);
	wdt_aurix_configure(dev, false);
	wdt_aurix_lock(dev);

	return 0;
}

static int wdt_aurix_feed(const struct device *dev, int channel_id)
{
	const struct wdt_aurix_config *config = dev->config;

	wdt_aurix_unlock(dev);
	wdt_aurix_lock(dev);

	return 0;
}

static int wdt_aurix_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *config)
{
	struct wdt_aurix_data *data = dev->data;

	if (config->window.min != 0) {
		return -EINVAL;
	}

	float rate = (float)config->window.max / (float)MSEC_PER_SEC * 100.0e6f;
	float reload;

	if ((reload = rate / 64.0f) <= 65535.0f) {
		data->reload = (uint16_t)(65535.0f - roundf(ceilf(reload)));
		data->ifs = 2;
	} else if ((reload = rate / 256.0f) <= 65535.0f) {
		data->reload = (uint16_t)(65535.0f - roundf(ceilf(reload)));
		data->ifs = 1;
	} else if ((reload = rate / 16384.0f) <= 65535.0f) {
		data->reload = (uint16_t)(65535.0f - roundf(ceilf(reload)));
		data->ifs = 0;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int wdt_aurix_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_aurix_config *config = dev->config;
	struct wdt_aurix_data *data = dev->data;
	Ifx_WTU_CTRLA ctrla;
	Ifx_WTU_CTRLB ctrlb;

	wdt_aurix_unlock(dev);
	wdt_aurix_configure(dev, true);
	wdt_aurix_lock(dev);

	return 0;
}

static int wdt_aurix_init(const struct device *dev)
{

	const struct wdt_aurix_config *config = dev->config;
	struct wdt_aurix_data *data = dev->data;
	Ifx_WTU_CTRLA ctrla;
	Ifx_WTU_CTRLB ctrlb;

	data->reload = 0xFFFC;

	ctrla.U = sys_read32(WDT_CTRLA(config->base));
	if (ctrla.B.LCK) {
		ctrla.B.LCK = 0;
#if CONFIG_SOC_SERIES_TC3X
		ctrla.B.PW ^= 0x003F;
		ctrla.B.ENDINIT = 1;
#elif CONFIG_SOC_SERIES_TC4X
		ctrla.B.PW ^= 0x007F;
#endif
		sys_write32(ctrla.U, WDT_CTRLA(config->base));
	}
	
#if CONFIG_SOC_SERIES_TC3X
	ctrla.B.ENDINIT = 0;
	ctrla.B.LCK = 1;
	sys_write32(ctrla.U, WDT_CTRLA(config->base));
#endif
	wdt_aurix_configure(dev, !IS_ENABLED(CONFIG_WDT_DISABLE_AT_BOOT));
	wdt_aurix_lock(dev);

	return 0;
};

struct wdt_driver_api wdt_aurix_api = {
	.disable = wdt_aurix_disable,
	.feed = wdt_aurix_feed,
	.install_timeout = wdt_aurix_install_timeout,
	.setup = wdt_aurix_setup,
};

#define WDT_AURIX_CPU_INIT(n)                                                                      \
	struct wdt_aurix_config wdt_aurix_config_##n = {                                           \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.password = 0x007C,                                                                \
	};                                                                                         \
	struct wdt_aurix_data wdt_aurix_data_##n;                                                  \
	DEVICE_DT_INST_DEFINE(n, wdt_aurix_init, NULL, &wdt_aurix_data_##n, &wdt_aurix_config_##n, \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_aurix_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_AURIX_CPU_INIT)
