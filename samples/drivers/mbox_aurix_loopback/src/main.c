/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-core GPSR loopback for the AURIX MBOX driver.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/printk.h>

#define MBOX_NODE       DT_PATH(mbox_consumer)
#define ITERATIONS      200
#define LOG_EVERY       20

static volatile uint32_t rx_count;
static volatile uint16_t last_rx;
static volatile bool     rx_gap;
static struct k_sem      rx_sem;

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		  void *user_data, struct mbox_msg *msg)
{
	uint16_t value = 0;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (msg && msg->size == sizeof(value)) {
		memcpy(&value, msg->data, sizeof(value));
	}
	if (rx_count != 0U && (uint16_t)(last_rx + 1U) != value) {
		rx_gap = true;
	}
	last_rx = value;
	rx_count++;
	k_sem_give(&rx_sem);
}

int main(void)
{
	const struct mbox_dt_spec tx = MBOX_DT_SPEC_GET(MBOX_NODE, tx);
	const struct mbox_dt_spec rx = MBOX_DT_SPEC_GET(MBOX_NODE, rx);
	int ret;
	uint16_t counter;

	printk("mbox_aurix_loopback start (%s)\n", CONFIG_BOARD_TARGET);
	printk("  mtu=%d max_channels=%d tx.ch=%d rx.ch=%d\n",
	       mbox_mtu_get_dt(&tx), mbox_max_channels_get_dt(&tx),
	       tx.channel_id, rx.channel_id);

	k_sem_init(&rx_sem, 0, 1);

	ret = mbox_register_callback_dt(&rx, rx_cb, NULL);
	if (ret != 0) {
		printk("FAIL register_callback: %d\n", ret);
		return 0;
	}
	ret = mbox_set_enabled_dt(&rx, true);
	if (ret != 0) {
		printk("FAIL set_enabled: %d\n", ret);
		return 0;
	}

	for (counter = 0U; counter < ITERATIONS; counter++) {
		struct mbox_msg msg = {
			.data = &counter,
			.size = sizeof(counter),
		};

		ret = mbox_send_dt(&tx, &msg);
		if (ret != 0) {
			printk("FAIL send #%u: %d\n", counter, ret);
			return 0;
		}

		ret = k_sem_take(&rx_sem, K_MSEC(50));
		if (ret != 0) {
			printk("FAIL no rx for #%u: %d\n", counter, ret);
			return 0;
		}

		if ((counter % LOG_EVERY) == (LOG_EVERY - 1U)) {
			printk("ok #%u rx=%u rx_count=%u\n",
			       counter, last_rx, rx_count);
		}
	}

	if (rx_gap) {
		printk("FAIL sequence gap detected; last_rx=%u rx_count=%u\n",
		       last_rx, rx_count);
		return 0;
	}
	if (rx_count != ITERATIONS) {
		printk("FAIL rx_count=%u expected=%u\n", rx_count, ITERATIONS);
		return 0;
	}
	printk("PASS %u messages, last=%u\n", rx_count, last_rx);
	return 0;
}
