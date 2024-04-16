/*
 * Mac80211 driver for BES2600 device
 *
 * Copyright (c) 2022, Bestechnic
 * Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/crc32.h>
#include <linux/version.h>
#include "bes2600_factory.h"
#include "bes_chardev.h"
#include "bes_log.h"

#define LE_CPU_TRANS(val, cvt)		(val = cvt(val))

#define TRANS_LE16(val) LE_CPU_TRANS(val, __cpu_to_le16)
#define TRANS_LE32(val) LE_CPU_TRANS(val, __cpu_to_le32)

#define TRANS_CPU16(val) LE_CPU_TRANS(val, __le16_to_cpu)
#define TRANS_CPU32(val) LE_CPU_TRANS(val, __le32_to_cpu)

static DEFINE_MUTEX(factory_lock);

/*
 * It is only used for temporary storage.
 * Every time get the factory, it will read from the
 * file and overwrite the original value.
 */
static struct factory_t factory_cali_data;
static struct factory_t *factory_p = NULL;

void bes2600_factory_lock(void)
{
	mutex_lock(&factory_lock);
}

void bes2600_factory_unlock(void)
{
	mutex_unlock(&factory_lock);
}

u8* bes2600_factory_get_file_buffer(void)
{
	u8 *file_buffer = NULL;

	file_buffer = kmalloc(FACTORY_MAX_SIZE, GFP_KERNEL);
	if (!file_buffer) {
		return NULL;
	}

	return file_buffer;
}

void bes2600_factory_free_file_buffer(u8 *file_buffer)
{
	if (file_buffer)
		kfree(file_buffer);
}

static int bes2600_wifi_cali_table_save(u8 *file_buffer, struct factory_t *factory_save_p);

static inline uint32_t factory_crc32(const uint8_t *data, uint32_t len)
{
	u32 crc_le = 0;
	crc_le = crc32_le(0xffffffffL, (uint8_t *)data, len);
	crc_le ^= 0xffffffffL;
	return crc_le;
}

static int bes2600_factory_head_info_check(struct factory_t *factory_data)
{
	if (!factory_data) {
		bes_err("%s NULL pointer err\n", __func__);
		return -1;
	}

	if (factory_data->head.magic != NVREC_DEV_MAGIC) {
		return -EBADMSG;
	}

	if ((factory_data->head.version < NVREC_MINI_VERSION) ||
		(factory_data->head.version > NVREC_CURRENT_VERSION)) {
		bes_err("factory version error:%d", factory_data->head.version);
		return -EBADMSG;
	}

	return 0;
}

static int bes2600_factory_crc_check(struct factory_t *factory_data)
{
#ifdef FACTORY_CRC_CHECK
	u32 cal_crc = 0;
	u32 crc_len = sizeof(factory_data_t);
#ifndef STANDARD_FACTORY_EFUSE_FLAG
	crc_len = (crc_len - sizeof(u16) + 3) & (~0x3);
#endif

	if (!factory_data) {
		bes_err("%s NULL pointer err \n", __func__);
		return -1;
	}

	cal_crc = factory_crc32((uint8_t *)(&(factory_data->data)), crc_len);
	if (factory_data->head.crc != cal_crc) {
		bes_err(ES2600_DBG_CHARDEV,
			"bes2600 factory check failed, calc_crc:0x%08x factory_crc: 0x%08x\n",
			cal_crc, factory_data->head.crc);
		return -1;
	}
	return 0;

#else
	return 0;
#endif
}

/**
 * factory_section_read_file - Read data of specified length from file
 * @path:	path of the file
 * @buffer:	storage of read data
 *
 * The maximum file length allowed is 600 bytes.
 * This function does not do crc verification to take into 
 * account different storage requirements.
 * 
 * Return: length on success, negative error code otherwise.
 */
static int factory_section_read_file(char *path, void *buffer)
{
	int ret = 0;
	struct file *fp;

	if (!path || !buffer) {
		bes_err("%s NULL pointer err\n", __func__);
		return -1;
	}

	bes_devel("reading %s \n", path);

	fp = filp_open(path, O_RDONLY, 0); //S_IRUSR
	if (IS_ERR(fp)) {
		bes_devel("BES2600 : can't open %s\n",path);
		return -1;
	}

	if (fp->f_inode->i_size <= 0 || fp->f_inode->i_size > FACTORY_MAX_SIZE) {
		bes_err(			"bes2600_factory.txt size check failed, read_size: %lld max_size: %d\n",
			fp->f_inode->i_size, FACTORY_MAX_SIZE);
		filp_close(fp, NULL);
		return -1;
	}

	ret = kernel_read(fp, buffer, fp->f_inode->i_size, &fp->f_pos);

	filp_close(fp, NULL);

	if (ret != fp->f_inode->i_size) {
		bes_err("bes2600_factory.txt read fail\n");
		ret = -1;
	}

	return ret;
}

/**
 * factory_section_write_file - Write data of specified length to file
 * @path:	path of the file
 * @buffer:	storage of write data
 * @size:	length of data to write
 *
 * Return: length on success, negative error code otherwise.
 */
static int factory_section_write_file(char *path, void *buffer, int size)
{
	int ret = 0;
	struct file *fp;

	bes_devel("writing %s \n", path);

	fp = filp_open(path, O_TRUNC | O_CREAT | O_RDWR, S_IRUSR);
	if (IS_ERR(fp)) {
		bes_devel("BES2600 : can't open %s\n",path);
		return -1;
	}

	ret = kernel_write(fp, buffer, size, &fp->f_pos);

	filp_close(fp,NULL);

	return ret;
}

static inline int factory_parse(uint8_t *source_buf, struct factory_t *factory)
{
	int ret = 0;

	if (!source_buf || !factory) {
		bes_err("%s NULL pointer err\n", __func__);
		return -1;
	}

	ret = sscanf(source_buf, STANDARD_FACTORY,\
		&factory->head.magic,\
		&factory->head.version,\
		&factory->head.crc,\
		&factory->data.iQ_offset,\
		&factory->data.freq_cal,\
		&factory->data.freq_cal_flags,\
		&factory->data.tx_power_ch[0],\
		&factory->data.tx_power_ch[1],\
		&factory->data.tx_power_ch[2],\
		&factory->data.tx_power_type,\
		&factory->data.temperature,\
		&factory->data.tx_power_ch_5G[0],\
		&factory->data.tx_power_ch_5G[1],\
		&factory->data.tx_power_ch_5G[2],\
		&factory->data.tx_power_ch_5G[3],\
		&factory->data.tx_power_ch_5G[4],\
		&factory->data.tx_power_ch_5G[5],\
		&factory->data.tx_power_ch_5G[6],\
		&factory->data.tx_power_ch_5G[7],\
		&factory->data.tx_power_ch_5G[8],\
		&factory->data.tx_power_ch_5G[9],\
		&factory->data.tx_power_ch_5G[10],\
		&factory->data.tx_power_ch_5G[11],\
		&factory->data.tx_power_ch_5G[12],\
		&factory->data.tx_power_flags_5G,\
		&factory->data.temperature_5G,\
		&factory->data.bt_tx_power[0],\
		&factory->data.bt_tx_power[1],\
		&factory->data.bt_tx_power[2],\
		&factory->data.bt_tx_power[3]
#ifdef STANDARD_FACTORY_EFUSE_FLAG
		,&factory->data.select_efuse);
#else
		);
#endif

#ifndef STANDARD_FACTORY_EFUSE_FLAG
	factory->data.select_efuse = 0;
#endif

	if (ret != FACTORY_MEMBER_NUM)
	{
		bes_err("bes2600_factory.txt parse fail\n");
		return -1;
	}

	return ret;
}

static int factory_section_read_and_check_file(u8 *file_buf, char* path)
{
	int ret = 0;

	if (!file_buf || !path) {
		bes_err("%s NULL pointer err\n", __func__);
		return -1;
	}

	ret = factory_section_read_file(path, file_buf);
	if (ret < 0)
		return ret;

	memset(&factory_cali_data, 0, sizeof(struct factory_t));
	ret = factory_parse(file_buf, &factory_cali_data);
	if (ret < 0)
		return ret;

	ret = bes2600_factory_head_info_check(&factory_cali_data);
	if (ret < 0)
		return ret;

	ret = bes2600_factory_crc_check(&factory_cali_data);
	if (ret < 0)
		return ret;

	factory_p = &factory_cali_data;

	bes_devel("open wifi factory section success");

	return 0;
}

static void factory_section_wifi_tx_power_check(struct factory_t *factory_data)
{
	int i;
	bool inval_v = false;

	if (!factory_data)
		return ;

	/* only check cali channel, 11n ch1, ch7, ch13 */
	for (i = 0; i < ARRAY_SIZE(factory_data->data.tx_power_ch); ++i) {
		if (factory_data->data.tx_power_ch[i] == 0x0 ||
			factory_data->data.tx_power_ch[i] > 0x3fff) {
			inval_v = true;
		}
	}

	if (inval_v)
		bes_warn("tx_power_ch_2g cali, inval calibration value\n");
	print_hex_dump(KERN_DEBUG,
		"tx_power_ch_2g dump", DUMP_PREFIX_NONE, 16, 1,
		factory_data->data.tx_power_ch, sizeof(factory_data->data.tx_power_ch), false);

}


static void factory_section_wifi_tx_power_5G_check(struct factory_t *factory_data)
{
	int i;
	bool inval_v = false;

	if (!factory_data)
		return ;

	/* only check cali channel */
	for (i = 0; i < ARRAY_SIZE(factory_data->data.tx_power_ch_5G); ++i) {
		if (factory_data->data.tx_power_ch_5G[i] == 0x0 ||
			factory_data->data.tx_power_ch_5G[i] > 0x3fff) {
			inval_v = true;
		}
	}

	if (inval_v)
		bes_warn("tx_power_ch_5g cali, inval calibration value\n");
	print_hex_dump(KERN_DEBUG,
		"tx_power_ch_5g dump", DUMP_PREFIX_NONE, 16, 1,
		factory_data->data.tx_power_ch, sizeof(factory_data->data.tx_power_ch), false);

}

static void factory_section_wifi_freq_cali_check(struct factory_t *factory_data)
{
	if (!factory_data)
		return ;

	if (factory_data->data.freq_cal == 0x0 ||
		factory_data->data.freq_cal > 0x1ff) {
			bes_warn("freq cali, inval calibration value\n");
		}

	print_hex_dump(KERN_DEBUG,
		"wifi freq cali dump dump: ", DUMP_PREFIX_NONE, 16, 1,
		&factory_data->data.freq_cal, sizeof(factory_data->data.freq_cal), false);

}

static void factory_section_bt_tx_power_check(struct factory_t *factory_data)
{
	int i;
	bool inval_v = false;

	if (!factory_data)
		return ;

	/* bt only check bdr & edr power, (bdr/edr: div, powerlevel) */
	for (i = 0; i < ARRAY_SIZE(factory_data->data.bt_tx_power) - 1; i += 2) {
		if (factory_data->data.bt_tx_power[i] != 0x05) {
			inval_v = true;
		}

		if (factory_data->data.bt_tx_power[i + 1] == 0x0 ||
			factory_data->data.bt_tx_power[i + 1] > 0x20) {
			inval_v = true;
		}
	}

	if (inval_v)
		bes_warn("bt tx power cali, inval calibration value\n");
	print_hex_dump(KERN_DEBUG,
		"bt tx power cali dump: ", DUMP_PREFIX_NONE, 16, 1,
		factory_data->data.bt_tx_power, sizeof(factory_data->data.bt_tx_power), false);

}

void factory_little_endian_cvrt(u8 *data)
{
	int i;
	struct factory_t *trans_data = NULL;
	if (!data)
		return ;

	trans_data = (struct factory_t *)data;

	TRANS_LE16(trans_data->head.magic);
	TRANS_LE16(trans_data->head.version);
	TRANS_LE32(trans_data->head.crc);

	TRANS_LE16(trans_data->data.freq_cal);
	TRANS_LE32(trans_data->data.iQ_offset);

	for (i = 0; i < ARRAY_SIZE(trans_data->data.tx_power_ch); i++)
		TRANS_LE16(trans_data->data.tx_power_ch[i]);

	TRANS_LE16(trans_data->data.temperature);

	for (i = 0; i < ARRAY_SIZE(trans_data->data.bt_tx_power); i++)
		TRANS_LE32(trans_data->data.bt_tx_power[i]);

	for (i = 0; i < ARRAY_SIZE(trans_data->data.tx_power_ch_5G); i++)
		TRANS_LE16(trans_data->data.tx_power_ch_5G[i]);

	TRANS_LE16(trans_data->data.tx_power_flags_5G);
	TRANS_LE16(trans_data->data.temperature_5G);
	TRANS_LE16(trans_data->data.select_efuse);

}

void bes2600_factory_data_check(u8* data)
{
	struct factory_t *factory_data = NULL;

	if (!data)
		return ;

	factory_data = (struct factory_t *)data;

	factory_section_wifi_tx_power_check(factory_data);
	factory_section_wifi_tx_power_5G_check(factory_data);
	factory_section_bt_tx_power_check(factory_data);
	factory_section_wifi_freq_cali_check(factory_data);

	/* In order to support manual value change, recalculate crc before sending */
	factory_data->head.crc =
		factory_crc32((uint8_t *)(&(factory_data->data)), sizeof(factory_data_t));

}

/*
 * get factory data from file each time, and update factory_p.
 */
u8* bes2600_get_factory_cali_data(u8 *file_buffer, u32 *data_len, char *path)
{
	u8 *ret_p = NULL;

	if (!file_buffer || !path || !data_len) {
		bes_err("%s NULL pointer\n", __func__);
		return NULL;
	}

	/* reset factory_p */
	factory_p = NULL;

	*data_len = sizeof(struct factory_t);
	if (factory_section_read_and_check_file(file_buffer, path) < 0) {
		bes_err("read and check %s error\n", path);
		*data_len = 0;
		return NULL;
	}

	if (!factory_p) {
		*data_len = 0;
		return NULL;
	}

	ret_p = (u8 *)factory_p;

	return ret_p;
}

/**
 * When the calibration file does not exist, a new file is automatically created when
 * the calibration value is written. After writing, update factory_p, if the update is successful,
 * it means the writing is successful, otherwise it fails. At the same time, subsequent calibration
 * values are saved on this basis to avoid duplicating file creation and flushing out previously saved values.
 */
static bool bes2600_factory_file_status_read(u8 *file_buffer)
{
	u8 *factory_temp = NULL;
	uint32_t len;
	bool ret = true;

#ifdef FACTORY_SAVE_MULTI_PATH
	factory_temp = bes2600_get_factory_cali_data(file_buffer, &len, FACTORY_PATH);
	if (!factory_temp) {
		bes_warn("get factory cali from first path fali\n");
		factory_temp = bes2600_get_factory_cali_data(file_buffer, &len, FACTORY_DEFAULT_PATH);
		/* clear the flag of the file in the default path, and then create a new file */
		if (factory_temp) {
			((struct factory_t *)factory_temp)->data.tx_power_type = 0xff;
			((struct factory_t *)factory_temp)->data.freq_cal_flags = 0;
			((struct factory_t *)factory_temp)->data.tx_power_flags_5G = 0;
		}
	}
#else
	factory_temp = bes2600_get_factory_cali_data(file_buffer, &len, FACTORY_PATH);
#endif
	if (!factory_temp) {
		bes_warn("get factory data fali, check whether the file exists\n");
		ret = false;
	}

	return ret;
}

/* create a new factory.txt file, and set default value */
static int bes2600_factory_cali_file_hdr_fill(struct factory_t **factory_head)
{
	u16 tx_power_type = 0xff;
	int i;

	if (!factory_head)
		return -1;

	*factory_head = &factory_cali_data;
	memset(*factory_head, 0, sizeof(struct factory_t));
	(*factory_head)->data.tx_power_type = tx_power_type;
	(*factory_head)->head.magic = 0xba80;
	(*factory_head)->head.version = 2;

	(*factory_head)->data.freq_cal = 0xa0;

	for (i = 0; i < 3; ++i) {
		(*factory_head)->data.tx_power_ch[i] = 0x1400;
	}

	for (i = 0; i < 13; ++i) {
		(*factory_head)->data.tx_power_ch_5G[i] = 0x1400;
	}

	(*factory_head)->data.bt_tx_power[0] = 0x05;
	(*factory_head)->data.bt_tx_power[1] = 0x10;
	(*factory_head)->data.bt_tx_power[2] = 0x05;
	(*factory_head)->data.bt_tx_power[3] = 0x15;
#ifdef STANDARD_FACTORY_EFUSE_FLAG
	(*factory_head)->data.select_efuse = 0;
#endif

	return 0;
}


int16_t bes2600_wifi_power_cali_table_write(struct wifi_power_cali_save_t *data_cali)
{
	u16 mode, band, ch, power_cali, bandwidth;
	int power_index = 0;
	struct factory_t *factory_power_p = NULL;
	u8 *file_buffer = NULL;
	int16_t ret = 0;

	if (!data_cali) {
		bes_warn("%s: power cali save pointer is NULL\n", __func__);
		return -FACTORY_GET_INPUT_NULL_POINTER;
	}

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	/**
	 * When it returns true, it means that the factory file has been read.
	 * When it returns false, it means that the factory file does not exist,
	 * or the operation of reading the text file fails. At this time, a new factory file will be created.
	 */
	if (bes2600_factory_file_status_read(file_buffer)) {
		factory_power_p = factory_p;
	} else {
		if (bes2600_factory_cali_file_hdr_fill(&factory_power_p)) {
			bes_warn("%s, create bes2600_factory.txt fail.", __func__);
			ret = -FACTORY_FACTORY_TXT_CREATE_FAIL;
			goto err;
		}
	}

	mode = data_cali->mode;
	bandwidth = data_cali->bandwidth;
	band = data_cali->band;
	ch = data_cali->ch;
	power_cali = data_cali->power_cali;

	bes_devel("%s: mode = %u, bandwidth = %u,, band = %u, ch = %u, power_cali = 0x%04x\n",
				__func__, mode, bandwidth, band, ch, power_cali);

	/* only in 802.11n 20M msc7 mode, the power calibration value is saved */
	if (bandwidth != 0 || mode != WIFI_RF_11N_MODE) {
		ret = -FACTORY_SAVE_MODE_ERR;
		goto err;
	}

	/* powerlevel value range: 0 ~ 0x3fff */
	if (power_cali == 0 || power_cali > 0x3fff) {
		ret = -FACTORY_SAVE_POWER_ERR;
		goto err;
	}


	if (band == BAND_2G4) {
		switch (ch) {
		case 1:
			power_index = 0;
			break;
		case 7:
			power_index = 1;
			break;
		case 13:
			power_index = 2;
			break;
		default:
			ret = -FACTORY_SAVE_CH_ERR;
			goto err;
			break;
		}
		factory_power_p->data.tx_power_ch[power_index] = power_cali;
	} else if (band == BAND_5G) {
		switch (ch) {
		case 36:
		case 38:
		case 40:
			power_index = 0;
			break;
		case 44:
		case 46:
		case 48:
			power_index = 1;
			break;
		case 52:
		case 54:
		case 56:
			power_index = 2;
			break;
		case 60:
		case 62:
		case 64:
			power_index = 3;
			break;
		case 100:
		case 102:
		case 104:
			power_index = 4;
			break;
		case 108:
		case 110:
		case 112:
			power_index = 5;
			break;
		case 116:
		case 114:
		case 120:
			power_index = 6;
			break;
		case 124:
		case 126:
		case 128:
			power_index = 7;
			break;
		case 132:
		case 134:
		case 136:
			power_index = 8;
			break;
		case 140:
		case 142:
		case 144:
			power_index = 9;
			break;
		case 149:
		case 151:
		case 153:
			power_index = 10;
			break;
		case 157:
		case 159:
		case 161:
			power_index = 11;
			break;
		case 165:
		case 169:
			power_index = 12;
			break;
		default:
			ret = -FACTORY_SAVE_CH_ERR;
			goto err;
			break;
		}
		factory_power_p->data.tx_power_ch_5G[power_index] = power_cali;
	}

	/* save to file */
	if (bes2600_wifi_cali_table_save(file_buffer, factory_power_p)) {
		ret = -FACTORY_SAVE_WRITE_ERR;
		goto err;
	}

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;

}

int16_t bes2600_wifi_cali_freq_write(struct wifi_freq_cali_t *data_cali)
{
	u16 freq_cali;
	struct factory_t *factory_freq_p = NULL;
	u8 *file_buffer = NULL;
	int16_t ret = 0;

	if (!data_cali) {
		bes_warn("%s: freq cali save pointer is NULL\n", __func__);
		return -FACTORY_GET_INPUT_NULL_POINTER;
	}

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	/**
	 * When it returns true, it means that the factory file has been read.
	 * When it returns false, it means that the factory file does not exist,
	 * or the operation of reading the text file fails. At this time, a new factory file will be created.
	 */
	if (bes2600_factory_file_status_read(file_buffer)) {
		factory_freq_p = factory_p;
	} else {
		if (bes2600_factory_cali_file_hdr_fill(&factory_freq_p)) {
			bes_warn("%s, create bes2600_factory.txt fail.", __func__);
			ret = -FACTORY_FACTORY_TXT_CREATE_FAIL;
			goto err;
		}
	}

	freq_cali = data_cali->freq_cali;
	data_cali->cali_flag = 1;

	/* freqOffset value range: 0 ~ 0x1ff */
	if (freq_cali == 0 || freq_cali > 0x1ff) {
		ret = -FACTORY_SAVE_FREQ_ERR;
		goto err;
	}

	factory_freq_p->data.freq_cal = freq_cali;
	factory_freq_p->data.freq_cal_flags = (u8)(data_cali->cali_flag);
	bes_devel("%s: freq_cali = 0x%04x\n", __func__, data_cali->freq_cali);

	/* save to file */
	if (bes2600_wifi_cali_table_save(file_buffer, factory_freq_p)) {
		ret = -FACTORY_SAVE_WRITE_ERR;
		goto err;
	}

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;
}

#ifdef STANDARD_FACTORY_EFUSE_FLAG
int16_t bes2600_select_efuse_flag_write(uint16_t select_efuse_flag)
{
	struct factory_t *factory_flag_p = NULL;
	u8 *file_buffer = NULL;
	int16_t ret = 0;

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	/**
	 * When it returns true, it means that the factory file has been read.
	 * When it returns false, it means that the factory file does not exist,
	 * or the operation of reading the text file fails. At this time, a new factory file will be created.
	 */
	if (bes2600_factory_file_status_read(file_buffer)) {
		factory_flag_p = factory_p;
	} else {
		if (bes2600_factory_cali_file_hdr_fill(&factory_flag_p)) {
			bes_warn("%s, create bes2600_factory.txt fail.", __func__);
			ret = -FACTORY_FACTORY_TXT_CREATE_FAIL;
			goto err;
		}
	}

	factory_flag_p->data.select_efuse = select_efuse_flag;
	bes_devel("%s: select_efuse = %x\n", __func__, select_efuse_flag);

	/* save to file */
	if (bes2600_wifi_cali_table_save(file_buffer, factory_flag_p)) {
		ret = -FACTORY_SAVE_WRITE_ERR;
		goto err;
	}

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;
}
#endif

int16_t vendor_set_power_cali_flag(struct wifi_power_cali_flag_t *cali_flag)
{
	struct factory_t *factory_power_flag_p = NULL;
	u8 *file_buffer = NULL;
	u16 calied_flag_5g = 1;
	u16 calied_flag_2g = 0;
	int16_t ret = 0;

	if (!cali_flag) {
		bes_warn("%s: power cali flag save pointer is NULL\n", __func__);
		return -FACTORY_GET_INPUT_NULL_POINTER;
	}

	if (cali_flag->band != BAND_2G4 && cali_flag->band != BAND_5G) {
		bes_warn("%s: power cali flag save band err\n", __func__);
		return -FACTORY_SET_POWER_CALI_FLAG_ERR;
	}

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	if (bes2600_factory_file_status_read(file_buffer)) {
		factory_power_flag_p = factory_p;
	} else {
		bes_warn("%s: factory cali data is not exist\n", __func__);
		ret = -FACTORY_SAVE_FILE_NOT_EXIST;
		goto err;
	}

	if (cali_flag->band == BAND_2G4) {
		factory_power_flag_p->data.tx_power_type = calied_flag_2g;
	} else {
		factory_power_flag_p->data.tx_power_flags_5G = calied_flag_5g;
	}

	/* save to file */
	if (bes2600_wifi_cali_table_save(file_buffer, factory_power_flag_p)) {
		ret = -FACTORY_SET_POWER_CALI_FLAG_ERR;
		goto err;
	}

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;

}

static inline int factory_build(uint8_t *dest_buf, struct factory_t *factory)
{
	return snprintf(dest_buf, FACTORY_MAX_SIZE, STANDARD_FACTORY,\
		factory->head.magic,\
		factory->head.version,\
		factory->head.crc,\
		factory->data.iQ_offset,\
		factory->data.freq_cal,\
		factory->data.freq_cal_flags,\
		factory->data.tx_power_ch[0],\
		factory->data.tx_power_ch[1],\
		factory->data.tx_power_ch[2],\
		factory->data.tx_power_type,\
		factory->data.temperature,\
		factory->data.tx_power_ch_5G[0],\
		factory->data.tx_power_ch_5G[1],\
		factory->data.tx_power_ch_5G[2],\
		factory->data.tx_power_ch_5G[3],\
		factory->data.tx_power_ch_5G[4],\
		factory->data.tx_power_ch_5G[5],\
		factory->data.tx_power_ch_5G[6],\
		factory->data.tx_power_ch_5G[7],\
		factory->data.tx_power_ch_5G[8],\
		factory->data.tx_power_ch_5G[9],\
		factory->data.tx_power_ch_5G[10],\
		factory->data.tx_power_ch_5G[11],\
		factory->data.tx_power_ch_5G[12],\
		factory->data.tx_power_flags_5G,\
		factory->data.temperature_5G,\
		factory->data.bt_tx_power[0],\
		factory->data.bt_tx_power[1],\
		factory->data.bt_tx_power[2],\
		factory->data.bt_tx_power[3]
#ifdef STANDARD_FACTORY_EFUSE_FLAG
		,factory->data.select_efuse);
#else
		);
#endif
}

static int bes2600_wifi_cali_table_save(u8 *file_buffer, struct factory_t *factory_save_p)
{
	int ret = 0;
	int w_size;
	u32 crc_len = sizeof(factory_data_t);
#ifndef STANDARD_FACTORY_EFUSE_FLAG
	crc_len = (crc_len - sizeof(u16) + 3) & (~0x3);
#endif

	bes_devel("enter %s\n", __func__);

	if (!file_buffer) {
		return -ENOMEM;
	}

	if (!factory_save_p) {
		return -ENOENT;
	}

	/* All initialized to space */
	memset(file_buffer, 32, FACTORY_MAX_SIZE);
	file_buffer[FACTORY_MAX_SIZE - 1] = '\n';

	factory_save_p->head.crc =
		factory_crc32((uint8_t *)(&(factory_save_p->data)), crc_len);

	w_size = factory_build(file_buffer, factory_save_p);

	if (w_size < 0 || w_size > FACTORY_MAX_SIZE) {
		bes_err("%s: build failed! ret = %d.", __func__, ret);
		return -ETXTBSY;
	}

#ifdef FACTORY_SAVE_MULTI_PATH
	/* avoid trailing characters '\0' */
	file_buffer[w_size] = 32;
	ret = factory_section_write_file(FACTORY_PATH, file_buffer, FACTORY_MAX_SIZE);
#else
	ret = factory_section_write_file(FACTORY_PATH, file_buffer, w_size);
#endif
	if(ret < 0) {
		bes_err("%s: write failed! ret = %d.", __func__, ret);
		return ret;
	}

	return 0;
}

int16_t vendor_get_power_cali(struct wifi_get_power_cali_t *power_cali)
{
	u8 *file_buffer = NULL;
	int16_t ret = 0;

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	if (!power_cali) {
		ret = -FACTORY_GET_INPUT_NULL_POINTER;
		goto err;
	}

	if (!bes2600_factory_file_status_read(file_buffer)) {
		ret = -FACTORY_SAVE_FILE_NOT_EXIST;
		goto err;
	}

	memcpy(power_cali->tx_power_ch, factory_p->data.tx_power_ch, sizeof(power_cali->tx_power_ch));
	memcpy(power_cali->tx_power_ch_5G, factory_p->data.tx_power_ch_5G, sizeof(power_cali->tx_power_ch_5G));

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;
}


int16_t vendor_get_freq_cali(struct wifi_freq_cali_t *vendor_freq)
{
	u8 *file_buffer = NULL;
	int16_t ret = 0;

	if (!vendor_freq)
		return -FACTORY_GET_INPUT_NULL_POINTER;

	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -FACTORY_GET_INPUT_NULL_POINTER;

	bes2600_factory_lock();

	if (!bes2600_factory_file_status_read(file_buffer)) {
		ret = -FACTORY_SAVE_FILE_NOT_EXIST;
		goto err;
	}

	vendor_freq->status = 0;
	vendor_freq->freq_cali = factory_p->data.freq_cal;
	vendor_freq->cali_flag = factory_p->data.freq_cal_flags;

err:
	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
	return ret;
}

