/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_tc3x_gpio

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#include <IfxGtm_reg.h>
#include <soc.h>

#include "gpio_tc3x.h"

/* TC3x GTM TIM unit base addresses (TIM0..TIM7 at 0x800 stride),
 * channel sub-stride within a TIM is 0x80. The Ifx_GTM_TIM struct
 * has CH0..CH7 as named fields (with reserved gaps), so step via
 * raw addresses instead of array indexing.
 */
#define TC3X_GTM_TIM_BASE        0xF0101000u
#define TC3X_GTM_TIM_STRIDE      0x800u
#define TC3X_GTM_TIM_CH_STRIDE   0x80u

#define TC3X_GTM_TIM_CH_CTRL_OFF     0x24
#define TC3X_GTM_TIM_CH_NOTIFY_OFF   0x2C
#define TC3X_GTM_TIM_CH_IRQ_EN_OFF   0x30
#define TC3X_GTM_TIM_CH_IRQ_MODE_OFF 0x38

/**
 * @brief Common gpio flags to custom flags
 */
static int gpio_tc3x_flags_to_iocr(gpio_flags_t flags, uint32_t *iocr)
{
	bool is_input = flags & GPIO_INPUT;
	bool is_output = flags & GPIO_OUTPUT;

	/* Disconnect not supported */
	if (!is_input && !is_output) {
		return -ENOTSUP;
	}

	/* Open source not supported*/
	if (flags & GPIO_OPEN_SOURCE) {
		return -ENOTSUP;
	}

	/* Pull up & pull down not supported in output mode */
	if (is_output && (flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
		return -ENOTSUP;
	}

	if (is_input) {
		if (flags & (GPIO_PULL_UP)) {
			*iocr = TC3X_GPIO_MODE_INPUT_PULL_UP;
		} else if (flags & (GPIO_PULL_DOWN)) {
			*iocr = TC3X_GPIO_MODE_INPUT_PULL_DOWN;
		} else {
			*iocr = TC3X_GPIO_MODE_INPUT_TRISTATE;
		}
	}
	if (is_output) {
		if (flags & GPIO_OPEN_DRAIN) {
			*iocr = TC3X_GPIO_MODE_OUTPUT_OPEN_DRAIN;
		} else {
			*iocr = TC3X_GPIO_MODE_OUTPUT_PUSH_PULL;
		}
	}

	return 0;
}

#if defined(CONFIG_GPIO_GET_CONFIG)
static int gpio_tc3x_pincfg_to_flags(uint32_t iocr, uint32_t out, gpio_flags_t *out_flags)
{
	if (iocr & TC3X_IOCR_OUTPUT) {
		if (out) {
			*out_flags = GPIO_OUTPUT_HIGH;
		} else {
			*out_flags = GPIO_OUTPUT_LOW;
		}
		if (iocr & TC3X_IOCR_OPEN_DRAIN) {
			*out_flags |= GPIO_OPEN_DRAIN;
		}
	} else {
		*out_flags = GPIO_INPUT;
		if (iocr & TC3X_IOCR_PULL_DOWN) {
			*out_flags |= GPIO_PULL_DOWN;
		} else if (iocr & TC3X_IOCR_PULL_UP) {
			*out_flags |= GPIO_PULL_UP;
		}
	}

	return 0;
}
#endif

static int gpio_tc3x_port_get_raw(const struct device *dev, uint32_t *value)
{
	const struct gpio_tc3x_config *cfg = dev->config;

	*value = sys_read32(cfg->base + TC3X_IN_OFFSET);

	return 0;
}

static int gpio_tc3x_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					  gpio_port_value_t value)
{
	const struct gpio_tc3x_config *cfg = dev->config;
	uint32_t clear, set;
	mask &= 0xFFFF;
	value &= 0xFFFF;

	set = (mask & value);
	clear = (mask & ~value);

	sys_write32((clear << 16) | set, cfg->base + TC3X_OMR_OFFSET);

	return 0;
}

static int gpio_tc3x_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_tc3x_config *cfg = dev->config;

	sys_write32(0xFFFF & pins, cfg->base + TC3X_OMR_OFFSET);

	return 0;
}

static int gpio_tc3x_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_tc3x_config *cfg = dev->config;

	sys_write32((0xFFFF & pins) << 16, cfg->base + TC3X_OMR_OFFSET);

	return 0;
}

static int gpio_tc3x_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_tc3x_config *cfg = dev->config;
	uint32_t out;
	uint64_t swap;

	do {
		out = sys_read32(cfg->base + TC3X_OUT_OFFSET);
		swap = ((uint64_t)out << 32) | (out ^ pins);
		__asm("	cmpswap.w [%1]+0, %A0\n" : "+d"(swap) : "a"((void *)cfg->base));
	} while ((swap & 0xFFFFFFFF) != out);

	return 0;
}

/**
 * @brief Configure pin or port
 */
static int gpio_tc3x_config(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_tc3x_config *cfg = dev->config;
	int err;
	uint32_t iocr = 0;

	/* figure out if we can map the requested GPIO
	 * configuration
	 */
	err = gpio_tc3x_flags_to_iocr(flags, &iocr);
	if (err != 0) {
		return err;
	}

	if ((flags & GPIO_OUTPUT) != 0) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
			gpio_tc3x_port_set_bits_raw(dev, BIT(pin));
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
			gpio_tc3x_port_clear_bits_raw(dev, BIT(pin));
		}
	}

	__asm("	imask %%e14, %0, %1, 5\n"
	      "	ldmst [%2]+0, %%e14\n"
	      :
	      : "d"(iocr), "d"((pin & 0x3) * 8 + 3),
          "a"(cfg->base + TC3X_IOCR_OFFSET + (pin >> 2)*4)
	      : "e14");

	return 0;
}

#if defined(CONFIG_GPIO_GET_CONFIG)
/**
 * @brief Get configuration of pin
 */
static int gpio_tc3x_get_config(const struct device *dev, gpio_pin_t pin, gpio_flags_t *flags)
{
	const struct gpio_tc3x_config *cfg = dev->config;

	gpio_tc3x_pincfg_to_flags(sys_read32(cfg->base + TC3X_IOCR_OFFSET),
				  sys_read32(cfg->base + TC3X_OUT_OFFSET), flags);

	return 0;
}
#endif

static int gpio_tc3x_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					      enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct gpio_tc3x_config *cfg = dev->config;
	const struct gpio_tc3x_irq_source *irq_src = NULL;
	static bool gtm_initialized;

	for (uint32_t i = 0; i < cfg->irq_source_count; i++) {
		if (cfg->irq_sources[i].pin == pin) {
			irq_src = &cfg->irq_sources[i];
			break;
		}
	}

	if (irq_src == NULL) {
		return -ENOTSUP;
	}

	if (irq_src->type != TC3X_IRQ_TYPE_GTM) {
		return -ENOTSUP;
	}

	if (MODULE_GTM.CLC.B.DISS == 1) {
		if (!aurix_enable_clock((uintptr_t)&MODULE_GTM.CLC, 1000)) {
			return -EIO;
		}
	}

	if (!gtm_initialized) {
		/* GCLK_NUM/DEN are CPU-EndInit protected on TC3x. */
		aurix_cpu_endinit_enable(false);
		sys_write32(1, (mem_addr_t)&MODULE_GTM.CMU.GCLK_NUM);
		sys_write32(1, (mem_addr_t)&MODULE_GTM.CMU.GCLK_DEN);
		aurix_cpu_endinit_enable(true);

		/* CMU.CLK_EN is a 2-bit-per-field register; 0b10 force-enables. */
		Ifx_GTM_CMU_CLK_EN clk_en = {.U = 0};
		clk_en.B.EN_CLK0 = 0x2;
		clk_en.B.EN_CLK1 = 0x2;
		clk_en.B.EN_CLK2 = 0x2;
		clk_en.B.EN_CLK3 = 0x2;
		clk_en.B.EN_CLK4 = 0x2;
		clk_en.B.EN_CLK5 = 0x2;
		clk_en.B.EN_CLK6 = 0x2;
		clk_en.B.EN_CLK7 = 0x2;
		clk_en.B.EN_ECLK0 = 0x2;
		clk_en.B.EN_ECLK1 = 0x2;
		clk_en.B.EN_ECLK2 = 0x2;
		clk_en.B.EN_FXCLK = 0x2;
		sys_write32(clk_en.U, (mem_addr_t)&MODULE_GTM.CMU.CLK_EN);
		gtm_initialized = true;
	}

	MODULE_GTM.TIMINSEL[irq_src->tim].U =
		(MODULE_GTM.TIMINSEL[irq_src->tim].U & ~(0xFu << (4 * irq_src->ch))) |
		((uint32_t)irq_src->mux << (4 * irq_src->ch));

	uintptr_t ch_base = TC3X_GTM_TIM_BASE +
			    (uintptr_t)irq_src->tim * TC3X_GTM_TIM_STRIDE +
			    (uintptr_t)irq_src->ch * TC3X_GTM_TIM_CH_STRIDE;

	Ifx_GTM_TIM_CH_CTRL ctrl = {.U = 0};
	ctrl.B.CLK_SEL = 7;
	ctrl.B.TIM_MODE = mode == GPIO_INT_MODE_EDGE ? 0x2 : 0x5;
	ctrl.B.DSL = trig == GPIO_INT_TRIG_HIGH ? 1 : 0;
	ctrl.B.ISL = trig == GPIO_INT_TRIG_BOTH ? 1 : 0;

	sys_write32(ctrl.U, ch_base + TC3X_GTM_TIM_CH_CTRL_OFF);
	sys_write32(0x3F, ch_base + TC3X_GTM_TIM_CH_NOTIFY_OFF);
	sys_write32(0x1, ch_base + TC3X_GTM_TIM_CH_IRQ_MODE_OFF);
	sys_write32(0x1, ch_base + TC3X_GTM_TIM_CH_IRQ_EN_OFF);
	ctrl.B.TIM_EN = 1;
	sys_write32(ctrl.U, ch_base + TC3X_GTM_TIM_CH_CTRL_OFF);

	return 0;
}

static int gpio_tc3x_manage_callback(const struct device *dev, struct gpio_callback *callback,
				      bool set)
{
	struct gpio_tc3x_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static DEVICE_API(gpio, gpio_tc3x_driver) = {
	.pin_configure = gpio_tc3x_config,
#if defined(CONFIG_GPIO_GET_CONFIG)
	.pin_get_config = gpio_tc3x_get_config,
#endif /* CONFIG_GPIO_GET_CONFIG */
	.port_get_raw = gpio_tc3x_port_get_raw,
	.port_set_masked_raw = gpio_tc3x_port_set_masked_raw,
	.port_set_bits_raw = gpio_tc3x_port_set_bits_raw,
	.port_clear_bits_raw = gpio_tc3x_port_clear_bits_raw,
	.port_toggle_bits = gpio_tc3x_port_toggle_bits,
	.pin_interrupt_configure = gpio_tc3x_pin_interrupt_configure,
	.manage_callback = gpio_tc3x_manage_callback,
};

static int gpio_tc3x_init(const struct device *dev)
{
	const struct gpio_tc3x_config *cfg = dev->config;
	struct gpio_tc3x_data *data = dev->data;

	sys_slist_init(&data->callbacks);
	if (cfg->config_func) {
		cfg->config_func(dev);
	}

	return 0;
}

#define GPIO_TC3X_ISR(inst)                                                                        \
	static void __maybe_unused gpio_tc3x_isr_##inst(struct gpio_tc3x_irq_source *irq_src)      \
	{                                                                                          \
		const struct device *dev = DEVICE_DT_INST_GET(inst);                               \
		struct gpio_tc3x_data *data = dev->data;                                           \
		uintptr_t ch_base = TC3X_GTM_TIM_BASE +                                            \
				    (uintptr_t)irq_src->tim * TC3X_GTM_TIM_STRIDE +                \
				    (uintptr_t)irq_src->ch * TC3X_GTM_TIM_CH_STRIDE;               \
		/* Acknowledge by clearing IRQ_NOTIFY (write-1-to-clear). */                       \
		sys_write32(0x3F, ch_base + 0x2C);                                                 \
		gpio_fire_callbacks(&data->callbacks, dev, BIT(irq_src->pin));                     \
	}

#define GPIO_TC3X_IRQ_CONFIGURE(n, inst)                                                           \
	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, n, irq), DT_INST_IRQ_BY_IDX(inst, n, priority),       \
		    gpio_tc3x_isr_##inst, &gpio_tc3x_irq_sources_##inst[n], 0);                    \
	irq_enable(DT_INST_IRQ_BY_IDX(inst, n, irq));

#define GPIO_TC3X_ALL_IRQS(inst) LISTIFY(DT_INST_NUM_IRQS(inst), GPIO_TC3X_IRQ_CONFIGURE, (), inst)

#define GPIO_TC3X_CONFIG_FUNC(n)                                                                   \
	GPIO_TC3X_ISR(n)                                                                           \
	static void gpio_tc3x_config_func_##n(const struct device *dev)                            \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		GPIO_TC3X_ALL_IRQS(n)                                                              \
	}

#define GPIO_TC3X_IRQ_SOURCE(node_id, prop, i)                                                     \
	{(DT_PROP_BY_IDX(node_id, prop, i) & 0xF),                                                 \
	 ((DT_PROP_BY_IDX(node_id, prop, i) >> 4) & 0xF),                                          \
	 ((DT_PROP_BY_IDX(node_id, prop, i) >> 8) & 0xF),                                          \
	 ((DT_PROP_BY_IDX(node_id, prop, i) >> 12) & 0x3),                                         \
	 ((DT_PROP_BY_IDX(node_id, prop, i) >> 28) & 0xF)}

#define GPIO_TC3X_INIT(n)                                                                          \
	static const struct gpio_tc3x_irq_source gpio_tc3x_irq_sources_##n[] = {COND_CODE_1(       \
		DT_INST_NODE_HAS_PROP(n, irq_sources),                                             \
		(DT_INST_FOREACH_PROP_ELEM_SEP(n, irq_sources, GPIO_TC3X_IRQ_SOURCE, (, ))),       \
		())};                                                                              \
	GPIO_TC3X_CONFIG_FUNC(n)                                                                   \
	static const struct gpio_tc3x_config gpio_tc3x_config_##n = {                              \
		.common =                                                                          \
			{                                                                          \
				.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n),               \
			},                                                                         \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.config_func = gpio_tc3x_config_func_##n,                                          \
		.irq_sources = gpio_tc3x_irq_sources_##n,                                          \
		.irq_source_count = DT_INST_PROP_LEN_OR(n, irq_sources, 0),                        \
	};                                                                                         \
                                                                                                   \
	static struct gpio_tc3x_data gpio_tc3x_data_##n = {};                                      \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_tc3x_init, NULL, &gpio_tc3x_data_##n,                        \
			      &gpio_tc3x_config_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,      \
			      &gpio_tc3x_driver);

DT_INST_FOREACH_STATUS_OKAY(GPIO_TC3X_INIT)
