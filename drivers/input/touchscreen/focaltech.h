/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on goodix_berlin driver
 */

#ifndef __FOCALTECH_H_
#define __FOCALTECH_H_

#include <linux/input/touchscreen.h>
#include <linux/pm.h>

#define FOCALTECH_CMD_START1		0x55
#define FOCALTECH_CMD_START2		0xaa
#define FOCALTECH_CMD_READ_ID		0x90

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

struct focaltech_ic_settings {
	u32 app2_offset;
	u32 ecclen_max;
	u8 eccok_val;
	u8 upgsts_boot;
	u8 delay_init;
	u8 spi_pe;
	u8 length_coefficient;
	u8 fd_check;
	u8 drwr_support;
	u8 ecc_delay;
};

struct focaltech_ic_data {
	bool is_incell;
	bool hid_supported;
	struct focaltech_ic_ids ids;
	struct focaltech_ic_settings settings;
	ssize_t spi_prefix_len;
	ssize_t data_len;
};

struct focaltech_fw_status {
	bool is_fw_loading;
	bool is_fw_running;
};

struct focaltech_core {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct touchscreen_properties props;
	struct input_dev *input_dev;
	const char *fw_path;
	int irq;

	const struct focaltech_ic_data *ic_data;
	struct focaltech_fw_status *fw_status;
};

struct device;
struct input_id;
struct regmap;

/* Core funcs */
int focaltech_probe(struct device *dev, int irq, const struct input_id *id,
		    struct regmap *regmap,
		    const struct focaltech_ic_data *ic_data);
void focaltech_request_handle_reset(struct focaltech_core *cd, int sleepms);

/* Firmware uploader funcs */
void focaltech_fw_recovery(struct focaltech_core *cd);
int focaltech_fwupload(struct focaltech_core *cd);

extern const struct dev_pm_ops focaltech_pm_ops;
extern const struct attribute_group *focaltech_groups[];

#endif
