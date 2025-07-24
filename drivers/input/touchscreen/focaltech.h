/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Danila Tikhonov <danila@jiaxyga.com>
 */

#ifndef __FOCALTECH_H_
#define __FOCALTECH_H_

#include <linux/pm.h>

struct focaltech_ic_ids {
	u16 type;
	u8 chip_idh;
	u8 chip_idl;
	u8 rom_idh;
	u8 rom_idl;
	u8 pb_idh;
	u8 pb_idl;
	u8 bl_idh;
	u8 bl_idl;
};

struct focaltech_ic_data {
	bool is_incell;
	bool hid_supported;
	struct focaltech_ic_ids ids;
	ssize_t spi_prefix_len;
};

struct device;
struct input_id;
struct regmap;

int focaltech_probe(struct device *dev, int irq, const struct input_id *id,
		    struct regmap *regmap,
		    const struct focaltech_ic_data *ic_data);

extern const struct dev_pm_ops focaltech_pm_ops;
extern const struct attribute_group *focaltech_groups[];

#endif
