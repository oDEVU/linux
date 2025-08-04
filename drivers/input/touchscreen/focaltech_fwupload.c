// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2012-2020, Focaltech Ltd. All rights reserved.
 * Copyright (C) 2025 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on fts_ts_flash driver
 */

#define DEBUG

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>

#include "focaltech.h"

#define INTERVAL_READ_REG	200 /* unit:ms */
#define TIMEOUT_READ_REG	1000 /* unit:ms */

#define BYTE_OFF_0(x)		(u8)((x) & 0xFF)
#define BYTE_OFF_8(x)		(u8)(((x) >> 8) & 0xFF)
#define BYTE_OFF_16(x)		(u8)(((x) >> 16) & 0xFF)
#define BYTE_OFF_24(x)		(u8)(((x) >> 24) & 0xFF)

#define FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN	7
#define FOCALTECH_UPLOAD_LOOP			30
#define FOCALTECH_DELAY_UPGRADE_RESET		80

#define FOCALTECH_CMD_RESET			0x07
#define FOCALTECH_ROMBOOT_CMD_WRITE		0xae
#define FOCALTECH_ROMBOOT_CMD_START_APP		0x08

#define FOCALTECH_REG_CHIP_ID			0xa3
#define FOCALTECH_REG_FW_VER			0xa6

#define FOCALTECH_MAX_LEN_FILE			(256 * 1024)
#define FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR	0xAD
#define FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR_LEN	4
#define FOCALTECH_PRAM_SADDR			0x000000
#define FOCALTECH_DRAM_SADDR			0xD00000
#define FOCALTECH_APP_INFO_OFFSET		0x100
#define FOCALTECH_MIN_LEN			0x120
#define FOCALTECH_READ_BOOTID_TIMEOUT		3
#define FOCALTECH_CMD_WRITE_LEN			6
#define FOCALTECH_FLASH_PACKET_LENGTH_SPI_LOW	(4 * 1024 - 4)
#define FOCALTECH_FLASH_PACKET_LENGTH_SPI	(32 * 1024 - 16)

#define FOCALTECH_ECC_FINISH_TIMEOUT		100
#define FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN	7
#define FOCALTECH_ROMBOOT_CMD_ECC		0xCC
#define FOCALTECH_ROMBOOT_CMD_ECC_FINISH	0xCE
#define FOCALTECH_ROMBOOT_CMD_ECC_FINISH_OK_A5	0xA5
#define FOCALTECH_ROMBOOT_CMD_ECC_FINISH_OK_00	0x00
#define FOCALTECH_ROMBOOT_CMD_ECC_READ		0xCD

#define AL2_FCS_COEF				((1 << 15) + (1 << 10) + (1 << 3))

static int focaltech_check_bootid(struct focaltech_core *cd)
{
	const struct focaltech_ic_ids *c = &cd->ic_data->ids;
	u8 id[2];
	int ret;

	ret = regmap_bulk_read(cd->regmap, FOCALTECH_CMD_READ_ID, id,
								sizeof(id));
	if (ret)
		return ret;

	dev_dbg(cd->dev, "Read BootID: 0x%02x%02x\n", id[0], id[1]);

	if ((c->rom_idh == id[0]) && (c->rom_idl == id[1]))
		return 0;

	return -EIO;
}

static int focaltech_enter_boot_mode(struct focaltech_core *cd)
{
	int ret;

	for (int i = 0; i < FOCALTECH_UPLOAD_LOOP; i++) {
		focaltech_request_handle_reset(cd, 0);
		mdelay(cd->ic_data->settings.delay_init + i);

		/* enter into boot & check boot id*/
		for (int j = 0; j < FOCALTECH_READ_BOOTID_TIMEOUT; j++) {
			ret = regmap_write(cd->regmap, FOCALTECH_CMD_START1, 1);
			if (ret)
				return ret;

			mdelay(cd->ic_data->settings.delay_init);

			ret = focaltech_check_bootid(cd);
			if (ret)
				return ret;

			break;
		}
	break;
	}

	dev_dbg(cd->dev, "BootID check pass\n");

	return ret;
}

static bool __maybe_unused focaltech_check_fast_download(struct focaltech_core *cd)
{
	u8 __maybe_unused cmd[6] = { 0xF2, 0x00, 0x78, 0x0A, 0x00, 0x02 };
	u32 value = 0; //u8
	u8 value2[2] = { 0 };
	int ret;

	ret = regmap_read(cd->regmap, 0xdb, &value);
	if (ret) {
		dev_err(cd->dev, "Read 0xdb failed, ret=%d", ret);
		goto read_err;
	}

	ret = regmap_bulk_read(cd->regmap, 6, value2, sizeof(value2));
	if (ret) {
		dev_err(cd->dev, "Read f2 failed, ret=%d", ret);
		goto read_err;
	}

	dev_dbg(cd->dev, "0xdb = 0x%x, 0xF2 = 0x%x", value, value2[0]);
	if ((value >= 0x18) && (value2[0] == 0x55)) {
		dev_info(cd->dev, "IC support fast-download");
		return true;
	}

read_err:
	dev_info(cd->dev, "IC not support fast-download");
	return false;
}


static int focaltech_ecc_cal_host(const u8 *data, u32 data_len, u16 *ecc_value)
{
	u16 ecc = 0;
	u32 i = 0;
	u32 j = 0;
	u16 al2_fcs_coef = AL2_FCS_COEF;

	for (i = 0; i < data_len; i += 2) {
		ecc ^= ((data[i] << 8) | (data[i + 1]));
		for (j = 0; j < 16; j++) {
			if (ecc & 0x01)
				ecc = (u16)((ecc >> 1) ^ al2_fcs_coef);
			else
				ecc >>= 1;
		}
	}

	*ecc_value = ecc & 0x0000FFFF;
	return 0;
}

static int focaltech_ecc_cal_tp
(struct focaltech_core *cd, u32 ecc_saddr, u32 ecc_len, u16 *ecc_value)
{
	int i = 0;
	u8 cmd[FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN] = { 0 };
	u32 value[2] = { 0 }; //u8
	int ret;

	cmd[0] = FOCALTECH_ROMBOOT_CMD_ECC;
	cmd[1] = BYTE_OFF_16(ecc_saddr);
	cmd[2] = BYTE_OFF_8(ecc_saddr);
	cmd[3] = BYTE_OFF_0(ecc_saddr);
	cmd[4] = BYTE_OFF_16(ecc_len);
	cmd[5] = BYTE_OFF_8(ecc_len);
	cmd[6] = BYTE_OFF_0(ecc_len);

	/* make boot to calculate ecc in pram */
	ret = regmap_write(cd->regmap, FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN, 1);
	if (ret) {
		dev_err(cd->dev, "ecc calc cmd fail, ret: %d\n", ret);
		return ret;
	}
	usleep_range(2000, 2100);

	/* wait boot calculate ecc finish */
	if (cd->ic_data->settings.ecc_delay) {
		mdelay(cd->ic_data->settings.ecc_delay);
	} else {
		for (i = 0; i < FOCALTECH_ECC_FINISH_TIMEOUT; i++) {
			ret = regmap_read(cd->regmap, FOCALTECH_ROMBOOT_CMD_ECC_FINISH, value);
			if (ret) {
				dev_err(cd->dev, "ecc finish cmd fail, ret:%d", ret);
				return ret;
			}
			if (cd->ic_data->settings.eccok_val == value[0])
				break;
			mdelay(1);
		}
		if (i >= FOCALTECH_ECC_FINISH_TIMEOUT) {
			dev_err(cd->dev,
				"wait ecc finish timeout,ecc_finish=%x",
				value[0]);
			return -EIO;
		}
	}

	/* get ecc value calculate in boot */

	ret = regmap_raw_read(cd->regmap, FOCALTECH_ROMBOOT_CMD_ECC_READ, value, 2);
	if (ret < 0) {
		dev_err(cd->dev, "ecc read cmd fail");
		return ret;
	}

	*ecc_value = ((u16)(value[0] << 8) + value[1]) & 0x0000FFFF;
	return 0;
}

static int focaltech_ecc_check
(struct focaltech_core *cd, const u8 *buf, u32 len, u32 ecc_saddr)
{
	int i = 0;
	u16 ecc_in_host = 0;
	u16 ecc_in_tp = 0;
	int packet_length = 0;
	int packet_number = 0;
	int packet_remainder = 0;
	int offset = 0;
	u32 packet_size = FOCALTECH_MAX_LEN_FILE;
	int ret = 0;

	if (cd->ic_data->settings.ecclen_max)
		packet_size = cd->ic_data->settings.ecclen_max;

	packet_number = len / packet_size;
	packet_remainder = len % packet_size;
	if (packet_remainder)
		packet_number++;
	packet_length = packet_size;

	for (i = 0; i < packet_number; i++) {
		/* last packet */
		if ((i == (packet_number - 1)) && packet_remainder)
			packet_length = packet_remainder;

		ret = focaltech_ecc_cal_host(buf + offset, packet_length,
				       &ecc_in_host);
		if (ret < 0) {
			dev_err(cd->dev, "ecc in host calc fail");
			return ret;
		}

		ret = focaltech_ecc_cal_tp(cd, ecc_saddr + offset, packet_length,
				     &ecc_in_tp);
		if (ret < 0) {
			dev_err(cd->dev, "ecc in tp calc fail");
			return ret;
		}

		dev_dbg(cd->dev, "ecc in tp:%04x,host:%04x,i:%d",
			ecc_in_tp, ecc_in_host, i);
		if (ecc_in_tp != ecc_in_host) {
			dev_err(cd->dev,
				"ecc_in_tp(%x) != ecc_in_host(%x), ecc check fail",
				ecc_in_tp, ecc_in_host);
			return -EIO;
		}

		offset += packet_length;
	}

	return 0;
}

static int focaltech_dpram_write
(struct focaltech_core *cd, u32 saddr, const u8 *buf, u32 len, bool wpram)
{
	int i = 0;
	int j = 0;
	u32 addr = 0;
	u32 baseaddr = wpram ? FOCALTECH_PRAM_SADDR : FOCALTECH_DRAM_SADDR;
	u32 offset = 0;
	u32 remainder = 0;
	u32 packet_number = 0;
	u32 packet_len = 0;
	u32 packet_size = FOCALTECH_FLASH_PACKET_LENGTH_SPI;
	int ret;

	/*if (!buf) {
		dev_err(cd->dev, "fw buf is null");
		return -EINVAL;
	}*/

	if ((len < FOCALTECH_MIN_LEN) || (len > cd->ic_data->settings.app2_offset)) {
		dev_err(cd->dev, "fw length(%d) fail", len);
		return -EINVAL;
	}

	u8 *cmd = vmalloc(packet_size + FOCALTECH_CMD_WRITE_LEN + 1);
	if (!cmd) {
		dev_err(cd->dev, "malloc memory for pram write buffer fail");
		return -ENOMEM;
	}
	memset(cmd, 0, packet_size + FOCALTECH_CMD_WRITE_LEN + 1);

	packet_number = len / packet_size;
	remainder = len % packet_size;
	if (remainder > 0)
		packet_number++;
	packet_len = packet_size;
	dev_info(cd->dev, "write data, num:%d remainder:%d",
		 packet_number, remainder);

	for (i = 0; i < packet_number; i++) {
		offset = i * packet_size;
		addr = saddr + offset + baseaddr;
		/* last packet */
		if ((i == (packet_number - 1)) && remainder)
			packet_len = remainder;

		/* set pram address */
		cmd[0] = FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR;
		cmd[1] = BYTE_OFF_16(addr);
		cmd[2] = BYTE_OFF_8(addr);
		cmd[3] = BYTE_OFF_0(addr);

		ret = regmap_raw_write(cd->regmap, 0, &cmd[0], FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR_LEN);
		if (ret) {
			dev_err(cd->dev,
				"set pram(%d) addr(%d) fail\n", i, addr);
			goto write_pram_err;
		}

		/* write pram data */
		cmd[0] = FOCALTECH_ROMBOOT_CMD_WRITE;
		for (j = 0; j < packet_len; j++) {
			cmd[1 + j] = buf[offset + j];
		}

		ret = regmap_raw_write(cd->regmap, 0, &cmd[0], 1 + packet_len);
		if (ret) {
			dev_err(cd->dev, "write fw to pram(%d) fail", i);
			goto write_pram_err;
		}
	}

write_pram_err:
	if (cmd) {
		vfree(cmd);
		cmd = NULL;
	}
	return ret;
}

static int focaltech_pram_write_ecc
(struct focaltech_core *cd, const u8 *data, size_t size)
{
	u32 pram_start_addr;
	int ret;

	/* get pram app length */
	u16 code_len = ((u16)data[FOCALTECH_APP_INFO_OFFSET + 0] << 8) +
		   data[FOCALTECH_APP_INFO_OFFSET + 1];
	u16 code_len_n = ((u16)data[FOCALTECH_APP_INFO_OFFSET + 2] << 8) +
		     data[FOCALTECH_APP_INFO_OFFSET + 3];
	if ((code_len + code_len_n) != 0xFFFF) {
		dev_err(cd->dev, "pram code len(%x %x) fail", code_len,
			code_len_n);
		return -EINVAL;
	}

	u32 pram_app_size =
		((u32)code_len) * cd->ic_data->settings.length_coefficient;
	dev_dbg(cd->dev, "pram app length in fact: %d", pram_app_size);

	/* write pram */
	if (cd->ic_data->settings.spi_pe) {
		dev_err(cd->dev, "SPI PE is unsupported yet :/\n");
		return -ENOSYS;
	} else
		ret = focaltech_dpram_write(cd, pram_start_addr, data,
							pram_app_size, true);
	if (ret) {
		dev_err(cd->dev, "write pram fail\n");
		return ret;
	}

	/* check ecc */
	ret = focaltech_ecc_check(cd, data, pram_app_size, pram_start_addr);
	if (ret) {
		dev_err(cd->dev, "pram ecc check fai\nl");
		return ret;
	}

	return ret;
}

static int focaltech_pram_start(struct focaltech_core *cd)
{
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	ret = regmap_write(cd->regmap, FOCALTECH_ROMBOOT_CMD_START_APP, 1);
	if (ret) {
		dev_err(cd->dev, "Write start PRAM cmd fail, ret: %d\n", ret);
		return ret;
	}

	usleep_range(10000, 11000);
	return 0;
}

static int focaltech_fw_write_start
(struct focaltech_core *cd, const u8 *data, size_t size, bool need_reset)
{
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	if (need_reset) {
		ret = focaltech_enter_boot_mode(cd);
		if (ret) {
			dev_err(cd->dev,
					"Failed to enter boot mode, %d\n", ret);
			return ret;
		}
	}

	ret = focaltech_pram_write_ecc(cd, data, size);
	if (ret) {
		dev_err(cd->dev, "Failed to write PRAM, %d\n", ret);
		return ret;
	}

	if (cd->ic_data->settings.drwr_support) {
		dev_err(cd->dev, "DRAM is unsupported yet :/\n");
		return -ENOSYS;
	}

	/* remap pram and run fw */
	ret = focaltech_pram_start(cd);
	if (ret) {
		dev_err(cd->dev, "PRAM start fail, %d\n", ret);
		return ret;
	}

	cd->fw_status->is_fw_running = true;
	dev_dbg(cd->dev, "fw download successfully\n");

	return ret;
}

static int focaltech_fw_download
(struct focaltech_core *cd, const u8 *data, size_t size, bool need_reset)
{
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	cd->fw_status->is_fw_loading = true;
	disable_irq(cd->irq);

	for (int i = 0; i < 4; i++) {
		if (i >= 4) {
			enable_irq(cd->irq);
			cd->fw_status->is_fw_loading = false;
			ret = -EIO;
			break;
		}

		ret = focaltech_fw_write_start(cd, data, size, need_reset);
		if (!ret)
			break;
	}

	return ret;
}

static int focaltech_fw_resume(struct focaltech_core *cd, bool need_reset) {
	const struct firmware *fw;
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	ret = request_firmware(&fw, cd->fw_path, cd->dev);
	if (ret)
		return ret;

	ret = focaltech_fw_download(cd, fw->data, fw->size, need_reset);
	if (ret)
		dev_err(cd->dev, "Failed to download firmware, %d\n", ret);

	if (!fw) {
		release_firmware(fw);
		fw = NULL;
	}

	return ret;
}

static int focaltech_check_cid(struct focaltech_core *cd, u8 id_h)
{
	//struct ft_chip_id_t *cid = &ts_data->ic_info.cid;
	u8 cid_h = 0x0;

	/*if (!cid->type)
		return -ENODATA;*/

	/*for (int i = 0; i < FTS_MAX_CHIP_IDS; i++) {
		cid_h = ((cid->chip_ids[i] >> 8) & 0x00FF);
		if (cid_h && (id_h == cid_h))
			return 0;
	}*/

	/* HACK */
	unsigned int cid[] = { 0x8A, 0x56, 0x62, 0x56, 0x62, 0x56, 0xE2, 0x00, 0x00 };
	for (int i = 0; i < 8; i++) {
		cid_h = ((cid[i] >> 8) & 0x00FF);
		if (cid_h && (id_h == cid_h))
			return 0;
	}

	return -ENODATA;
}

static int focaltech_wait_tp_to_valid(struct focaltech_core *cd)
{
	int cnt = 0;
	u32 idh = 0; //u8
	int ret;

	do {
		ret = regmap_read(cd->regmap, FOCALTECH_REG_CHIP_ID, &idh);
		if (ret < 0) {
			dev_err(cd->dev, "Failed to read ChipID\n");
			return ret;
		}

		if ((idh == cd->ic_data->ids.chip_idh) ||
		    (focaltech_check_cid(cd, idh) == 0)) {
			dev_info(cd->dev, "TP Ready, Device ID: 0x%02x\n", idh);
			return 0;
		} else
			dev_dbg(cd->dev,
				"TP Not Ready, ReadData: 0x%02x, ret: %d\n",
				idh, ret);

		cnt++;
		msleep(INTERVAL_READ_REG);
	} while ((cnt * INTERVAL_READ_REG) < TIMEOUT_READ_REG);

	return -EIO;
}

void focaltech_fw_recovery(struct focaltech_core *cd) {
	u8 chip_id[2];
	u32 chipid;
	u32 boot_state;
	int ret;

	if (cd->fw_status->is_fw_loading)
		return;

	ret = focaltech_check_bootid(cd);
	if (ret) {
		dev_err(cd->dev, "Failed to read BootID, ret=%d", ret);
		goto fw_is_running;
	}

	ret = regmap_read(cd->regmap, 0xd0, &boot_state);
	if (ret) {
		dev_err(cd->dev, "Failed to read boot state, ret=%d", ret);
		goto fw_is_running;
	}

	if (cd->ic_data->settings.upgsts_boot != boot_state) {
		dev_err(cd->dev, "Chip is not in the boot mode, ret=%d", ret);
		goto fw_is_running;
	}

	cd->fw_status->is_fw_running = false;

	ret = focaltech_fw_resume(cd, false);
	if (ret) {
		dev_err(cd->dev, "Failed to resume firmware, ret: %d\n", ret);
		return;
	}

	ret = regmap_read(cd->regmap, FOCALTECH_REG_CHIP_ID, &chipid);
	if (ret) {
		dev_err(cd->dev, "Failed to read ChipID, ret: %d\n", ret);
		return;
	}

	dev_dbg(cd->dev, "Read ChipID: 0x%02x", (u8)chip_id);

	focaltech_wait_tp_to_valid(cd);

fw_is_running:
	cd->fw_status->is_fw_running = true;
	return;
}

int focaltech_fwupload(struct focaltech_core *cd)
{
	int ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	if (cd->fw_status->is_fw_loading)
		return 0;

	cd->fw_status->is_fw_loading = true;

	ret = focaltech_fw_resume(cd, true);
	if (ret)
		return ret;


	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	return ret;
}

////////////////////////////////////////////////////////

/*
static int focaltech_get_fwver(struct focaltech_core *cd, int *ver)
{
	int ret = 0;

	if (!ver)
		return -EINVAL;

	ret = regmap_read(cd->regmap, FOCALTECH_REG_FW_VER, ver);
	if (ret < 0) {
		dev_err(cd->dev, "Read firmware version from tp fail\n");
		return ret;
	}

	return 0;
}

int focaltech_fw_reset(struct focaltech_core *cd)
{
	int ret = 0;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	ret = regmap_write(cd->regmap, FOCALTECH_CMD_RESET, 1);
	if (ret < 0) {
		dev_dbg(cd->dev, "pram/rom/bootloader reset cmd write fail");
		return ret;
	}

	msleep(FOCALTECH_DELAY_UPGRADE_RESET);

	return 0;
}

int focaltech_fwupload(struct focaltech_core *cd) {
	int ver = 0x00, ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	ret = focaltech_get_fwver(cd, &ver);
	if (ret)
		return ret;

	dev_dbg(cd->dev, "%s: line: %d: firmware ver: %d\n",
						__func__, __LINE__, ver);

	ret = focaltech_fw_reset(cd);
	if (ret)
		return ret;

	dev_dbg(cd->dev, "%s: line: %d\n", __func__, __LINE__);

	return ret;
}*/
