// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2012-2020, Focaltech Ltd. All rights reserved.
 * Copyright (C) 2025 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on fts_ts and goodix_berlin_spi drivers
 */

#define DEBUG

#include <linux/unaligned.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/input.h>

#include "focaltech.h"

#define FOCALTECH_SPI_RETRY_NUM		3
#define FOCALTECH_CS_HIGH_DELAY		150 /* unit: us */

#define FOCALTECH_DATA_CRC_EN		0x20
#define FOCALTECH_WRITE_CMD		0x00
#define FOCALTECH_READ_CMD		(0x80 | FOCALTECH_DATA_CRC_EN)

#define FOCALTECH_SPI_HEADER_LEN	6
#define FOCALTECH_SPI_HEADER_LEN_V2	8
#define FOCALTECH_SPI_DUMMY_LEN		3
#define FOCALTECH_SPI_PREFIX_LEN	(FOCALTECH_SPI_HEADER_LEN +	\
					 FOCALTECH_SPI_DUMMY_LEN)
#define FOCALTECH_SPI_PREFIX_LEN_V2	(FOCALTECH_SPI_HEADER_LEN_V2 +	\
					 FOCALTECH_SPI_DUMMY_LEN)
#define FOCALTECH_MAX_POINTS_SUPPORT	10
#define FOCALTECH_ONE_TCH_LEN		6
#define FOCALTECH_ONE_TCH_LEN_V2	8
#define FOCALTECH_DATA_LEN		(FOCALTECH_MAX_POINTS_SUPPORT *	\
					FOCALTECH_ONE_TCH_LEN + 2)
#define FOCALTECH_DATA_LEN_V2		(FOCALTECH_MAX_POINTS_SUPPORT *	\
					FOCALTECH_ONE_TCH_LEN_V2 + 4)

#define FOCALTECH_FLAG_HID_BIT		10
#define FOCALTECH_FLAG_IDC_BIT		11

#define FOCALTECH_IS_INCELL(type)	(!!((type) & (1U << (FOCALTECH_FLAG_HID_BIT))))
#define FOCALTECH_HID_SUPPORTED(type)	(!!((type) & (1U << (FOCALTECH_FLAG_IDC_BIT))))

static int focaltech_spi_write(struct focaltech_core *cd,
			       unsigned char *data, unsigned int len)
{
	unsigned char header[4 + FOCALTECH_SPI_DUMMY_LEN];
	int ret;

	mutex_lock(cd->bus_lock);
	header[0] = data[0];
	header[1] = FOCALTECH_WRITE_CMD;
	header[2] = (len >> 8) & 0xff;
	header[3] = len & 0xff;
	memset (header[4], 0x00, FOCALTECH_SPI_DUMMY_LEN);

	struct spi_transfer xfers[2] = {
		{ .tx_buf = header, .len = sizeof(header) },
		{ .tx_buf = data + 1, .len = len - 1 },
	};

	ret = spi_sync_transfer(spi, xfers, ARRAY_SIZE(xfers));
	if (ret) {
		dev_err(cd->dev, "SPI transfer error, %d\n", ret);
		return ret;
	}

	if ((header[3] & 0xa0) != 0) {
		dev_err(cd->dev, "Command execution failed, %d\n", ret);
		ret = -EIO;
	}

	udelay(FOCALTECH_CS_HIGH_DELAY);
	mutex_unlock(cd->bus_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(focaltech_spi_write);

static int focaltech_spi_read(struct focaltech_core *cd,
			      unsigned char cmd,
			      unsigned char *data, unsigned int len)
{
	unsigned char header[4 + FOCALTECH_SPI_DUMMY_LEN];
	int ret;

	mutex_lock(cd->bus_lock);
	header[0] = cmd;
	header[1] = FOCALTECH_READ_CMD;
	header[2] = (length >> 8) & 0xff;
	header[3] = length & 0xff;
	memset (header[4], 0x00, FOCALTECH_SPI_DUMMY_LEN);

	struct spi_transfer xfers[2] = {
		{ .tx_buf = header, .len = sizeof(header) },
		{ .tx_buf = data, .len = len },
	};

	ret = spi_sync_transfer(cd->spi, xfers, ARRAY_SIZE(xfers));
	if (ret) {
		dev_err(cd->dev, "SPI transfer error, %d\n", ret);
		return ret;
	}

	if ((header[3] & 0xa0) != 0) {
		dev_err(cd->dev, "Command execution failed, %d\n", ret);
		ret = -EIO;
	}

	udelay(FOCALTECH_CS_HIGH_DELAY);
	mutex_unlock(cd->bus_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(focaltech_spi_read);

static const struct input_id focaltech_spi_input_id = {
	.bustype = BUS_SPI,
};

static int focaltech_spi_probe(struct spi_device *spi) {
	struct focaltech_core *cd;
	const struct focaltech_ic_data *ic_data =
						spi_get_device_match_data(spi);
	size_t max_size;
	int ret = 0;

	if (irq <= 0)
		return dev_err_probe
			(dev, -EINVAL, "Missing interrupt number\n");

	cd = devm_kzalloc(dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	/* SPI configure */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	max_size = spi_max_transfer_size(spi);

	cd->dev = &spi->dev;
	cd->irq = spi->irq;
	cd->ic_data = ic_data;

	/* Core probe */
	ret = focaltech_probe(cd, &focaltech_spi_input_id);
	if (ret)
		return ret;

	return 0;
}

static const struct focaltech_ic_data ft3680_data = {
	.is_incell	= FOCALTECH_IS_INCELL(0x3680008A),
	.hid_supported	= FOCALTECH_HID_SUPPORTED(0x3680008A),
	.ids = {
		.type		= 0x8a, /* TYPE */
		.chip_idh	= 0x56,	/* CHIP ID */
		.chip_idl	= 0x62,	/* CHIP ID */
		.rom_idh	= 0x56,	/* ROM ID */
		.rom_idl	= 0x62,	/* ROM ID */
		.pb_idh		= 0x56,	/* PRODUCTION BOARD ID */
		.pb_idl		= 0xe2,	/* PRODUCTION BOARD ID */
		.bl_idh		= 0x00,	/* BOOTLOADER ID */
		.bl_idl		= 0x00,	/* BOOTLOADER ID */
	},
	.settings = {
		.app2_offset	= (128 * 1024),
		.ecclen_max	= (128 * 1024),
		.eccok_val	= 0xa5,
		.upgsts_boot	= 0x01,
		.delay_init	= 8,
		.spi_pe		= 0,
		.length_coefficient	= 4,
		.fd_check	= 0,
		.drwr_support	= 0,
		.ecc_delay	= 5,
	},
	.spi_prefix_len = FOCALTECH_SPI_PREFIX_LEN,
	.data_len = FOCALTECH_DATA_LEN,
};

static const struct focaltech_ic_data ft3683g_data = {
	.is_incell	= FOCALTECH_IS_INCELL(0x56720090),
	.hid_supported	= FOCALTECH_HID_SUPPORTED(0x56720090),
	.ids = {
		.type		= 0x90, /* TYPE */
		.chip_idh	= 0x56,	/* CHIP ID */
		.chip_idl	= 0x72,	/* CHIP ID */
		.rom_idh	= 0x00,	/* ROM ID */
		.rom_idl	= 0x00,	/* ROM ID */
		.pb_idh		= 0x00,	/* PRODUCTION BOARD ID */
		.pb_idl		= 0x00,	/* PRODUCTION BOARD ID */
		.bl_idh		= 0x36,	/* BOOTLOADER ID */
		.bl_idl		= 0xb3,	/* BOOTLOADER ID */
	},
	.settings = { /* FIXME */
		.app2_offset	= (128 * 1024),
		.ecclen_max	= (128 * 1024),
		.eccok_val	= 0xa5,
		.upgsts_boot	= 0x01,
		.delay_init	= 8,
		.spi_pe		= 0,
		.length_coefficient	= 4,
		.fd_check	= 0,
		.drwr_support	= 0,
		.ecc_delay	= 5,
	},
	.spi_prefix_len = FOCALTECH_SPI_PREFIX_LEN_V2,
	.data_len = FOCALTECH_DATA_LEN_V2,
};

static const struct spi_device_id focaltech_spi_ids[] = {
	{ .name = "ft3680", .driver_data = (long)&ft3680_data },
	{ .name = "ft3683g", .driver_data = (long)&ft3683g_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(spi, focaltech_spi_ids);

static const struct of_device_id focaltech_spi_of_match[] = {
	{ .compatible = "focaltech,ft3680", .data = &ft3680_data },
	{ .compatible = "focaltech,ft3683g", .data = &ft3683g_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, focaltech_spi_of_match);

static struct spi_driver focaltech_spi_driver = {
	.driver = {
		.name = "focaltech-spi",
		.of_match_table = focaltech_spi_of_match,
		.pm = pm_sleep_ptr(&focaltech_pm_ops),
		.dev_groups = focaltech_groups,
	},
	.probe = focaltech_spi_probe,
	.id_table = focaltech_spi_ids,
};
module_spi_driver(focaltech_spi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FocalTech SPI Touchscreen driver");
MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
