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
#include <linux/regmap.h>
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

static int focaltech_spi_xfer(struct spi_device *spi,
			      u8 *tx_buf, u8 *rx_buf, u32 len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx_buf,
		.rx_buf = rx_buf,
		.len = len,
	};

	return spi_sync_transfer(spi, &xfer, 1);
}

static void crckermit(u8 *data, u32 len, u16 *crc_out)
{
	u32 i = 0, j = 0;
	u16 crc = 0xFFFF;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			if (crc & 0x01)
				crc = (crc >> 1) ^ 0x8408;
			else
				crc = (crc >> 1);
		}
	}

	*crc_out = crc;
}

static int rdata_check(u8 *rdata, u32 rlen)
{
	u16 crc_calc = 0;
	u16 crc_read = 0;

	crckermit(rdata, rlen - 2, &crc_calc);
	crc_read = (u16)(rdata[rlen - 1] << 8) + rdata[rlen - 2];
	if (crc_calc != crc_read)
		return -EIO;

	return 0;
}

static int focaltech_spi_write(void *context,
			       const void *data, size_t count)
{
	struct spi_device *spi = context;
	const struct focaltech_ic_data *ic_data =
						spi_get_device_match_data(spi);
	const u8 *writebuf = data;
	u32 len = 0;
	u32 datalen = count - 1;
	int ret;

	dev_dbg(&spi->dev, "%s: line: %d\n", __func__, __LINE__);

	u8 *buf __free(kfree) =
		kzalloc(count + ic_data->spi_prefix_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[len++] = writebuf[0];
	buf[len++] = FOCALTECH_WRITE_CMD;
	buf[len++] = (datalen >> 8) & 0xFF;
	buf[len++] = datalen & 0xFF;
	if (datalen > 0) {
		len += FOCALTECH_SPI_DUMMY_LEN;
		memcpy(&buf[len], &writebuf[1], datalen);
		len += datalen;
	}

	for (int i = 0; i < FOCALTECH_SPI_RETRY_NUM; i++) {
		ret = focaltech_spi_xfer(spi, buf, buf, len);
		if (ret) {
			dev_err(&spi->dev, "SPI transfer error, %d\n", ret);
			udelay(FOCALTECH_CS_HIGH_DELAY);
			continue;
		}

		if (buf[3] & 0xa0) {
			ret = -EIO;
			udelay(FOCALTECH_CS_HIGH_DELAY);
		} else
			break;
	}

	return ret;
}

static int focaltech_spi_read(void *context,
			      const void *reg_buf, size_t reg_size,
		    	      void *val_buf, size_t val_size)
{
	struct spi_device *spi = context;
	const struct focaltech_ic_data *ic_data =
						spi_get_device_match_data(spi);
	const u8 *cmd = reg_buf;
	u32 dp, len = 0;
	int i = 0;
	int ret;

	dev_dbg(&spi->dev, "%s: line: %d\n", __func__, __LINE__);

	u8 *buf __free(kfree) =
		kzalloc(val_size + ic_data->spi_prefix_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[len++] = cmd[0];
	buf[len++] = FOCALTECH_READ_CMD;
	buf[len++] = (val_size >> 8) & 0xFF;
	buf[len++] = val_size & 0xFF;
	dp = len + FOCALTECH_SPI_DUMMY_LEN;
	len = dp + val_size;
	if (FOCALTECH_READ_CMD & FOCALTECH_DATA_CRC_EN)
		len += 2;

	for (i = 0; i < FOCALTECH_SPI_RETRY_NUM; i++) {
		ret = focaltech_spi_xfer(spi, buf, buf, len);
		if (ret) {
			dev_err(&spi->dev, "SPI transfer error, %d\n", ret);
			udelay(FOCALTECH_CS_HIGH_DELAY);
			continue;
		}

		if (!(buf[3] & 0xA0)) {
			memcpy((u8 *)val_buf, &buf[dp], val_size);
			/* crc check */
			if (FOCALTECH_READ_CMD & FOCALTECH_DATA_CRC_EN) {
				ret = rdata_check(&buf[dp], len - dp);
				if (ret < 0) {
					dev_dbg(&spi->dev,
						"data read(addr:%x) crc abnormal,retry:%d",
						cmd[0], i);
					udelay(FOCALTECH_CS_HIGH_DELAY);
					continue;
				}
			}
			break;
		} else {
			ret = -EIO;
			udelay(FOCALTECH_CS_HIGH_DELAY);
		}
	}

	return ret;
}

static const struct regmap_config focaltech_spi_regmap_conf = {
	.reg_bits = 8,
	.val_bits = 8,
	.read = focaltech_spi_read,
	.write = focaltech_spi_write,
};

static const struct input_id focaltech_spi_input_id = {
	.bustype = BUS_SPI,
};

static int focaltech_spi_probe(struct spi_device *spi) {
	const struct focaltech_ic_data *ic_data =
						spi_get_device_match_data(spi);
	struct regmap_config regmap_config;
	struct regmap *regmap;
	size_t max_size;
	int ret = 0;

	dev_dbg(&spi->dev, "%s: line: %d\n", __func__, __LINE__);

	/* SPI configure */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	max_size = spi_max_transfer_size(spi);
	dev_dbg(&spi->dev, "%s: max_size is %lu\n", __func__, max_size);

	regmap_config = focaltech_spi_regmap_conf;
	regmap_config.max_raw_read = max_size - ic_data->spi_prefix_len;
	regmap_config.max_raw_write = max_size - ic_data->spi_prefix_len;

	/* Regmap init */
	regmap = devm_regmap_init(&spi->dev, NULL, spi, &regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Core probe */
	ret = focaltech_probe
		(&spi->dev, spi->irq, &focaltech_spi_input_id, regmap, ic_data);
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
