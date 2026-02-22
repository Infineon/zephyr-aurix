/*
 * Copyright (c) 2025 Linumiz
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <string.h>

struct flags_no_pad {
	uint32_t a :1;
	uint32_t b :1;
	uint32_t c :1;
	uint32_t d :1;
	uint32_t e :1;
	uint32_t f :1;
};

struct flags_padded {
	uint32_t a :1;
	uint32_t b :1;
	uint32_t c :1;
	uint32_t d :1;
	uint32_t e :1;
	uint32_t f :1;
	uint32_t _reserved :26;
};

static volatile struct {
	struct flags_no_pad flags;
	uint32_t canary;
} no_pad;

static volatile struct {
	struct flags_padded flags;
	uint32_t canary;
} padded;

int main(void)
{
	printk("sizeof no_pad.flags = %lu (expected 4)\n",
	       sizeof(no_pad.flags));
	printk("sizeof padded.flags = %lu (expected 4)\n",
	       sizeof(padded.flags));

	memset((void *)&no_pad, 0xAA, sizeof(no_pad));
	no_pad.flags = (struct flags_no_pad){.a = 1, .c = 1, .e = 1};
	printk("no_pad:  canary=0x%08x (expect 0xAAAAAAAA)\n", no_pad.canary);

	memset((void *)&padded, 0xAA, sizeof(padded));
	padded.flags = (struct flags_padded){.a = 1, .c = 1, .e = 1};
	printk("padded:  canary=0x%08x (expect 0xAAAAAAAA)\n", padded.canary);

	return 0;
}
