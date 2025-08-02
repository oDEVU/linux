// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2012-2020, Focaltech Ltd. All rights reserved.
 * Copyright (C) 2025 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on fts_ts and goodix_berlin_core drivers
 *
 * Support is missing for:
 * - ESD Management
 * - Stylus Events
 * - Gesture Events
 * - DRM Notifier
 */

#define DEBUG

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "focaltech.h"

#define FOCALTECH_CMD_START1		0x55
#define FOCALTECH_CMD_START2		0xaa
#define FOCALTECH_CMD_READ_ID		0x90

#define FOCALTECH_CMD_START_DELAY	12
#define FOCALTECH_TOUCH_E_NUM		1

#define FOCALTECH_MAX_TOUCH_BUF		4096
#define FOCALTECH_ADDR			0x01

#define FOCALTECH_TOUCH_DEFAULT		0x00
#define FOCALTECH_TOUCH_EVENT_NUM	0x02
#define FOCALTECH_TOUCH_EXTRA_MSG	0x08
#define FOCALTECH_TOUCH_PEN		0x0b
#define FOCALTECH_TOUCH_GESTURE		0x80
#define FOCALTECH_TOUCH_FW_INIT		0x81
#define FOCALTECH_TOUCH_IGNORE		0xfe
#define FOCALTECH_TOUCH_ERROR		0xff

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
};

static const struct regulator_bulk_data focaltech_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "iovdd" },
};

static int focaltech_request_handle_reset
				(struct focaltech_core *cd, int sleepms)
{
	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	gpiod_set_value_cansleep(cd->reset_gpio, 0);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(cd->reset_gpio, 1);

	if (sleepms)
		msleep(sleepms);

	return 0;
}

/* DEBUG */
static void focaltech_show_touch_buffer(
			struct focaltech_core *cd, u8 *data, u32 datalen)
{
	const u32 bufsize = 1024;
	u32 count = 0;

	char *tmpbuf __free(kfree) = kzalloc(bufsize, GFP_ATOMIC);
	if (!tmpbuf)
		return;

	for (int i = 0; i < datalen && count < bufsize - 1; i++)
		count += scnprintf
			(tmpbuf + count, bufsize - count, "%02X,", data[i]);

	dev_dbg(cd->dev, "touch_buf:%s", tmpbuf);
}

static irqreturn_t focaltech_irq(int irq, void *data)
{
	struct focaltech_core *cd = data;
	int ts_etype = 0;
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	u8 *touch_buf __free(kfree) =
			kmalloc(FOCALTECH_MAX_TOUCH_BUF, GFP_ATOMIC);
	if (!touch_buf)
		return IRQ_NONE;

	memset(touch_buf, 0xff, FOCALTECH_MAX_TOUCH_BUF);

	ret = regmap_raw_read(cd->regmap, FOCALTECH_ADDR,
					touch_buf, cd->ic_data->data_len); // ?
	if (ret) {
		dev_err(cd->dev, "Failed to get event data: %d\n", ret);
		ts_etype = FOCALTECH_TOUCH_ERROR;
	}

	if ((touch_buf[0] == 0xef) || ((touch_buf[1] == 0xef) &&
	    (touch_buf[2] == 0xef) && (touch_buf[3] == 0xef))) {
		/* fts_release_all_finger() */
		/* fts_fw_recovery() */
		ts_etype = FOCALTECH_TOUCH_ERROR;
	};

#ifdef DEBUG
	focaltech_show_touch_buffer(cd, touch_buf, cd->ic_data->data_len); // ?
#endif

	if ((touch_buf[1] == 0xff) && (touch_buf[2] == 0xff) &&
	    (touch_buf[3] == 0xff) && (touch_buf[4] == 0xff))
		ts_etype = FOCALTECH_TOUCH_FW_INIT;

	if (!ts_etype)
		ts_etype = ((touch_buf[FOCALTECH_TOUCH_E_NUM] >> 4) & 0x0F);

	dev_dbg(cd->dev, "%s: line: %d etype = %d\n",
						__func__, __LINE__, ts_etype);

	switch (ts_etype) {
	case FOCALTECH_TOUCH_DEFAULT:
		dev_dbg(cd->dev, "TOUCH_DEFAULT\n");
		break;
	case FOCALTECH_TOUCH_EVENT_NUM:
		dev_dbg(cd->dev, "TOUCH_EVENT_NUM\n");
		break;
	case FOCALTECH_TOUCH_EXTRA_MSG:
		dev_dbg(cd->dev, "TOUCH_EXTRA_MSG\n");
		break;
	case FOCALTECH_TOUCH_PEN:
		dev_dbg(cd->dev, "TOUCH_PEN\n");
		break;
	case FOCALTECH_TOUCH_GESTURE:
		dev_dbg(cd->dev, "TOUCH_GESTURE\n");
		break;
	case FOCALTECH_TOUCH_FW_INIT:
		/* fts_release_all_finger() */
		/* fts_fw_recovery() */
		dev_dbg(cd->dev, "TOUCH_FW_INIT\n");
		break;
	case FOCALTECH_TOUCH_IGNORE:
	case FOCALTECH_TOUCH_ERROR:
		dev_dbg(cd->dev, "TOUCH_IGNORE_ERROR\n");
		break;
	default:
		dev_info(cd->dev, "Unknown touch event: %d\n", ts_etype);
		break;
	}
	goto out;

out:
	return IRQ_HANDLED;
}

static int focaltech_read_bootid(struct focaltech_core *cd, u8 *id)
{
	u8 buf[] = { FOCALTECH_CMD_START1, FOCALTECH_CMD_START2 };
	u8 chip_id[2];
	int ret;

	ret = regmap_raw_write(cd->regmap, 0, buf, sizeof(buf));
	if (ret < 0) {
		dev_err(cd->dev, "Start cmd write fail: %d\n", ret);
		return ret;
	}

	usleep_range(FOCALTECH_CMD_START_DELAY * 1000,
		     FOCALTECH_CMD_START_DELAY * 1000 + 100);

	ret = regmap_bulk_read(cd->regmap, FOCALTECH_CMD_READ_ID,
						chip_id, sizeof(chip_id));
	if (ret) {
		dev_err(cd->dev, "Read BootID fail: %d\n", ret);
		return ret;
	}

	if (!chip_id[0] || !chip_id[1]) {
		dev_err(cd->dev, "Read BootID invalid: 0x%02x%02x\n",
							chip_id[0], chip_id[1]);
		return -EIO;
	}

	id[0] = chip_id[0];
	id[1] = chip_id[1];

	return 0;
}

static int focaltech_get_chip_types(struct focaltech_core *cd,
		const struct focaltech_ic_data *ic_data, u8 id_h, u8 id_l)
{
	const struct focaltech_ic_ids *c = &ic_data->ids;

	dev_dbg(cd->dev, "Verifying ID: 0x%02x%02x\n", id_h, id_l);

	if ((id_h == c->chip_idh && id_l == c->chip_idl) ||
	    (id_h == c->rom_idh  && id_l == c->rom_idl)  ||
	    (id_h == c->pb_idh   && id_l == c->pb_idl)   ||
	    (id_h == c->bl_idh   && id_l == c->bl_idl))  {
		dev_info(cd->dev, "Detected chip ID 0x%02x%02x\n", id_h, id_l);
		return 0;
	}

	dev_err(cd->dev,
		"No matching chip type for ID 0x%02x%02x\n", id_h, id_l);

	return -ENODATA;
}

static int focaltech_get_ic_information
	(struct focaltech_core *cd, const struct focaltech_ic_data *ic_data)
{
	u8 chip_id[2];
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	for (int cnt = 0; cnt < 3; cnt++) {
		focaltech_request_handle_reset(cd,
				FOCALTECH_CMD_START_DELAY + cnt * 8);

		ret = focaltech_read_bootid(cd, chip_id);
		if (ret < 0) {
			dev_dbg(cd->dev, "Read BootID failed, retry %d\n", cnt);
			continue;
		}

		ret = focaltech_get_chip_types
					(cd, ic_data, chip_id[0], chip_id[1]);
		if (ret < 0) {
			dev_dbg(cd->dev,
				"Chip type lookup failed, retry %d\n", cnt);
			continue;
		}

		return 0;
	}

	return -EIO;
}

static int focaltech_input_dev_config(struct focaltech_core *cd,
					  const struct input_id *id)
{
	struct input_dev *input_dev;
	int ret;

	input_dev = devm_input_allocate_device(cd->dev);
	if (!input_dev)
		return -ENOMEM;

	cd->input_dev = input_dev;
	input_set_drvdata(input_dev, cd);

	input_dev->name = "FocalTech TouchScreen";
	input_dev->phys = "input/ts";

	input_dev->id = *id;

	input_set_abs_params(cd->input_dev, ABS_MT_POSITION_X,
			     0, SZ_64K - 1, 0, 0);
	input_set_abs_params(cd->input_dev, ABS_MT_POSITION_Y,
			     0, SZ_64K - 1, 0, 0);
	input_set_abs_params(cd->input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xff, 0, 0);

	touchscreen_parse_properties(cd->input_dev, true, &cd->props);

#define FOCALTECH_MAX_TOUCH	10

	ret = input_mt_init_slots(cd->input_dev, FOCALTECH_MAX_TOUCH,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(cd->input_dev);
	if (ret)
		return ret;

	return 0;
}

static int focaltech_power_on(struct focaltech_core *cd)
{
	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	int ret = regulator_bulk_enable(ARRAY_SIZE(focaltech_supplies),
					cd->supplies);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(focaltech_supplies),
					cd->supplies);
		return ret;
	}

	gpiod_set_value_cansleep(cd->reset_gpio, 0);

	return 0;
}

static void focaltech_power_off(struct focaltech_core *cd)
{
	gpiod_set_value_cansleep(cd->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(focaltech_supplies),
					cd->supplies);
}


static int focaltech_suspend(struct device *dev)
{
	struct focaltech_core *cd = dev_get_drvdata(dev);
	dev_dbg(dev, "%s: line: %d\n", __func__, __LINE__);

	focaltech_power_off(cd);

	return 0;
}

static int focaltech_resume(struct device *dev)
{
	struct focaltech_core *cd = dev_get_drvdata(dev);
	dev_dbg(dev, "%s: line: %d\n", __func__, __LINE__);

	return focaltech_power_on(cd);;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(focaltech_pm_ops,
			     focaltech_suspend, focaltech_resume);

static void focaltech_power_off_act(void *data)
{
	struct focaltech_core *cd = data;
	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	focaltech_power_off(cd);
}

static ssize_t registers_read(struct file *filp, struct kobject *kobj,
			      const struct bin_attribute *bin_attr,
			      char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct focaltech_core *cd = dev_get_drvdata(dev);
	int ret;

	ret = regmap_raw_read(cd->regmap, off, buf, count);

	return ret ? ret : count;
}

static ssize_t registers_write(struct file *filp, struct kobject *kobj,
			       const struct bin_attribute *bin_attr,
			       char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct focaltech_core *cd = dev_get_drvdata(dev);
	int ret;

	ret = regmap_raw_write(cd->regmap, off, buf, count);

	return ret ? ret : count;
}

static const BIN_ATTR_ADMIN_RW(registers, 0);

static const struct bin_attribute *const focaltech_bin_attrs[] = {
	&bin_attr_registers,
	NULL,
};

static const struct attribute_group focaltech_attr_group = {
	.bin_attrs = focaltech_bin_attrs,
};

const struct attribute_group *focaltech_groups[] = {
	&focaltech_attr_group,
	NULL,
};
EXPORT_SYMBOL_GPL(focaltech_groups);

int focaltech_probe(struct device *dev, int irq, const struct input_id *id,
		    struct regmap *regmap,
		    const struct focaltech_ic_data *ic_data)
{
	struct focaltech_core *cd;
	int ret;

	dev_dbg(dev, "%s: line: %d\n", __func__, __LINE__);

	if (irq <= 0) {
		dev_err(dev, "Missing interrupt number\n");
		return -EINVAL;
	}

	cd = devm_kzalloc(dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->dev = dev;
	cd->regmap = regmap;
	cd->irq = irq;
	cd->ic_data = ic_data;

	/* Get reset GPIO */
	cd->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(cd->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(cd->reset_gpio),
				     "Failed to request reset gpio\n");

	/* Get regulators */
	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(focaltech_supplies),
					    focaltech_supplies,
					    &cd->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	/* Power ON */
	ret = focaltech_power_on(cd);
	if (ret)
		return dev_err_probe(dev, ret, "Failed power on\n");

	/* Get firmware path */
	ret = device_property_read_string(dev, "firmware-name", &cd->fw_path);
	if (ret)
		return dev_err_probe
			(dev, ret, "Failed to read firmware-name property\n");
	dev_dbg(dev, "%s: firmware path: %s\n", __func__, cd->fw_path);

	/* Register cleanup action; call it now if registration fails */
	ret = devm_add_action_or_reset(dev, focaltech_power_off_act, cd);
	if (ret)
		return ret;

	/* Setup input device */
	ret = focaltech_input_dev_config(cd, id);
	if (ret)
		return dev_err_probe
			(dev, ret, "Failed set input device\n");

	/* Get touchscreen type */
	if (!ic_data->is_incell) {
		dev_dbg(dev, "%s: line: %d On-Cell type\n", __func__, __LINE__);
		focaltech_request_handle_reset(cd, 200);
	} else
		dev_dbg(dev, "%s: line: %d In-Cell type\n", __func__, __LINE__);

	/* Get IC information */
	ret = focaltech_get_ic_information(cd, ic_data);
	if (ret)
		return dev_err_probe
			(dev, ret, "Failed to get IC information\n");

	/* Request IRQ */
	ret = devm_request_threaded_irq(dev, cd->irq, NULL, focaltech_irq,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "focaltech", cd);
	if (ret)
		return dev_err_probe
			(dev, ret, "Request threaded IRQ failed\n");

	/* Firmware upload */
	/*ret = focaltech_fwupload(cd);
	if (ret)
		return dev_err_probe
			(dev, ret, "Init firmware upload fail\n");*/

	dev_set_drvdata(dev, cd);

	dev_dbg(dev, "%s: line: %d\n", __func__, __LINE__);

	return 0;
}
EXPORT_SYMBOL_GPL(focaltech_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FocalTech Core Touchscreen driver");
MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
