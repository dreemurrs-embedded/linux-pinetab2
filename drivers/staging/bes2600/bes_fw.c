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
#include "bes_fw_common.h"
#include "bes2600.h"
#include "hwio.h"
#include "sbus.h"
#include "bes_chardev.h"
#include <linux/string.h>
#include "bes2600_factory.h"
#include "bes_log.h"

// fw blob names
#define BES2600_LOAD_BOOT_NAME      "bes2600/best2002_fw_boot_sdio.bin"
#define BES2600_LOAD_FW_NAME        "bes2600/best2002_fw_sdio.bin"
#define BES2600_LOAD_NOSIGNAL_FW_NAME   "bes2600/best2002_fw_sdio_nosignal.bin"
#define BES2600_LOAD_BTRF_FW_NAME   "bes2600/best2002_fw_sdio_btrf.bin"

int bes2600_load_firmware_sdio(struct sbus_ops *ops, struct sbus_priv *priv);

struct platform_fw_t {
	struct delayed_work work_data;
	struct sdio_func *func;
	struct completion completion_data;
	const struct sbus_ops *sbus_ops;
	struct sbus_priv *sbus_priv;
};

static void bes_fw_irq_handler(void *priv)
{
	struct platform_fw_t *fw_data = (struct platform_fw_t *)priv;
	bes_devel("%s\n", __func__);
	complete(&fw_data->completion_data);
}

//#define BES_SLAVE_RX_DOUBLE_CHECK
static int bes_slave_rx_ready(struct platform_fw_t *fw_data, u8* buf_cnt,
					u16* buf_len, int timeout)
{
	int ret;
	unsigned long start = jiffies;

	do {
		ret = bes2600_reg_read(0x108, buf_cnt, 1);
		if (!(ret || buf_cnt)) {
			mdelay(50);
			continue;
		} else if (ret) {
			bes_err("%s,%d err=%d\n", __func__, __LINE__, ret);
		} else {
			ret = bes2600_reg_read_16(0x109, buf_len);
		}
		break;
	} while(time_before(jiffies, start + timeout));

	return ret;
}

//#define BES_SLAVE_TX_DOUBLE_CHECK
//#define MISSED_INTERRUPT_WORKAROUND
static int bes_slave_tx_ready(struct platform_fw_t *fw_data, u16 *tx_len, int timeout)
{
	int ret, retry = 0;

	bes_devel("%s now=%lu\n", __func__, jiffies);

	msleep(2);

	ret = wait_for_completion_interruptible_timeout(&fw_data->completion_data, timeout);
	if (ret > 0) {
#ifdef MISSED_INTERRUPT_WORKAROUND
test_read_tx:
#endif
		do {
			ret = bes2600_reg_read_16(0, tx_len);
			if (!ret && (*tx_len))
				break;
			else
				bes_err("%s,%d ret=%d tx_len=%x retry=%d\n",
						__func__, __LINE__, ret, *tx_len, retry);
			retry++;
		} while(retry <= 5);
		reinit_completion(&fw_data->completion_data);

	} else if(!ret) {
		bes_err("%s now=%lu delta=%d\n", __func__, jiffies, timeout);
#ifndef MISSED_INTERRUPT_WORKAROUND
		ret = -110;
#else
		goto test_read_tx;
#endif
	} else {
		// ret = -ERESTARTSYS, to be continued;
	}

	return ret;
}

// UNUSED
/*
int bes_host_slave_sync(struct bes2600_common *hw_priv)
{
	u8 val;
	int ret;

	ret = bes2600_reg_read(BES_HOST_INT_REG_ID, &val, 1);
	if (ret) {
		bes_err("%s,%d err=%d\n", __func__, __LINE__, ret);
		return ret;
	}

	val |= BES_HOST_INT;
	ret = bes2600_reg_write(BES_HOST_INT_REG_ID, &val, 1);
	if (ret) {
		bes_err("%s,%d err=%d\n", __func__, __LINE__, ret);
	}
	return ret;
}
*/

//#define DATA_DUMP_OBSERVE

static int bes_firmware_download_write_reg(struct platform_fw_t *fw_data, u32 addr, u32 val)
{
	u8 frame_num = 0;
	u8 buf_cnt = 0;
	u16 tx_size = 0;
	u16 rx_size = 0;
	u32 length = 0;
	u8 *short_buf;
	int ret;

	struct fw_msg_hdr_t header;
	struct fw_info_t fw_info;
	struct download_fw_t download_addr;

	fw_info.addr = addr;
	fw_info.len = 4;

	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave rx buf cnt:%d,buf len max:%d\n", buf_cnt, tx_size);
	} else {
		bes_err("wait bes sdio slave rx ready tiemout:%d\n", ret);
		return ret;
	}

	short_buf = kzalloc(512, GFP_KERNEL);
	if (!short_buf)
		return -ENOMEM;

	header.type = FRAME_HEADER_DOWNLOAD_INFO;
	header.seq = frame_num;
	header.len = sizeof(struct fw_info_t);
	frame_num++;
	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&fw_info, sizeof(struct fw_info_t));
	length = BES_FW_MSG_TOTAL_LEN(header);
	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx download firmware info err:%d\n", ret);
		goto err;
	}

	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave tx ready %d bytes\n", rx_size);
	} else {
		bes_err("wait slave process failed:%d\n", ret);
		goto err;
	}

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("rx download firmware info rsp err:%d\n", ret);
		goto err;
	}

	header.type = FRAME_HEADER_DOWNLOAD_DATA;
	header.seq = frame_num;
	header.len = 8;
	frame_num++;

	download_addr.addr = fw_info.addr;

	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), &download_addr.addr, sizeof(struct download_fw_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t), &val, 4);
	length = BES_FW_MSG_TOTAL_LEN(header);

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx download fw data err:%d\n", ret);
		goto err;
	}
	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("bes_slave ready tx %d bytes\n", rx_size);
	} else {
		bes_err("wait slave process download fw data err:%d\n", ret);
		goto err;
	}

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("rx tx download fw data rsp err:%d\n", ret);
		goto err;
	}

err:
	kfree(short_buf);
	return ret;
}

static int bes_firmware_download_write_mem(struct platform_fw_t *fw_data, const u32 addr, const u8 *data, const  u32 len)
{
	u8 frame_num = 0;
	u8 last_frame_num = 0;
	u8 buf_cnt = 0;

	u16 tx_size = 0;
	u16 rx_size = 0;

	u32 length = 0;
	u32 code_length = len;
	u32 retry_cnt = 0;
	int ret;

	const u8 *data_p;
	u8 *short_buf, *long_buf;

	struct fw_msg_hdr_t header;
	struct fw_info_t fw_info;
	struct download_fw_t download_addr;
	struct fw_crc_t crc32_t;

retry:
	fw_info.addr = addr;
	fw_info.len = len;
	data_p = data;

	crc32_t.crc32 = 0;
	crc32_t.crc32 ^= 0xffffffffL;
	crc32_t.crc32 = crc32_le(crc32_t.crc32, (u8 *)data, len);
	crc32_t.crc32 ^= 0xffffffffL;

	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave rx buf cnt:%d,buf len max:%d\n", buf_cnt, tx_size);
	} else {
		bes_devel("wait bes sdio slave rx ready tiemout:%d\n", ret);
		return ret;
	}

	header.type = FRAME_HEADER_DOWNLOAD_INFO;
	header.seq = frame_num;
	header.len = sizeof(struct fw_info_t);
	last_frame_num = frame_num;
	frame_num++;

	short_buf = kzalloc(512, GFP_KERNEL);
	if (!short_buf)
		return -ENOMEM;
	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&fw_info, sizeof(struct fw_info_t));
	length = BES_FW_MSG_TOTAL_LEN(header);

	if (tx_size > length) {
		bes_devel("%s", "tx download firmware info\n");
	} else {
		bes_devel("%s:%d bes slave has no enough buffer%d/%d\n", __func__, __LINE__, tx_size, length);
		goto err1;
	}

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx download firmware info err:%d\n", ret);
		goto err1;
	}

	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave tx ready %d bytes\n", rx_size);
	} else {
		bes_devel("wait slave process failed:%d\n", ret);
		goto err1;
	}

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("rx download firmware info rsp err:%d\n", ret);
		goto err1;
	}

	//check device rx status
	ret = bes_frame_rsp_check(short_buf, last_frame_num);
	if (ret) {
		bes_err("rsp download firmware info err:%d\n", ret);
		goto err1;
	}

	//download firmware
	long_buf = kmalloc(1024 * 32, GFP_KERNEL);
	if (!long_buf) {
		bes_err("%s:%d fw failed to allocate memory\n",__func__, __LINE__);
		ret = -ENOMEM;
		goto err1;
	}
	download_addr.addr = fw_info.addr;

	while (code_length) {

		ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
		if (ret) {
			goto err2;
		} else {
			bes_devel("bes salve rx ready %d bytes\n", tx_size);
		}


		if ((tx_size < 4) || (tx_size % 4)) {
			bes_err("%s:%d tx size=%d\n", __func__, __LINE__, tx_size);
			ret = -203;
			goto err2;
		}

		if ((code_length + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t)) < tx_size) {
			length = code_length + sizeof(struct download_fw_t);
		} else {
			length = tx_size - sizeof(struct fw_msg_hdr_t);
		}

		header.type = FRAME_HEADER_DOWNLOAD_DATA;
		header.seq = frame_num;
		header.len = length;
		last_frame_num = frame_num;
		frame_num++;

		memcpy(long_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
		memcpy(long_buf + sizeof(struct fw_msg_hdr_t), &download_addr.addr, sizeof(struct download_fw_t));
		length -= sizeof(struct download_fw_t);//real data length
		memcpy(long_buf + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t), data_p, length);

		length += (sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t));

		bes_devel("tx_download_firmware_data:%x %d\n", download_addr.addr, length);

		ret = bes2600_data_write(long_buf, length > 512 ? length : 512);
		if (ret) {
			bes_err("tx download fw data err:%d\n", ret);
			goto err2;
		}
		length -= (sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t));

		ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
		if (!ret) {
			bes_devel("bes_slave ready tx %d bytes\n", rx_size);
		} else {
			bes_err("wait slave process download fw data err:%d\n", ret);
			goto err2;
		}

		ret = bes2600_data_read(short_buf, rx_size);
		if (ret) {
			bes_err("rx tx download fw data rsp err:%d\n", ret);
			goto err2;
		}

		//check device rx status
		ret = bes_frame_rsp_check(short_buf, last_frame_num);
		if (ret) {
			bes_err("rsp tx download fw err:%d\n", ret);
			goto err2;
		}

		code_length -= length;
		data_p += length;
		download_addr.addr += length;
		bes_devel("already tx fw size:%x/%x\n", download_addr.addr - fw_info.addr, fw_info.len);
	}

	//Notify Device:The firmware download is complete

	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (ret) {
		goto err2;
	} else {
		bes_devel("bes salve rx ready %d bytes\n", tx_size);
	}

	header.type = FRAME_HEADER_DOWNLOAD_END;
	header.seq = frame_num;
	header.len = sizeof(struct fw_crc_t);
	last_frame_num = frame_num;
	frame_num++;

	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&crc32_t.crc32, sizeof(struct fw_crc_t));
	length = BES_FW_MSG_TOTAL_LEN(header);

	bes_devel("%s", "tx download firmware complete command\n");

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx downlod firmware complete command err:%d\n", ret);
		goto err2;
	}

	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("bes_slave ready tx %d bytes\n", rx_size);
	} else {
		bes_err("wait slave process download fw data err:%d\n", ret);
		goto err2;
	}

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("receive download firmware complete cmd rsp err:%d\n", ret);
		goto err2;
	}

	//check device rx status
	ret = bes_frame_rsp_check(short_buf, last_frame_num);
	if (ret) {
		bes_err("rsp download firmware complete err:%d\n", ret);
		goto err2;
	}
err2:
	kfree(long_buf);
err1:
	kfree(short_buf);

	if (ret && retry_cnt < 3) {
		retry_cnt++;
		goto retry;
	}
	return ret;
}

static int bes_firmware_download(struct platform_fw_t *fw_data, const char *fw_name, bool auto_run)
{
	u8 frame_num = 0;
	u8 last_frame_num = 0;
	u8 buf_cnt = 0;

	u16 tx_size = 0;
	u16 rx_size = 0;

	u32 length = 0;
	u32 code_length = 0;
	u32 retry_cnt = 0;
	int ret;
	const u8 *fw_ver_ptr;
	const u8 *data_p;
	u8 *short_buf, *long_buf;

const struct firmware *fw_bin;

#ifdef DATA_DUMP_OBSERVE
	char *observe;
	size_t observe_len;
	loff_t observe_off = 0;
	mm_segment_t old_fs;
	struct file *observe_file = NULL;
#endif

	struct fw_msg_hdr_t header;
	struct fw_info_t fw_info;
	struct download_fw_t download_addr;
	struct fw_crc_t crc32_t;
	struct run_fw_t run_addr;

retry:
	ret = request_firmware(&fw_bin, fw_name, NULL);
	if (ret) {
		bes_err("request firmware err:%d\n", ret);
		return ret;
	}

	bes_parse_fw_info(fw_bin->data, fw_bin->size, &fw_info.addr, &crc32_t.crc32);

	fw_ver_ptr = bes2600_get_firmware_version_info(fw_bin->data, fw_bin->size);
	if(fw_ver_ptr == NULL)
		bes_err("------Firmware version get failed\n");
	else
		bes_devel("------Firmware: %s version :%s\n", fw_name ,fw_ver_ptr);

	bes_devel("------load addr  :0x%08X\n", fw_info.addr);
	bes_devel("------data crc   :0x%08X\n", crc32_t.crc32);

	code_length = fw_bin->size - CODE_DATA_USELESS_SIZE;
	bes_devel("------code size  :%d\n", code_length);

	fw_info.len = code_length;
	data_p = fw_bin->data;

	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave rx buf cnt:%d,buf len max:%d\n", buf_cnt, tx_size);
	} else {
		bes_devel("wait bes sdio slave rx ready tiemout:%d\n", ret);
		return ret;
	}

	header.type = FRAME_HEADER_DOWNLOAD_INFO;
	header.seq = frame_num;
	header.len = sizeof(struct fw_info_t);
	last_frame_num = frame_num;
	frame_num++;

	short_buf = kzalloc(512, GFP_KERNEL);
	if (!short_buf)
		return -ENOMEM;
	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&fw_info, sizeof(struct fw_info_t));
	length = BES_FW_MSG_TOTAL_LEN(header);

	//mdelay(5000);
	print_hex_dump(KERN_DEBUG, "FW info: ", DUMP_PREFIX_NONE, 16, 1, short_buf, length, false);

	if (tx_size > length) {
		bes_devel("%s", "tx download firmware info\n");
	} else {
		bes_devel("%s:%d bes slave has no enough buffer%d/%d\n", __func__, __LINE__, tx_size, length);
		goto err1;
	}

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx download firmware info err:%d\n", ret);
		goto err1;
	}

#if 1
	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("sdio slave tx ready %d bytes\n", rx_size);
	} else {
		bes_devel("wait slave process failed:%d\n", ret);
		goto err1;
	}
#ifdef BES_SLAVE_TX_DOUBLE_CHECK
	if (rx_size != 8)
		rx_size = 8;
#endif
#else
	mdelay(100);
	rx_size = 8;
#endif

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("rx download firmware info rsp err:%d\n", ret);
		goto err1;
	}

	//check device rx status
	ret = bes_frame_rsp_check(short_buf, last_frame_num);
	if (ret) {
		bes_err("rsp download firmware info err:%d\n", ret);
		goto err1;
	}

	//download firmware
	long_buf = kmalloc(1024 * 32, GFP_KERNEL);
	if (!long_buf) {
		bes_err("%s:%d fw failed to allocate memory\n",__func__, __LINE__);
		ret = -ENOMEM;
		goto err1;
	}
	download_addr.addr = fw_info.addr;

#ifdef DATA_DUMP_OBSERVE
	observe_file = filp_open("/lib/firmware/bes2002_fw_write.bin", O_CREAT | O_RDWR, 0);
	if (IS_ERR(observe_file)) {
		bes_err("create data_dump file err:%ld\n", IS_ERR(observe_file));
		observe_file = NULL;
	}
#endif

	while (code_length) {

#if 1
		ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
		if (ret) {
			goto err2;
		} else {
			bes_devel("bes salve rx ready %d bytes\n", tx_size);
		}
#endif
#ifdef BES_SLAVE_RX_DOUBLE_CHECK
		tx_size = 512;
#endif

		if ((tx_size < 4) || (tx_size % 4)) {
			bes_err("%s:%d tx size=%d\n", __func__, __LINE__, tx_size);
			ret = -203;
			goto err2;
		}

		if ((code_length + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t)) < tx_size) {
			length = code_length + sizeof(struct download_fw_t);
		} else {
			length = tx_size - sizeof(struct fw_msg_hdr_t);
		}

#if 0 // for SDIO_USE_V2
		if (length + sizeof(struct fw_msg_hdr_t) > func->cur_blksize) {
			length = (length + sizeof(struct fw_msg_hdr_t)) / func->cur_blksize * func->cur_blksize;
			length -= sizeof(struct fw_msg_hdr_t);
		}
#endif

		header.type = FRAME_HEADER_DOWNLOAD_DATA;
		header.seq = frame_num;
		header.len = length;
		last_frame_num = frame_num;
		frame_num++;

		memcpy(long_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
		memcpy(long_buf + sizeof(struct fw_msg_hdr_t), &download_addr.addr, sizeof(struct download_fw_t));
		length -= sizeof(struct download_fw_t);//real data length
		memcpy(long_buf + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t), data_p, length);

		length += (sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t));

		//mdelay(5000);
		bes_devel("tx_download_firmware_data:%x %d\n", download_addr.addr, length);

#ifdef DATA_DUMP_OBSERVE
		if (observe_file) {
			observe = (char *)(long_buf + sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t));
			observe_len = length - sizeof(struct fw_msg_hdr_t) - sizeof(struct download_fw_t);
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			vfs_write(observe_file, observe, observe_len, &observe_off);
			set_fs(old_fs);
		}
#endif

		ret = bes2600_data_write(long_buf, length > 512 ? length : 512);
		if (ret) {
			bes_err("tx download fw data err:%d\n", ret);
			goto err2;
		}
		length -= (sizeof(struct fw_msg_hdr_t) + sizeof(struct download_fw_t));

#if 1
		ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
		if (!ret) {
			bes_devel("bes_slave ready tx %d bytes\n", rx_size);
		} else {
			bes_err("wait slave process download fw data err:%d\n", ret);
			goto err2;
		}
#ifdef BES_SLAVE_TX_DOUBLE_CHECK
	if (rx_size != 8)
		rx_size = 8;
#endif
#else
		mdelay(100);
		rx_size = 8;
#endif

		ret = bes2600_data_read(short_buf, rx_size);
		if (ret) {
			bes_err("rx tx download fw data rsp err:%d\n", ret);
			goto err2;
		}

		//check device rx status
		ret = bes_frame_rsp_check(short_buf, last_frame_num);
		if (ret) {
			bes_err("rsp tx download fw err:%d\n", ret);
			goto err2;
		}

		code_length -= length;
		data_p += length;
		download_addr.addr += length;
		bes_devel("already tx fw size:%x/%x\n", download_addr.addr - fw_info.addr, fw_info.len);
	}

	//Notify Device:The firmware download is complete

#if 1
	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (ret) {
		goto err2;
	} else {
		bes_devel("bes salve rx ready %d bytes\n", tx_size);
	}
#endif
#ifdef BES_SLAVE_RX_DOUBLE_CHECK
		tx_size = 512;
#endif

	header.type = FRAME_HEADER_DOWNLOAD_END;
	header.seq = frame_num;
	header.len = sizeof(struct fw_crc_t);
	last_frame_num = frame_num;
	frame_num++;

	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&crc32_t.crc32, sizeof(struct fw_crc_t));
	length = BES_FW_MSG_TOTAL_LEN(header);

	bes_devel("%s", "tx download firmware complete command\n");

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx downlod firmware complete command err:%d\n", ret);
		goto err2;
	}

#if 1
	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("bes_slave ready tx %d bytes\n", rx_size);
	} else {
		bes_err("wait slave process download fw data err:%d\n", ret);
		goto err2;
	}
#ifdef BES_SLAVE_TX_DOUBLE_CHECK
	if (rx_size != 8)
		rx_size = 8;
#endif
#else
	mdelay(100);
	rx_size = 8;
	bes_devel("enter sdio irqs:%d", enter_sdio_irqs);
#endif

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("receive download firmware complete cmd rsp err:%d\n", ret);
		goto err2;
	}

	//check device rx status
	ret = bes_frame_rsp_check(short_buf, last_frame_num);
	if (ret) {
		bes_err("rsp download firmware complete err:%d\n", ret);
		goto err2;
	}

	if (auto_run == false) {
		bes_devel("partial firmware(%s) is downloaded successfully\n", fw_name);
		goto err2;
	}

#if 1
	ret = bes_slave_rx_ready(fw_data, &buf_cnt, &tx_size, HZ);
	if (ret) {
		goto err2;
	} else {
		bes_devel("bes salve rx ready %d bytes\n", tx_size);
	}
#endif
#ifdef BES_SLAVE_RX_DOUBLE_CHECK
	tx_size = 512;
#endif

	//Notify Device:Run firmware
	run_addr.addr = fw_info.addr;

	header.type = FRAME_HEADER_RUN_CODE;
	header.seq = frame_num;
	header.len = sizeof(struct run_fw_t);
	last_frame_num = frame_num;
	frame_num++;

	memcpy(short_buf, (u8 *)&header, sizeof(struct fw_msg_hdr_t));
	memcpy(short_buf + sizeof(struct fw_msg_hdr_t), (u8 *)&run_addr.addr, sizeof(struct run_fw_t));
	length = BES_FW_MSG_TOTAL_LEN(header);

	bes_devel("tx run firmware command:0x%X\n", run_addr.addr);

	length = length > 512 ? length : 512;
	ret = bes2600_data_write(short_buf, length);
	if (ret) {
		bes_err("tx run firmware command err:%d\n", ret);
		goto err2;
	}

#if 1
	ret = bes_slave_tx_ready(fw_data, &rx_size, HZ);
	if (!ret) {
		bes_devel("bes_slave ready tx %d bytes\n", rx_size);
	} else {
		bes_err("wait slave process run fw cmd err:%d\n", ret);
		goto err2;
	}
#ifdef BES_SLAVE_TX_DOUBLE_CHECK
	if (rx_size != 8)
		rx_size = 8;
#endif
#else
	mdelay(100);
	rx_size = 8;
#endif

	ret = bes2600_data_read(short_buf, rx_size);
	if (ret) {
		bes_err("rx run firmware command err:%d\n", ret);
		goto err2;
	}

	//check device rx status
	ret = bes_frame_rsp_check(short_buf, last_frame_num);
	if (ret) {
		bes_err("rsp run firmware command err:%d\n", ret);
		goto err2;
	}

	bes_devel("%s", "firmware is downloaded successfully and is already running\n");
	msleep(500);

err2:
	kfree(long_buf);
#ifdef DATA_DUMP_OBSERVE
	if (observe_file) {
		filp_close(observe_file, NULL);
	}
#endif
err1:
	kfree(short_buf);
	release_firmware(fw_bin);
	if (ret && retry_cnt < 3) {
		retry_cnt++;
		goto retry;
	}
	return ret;
}

static int bes_read_dpd_data(struct platform_fw_t *fw_data)
{
	u16 dpd_size = 0;
	int ret = 0;
	u8 *dpd_buf = NULL;
	u8 mcu_status = 0;
	unsigned long wait_timeout;

	/* wait for device ready */
	wait_timeout = jiffies + 15 * HZ;
	do {
		msleep(100);
		ret = bes2600_reg_read(BES_SLAVE_STATUS_REG_ID, &mcu_status, 1);
	} while(((ret == 0) || (ret == -84)) &&
	        !(mcu_status & BES_SLAVE_STATUS_DPD_READY) &&
		time_before(jiffies, wait_timeout));

	/* check if read dpd error */
	if(ret < 0 || time_after(jiffies, wait_timeout)) {
		bes_err("wait dpd data ready failed:%d\n", ret);
		return -1;
	}

	/* wait dpd read ready */
	ret = bes_slave_tx_ready(fw_data, &dpd_size, HZ);
	if (ret)  {
		bes_err("wait dpd data failed:%d\n", ret);
		return -1;
	}

	/* dpd size check */
	if (dpd_size != DPD_BIN_SIZE) {
		bes_err("get dpd data size err:%u\n", dpd_size);
		return -1;
	}

	/* read dpd data */
	dpd_buf = bes2600_chrdev_get_dpd_buffer(DPD_BIN_FILE_SIZE);
	if(!dpd_buf) {
		bes_err("allocate dpd buffer failed.\n");
		return -1;
	}

	ret = bes2600_data_read(dpd_buf, dpd_size);
	bes_devel("read dpd data size:%d\n", dpd_size);
	if (ret) {
		bes_err("read dpd data failed:%d\n", ret);
		bes2600_chrdev_free_dpd_data();
		return -1;
	}

	/* update dpd data */
	ret = bes2600_chrdev_update_dpd_data();
	if (ret)
		bes2600_chrdev_free_dpd_data();

	return ret;
}

#ifdef BES2600_DUMP_FW_DPD_LOG
static int bes_read_dpd_log(struct platform_fw_t *fw_data)
{
	u16 dpd_log_size = 0;
	int ret = 0;
	u8 mcu_status = 0;
	u8 *dpd_log = NULL;
	unsigned long wait_timeout;

	/* wait for device ready */
	wait_timeout = jiffies + 5 * HZ;
	do {
		msleep(10);
		ret = bes2600_reg_read(BES_SLAVE_STATUS_REG_ID, &mcu_status, 1);
	} while(((ret == 0) || (ret == -84)) &&
	        !(mcu_status & BES_SLAVE_STATUS_DPD_LOG_READY) &&
		time_before(jiffies, wait_timeout));

	if(ret < 0 || time_after(jiffies, wait_timeout)) {
		bes_err("wait dpd log ready failed:%d\n", ret);
		return -1;
	}

	/* wait dpd log dump data ready */
	ret = bes_slave_tx_ready(fw_data, &dpd_log_size, HZ);
	if (ret) {
		bes_err("wait dpd log failed:%d\n", ret);
		return -1;
	}

	dpd_log = bes2600_alloc_dpd_log_buffer((dpd_log_size + 3) & (~0x3));
	if(!dpd_log) {
		bes_err("dpd log buffer alloc fail");
		return -1;
	}

	ret = bes2600_data_read(dpd_log, (dpd_log_size + 3) & (~0x3));
	if (ret) {
		bes_err("read dpd log failed:%d\n", ret);
		bes2600_free_dpd_log_buffer();
		return -1;
	}

	bes_devel("read dpd log size: %u\n", dpd_log_size);

	return ret;
}
#endif /* BES2600_DUMP_FW_DPD_LOG */

static int bes2600_load_wifi_firmware(struct platform_fw_t *fw_data)
{
	int ret = 0;
	const char *fw_name_tbl[3];
	int fw_type = bes2600_chrdev_get_fw_type();

	fw_name_tbl[0] = BES2600_LOAD_FW_NAME;
	fw_name_tbl[1] = BES2600_LOAD_NOSIGNAL_FW_NAME;
	fw_name_tbl[2] = BES2600_LOAD_BTRF_FW_NAME;

	bes_devel("bes2600 download cali and wifi signal firmware.\n");
	ret = bes_firmware_download(fw_data, BES2600_LOAD_BOOT_NAME, true);
	if (ret)
		bes_err("download dpd cali firmware failed\n");

	if (!ret) {
		bes_devel("bes2600 read dpd cali data.\n");
		ret = bes_read_dpd_data(fw_data);
		if (ret)
			bes_err("read dpd data failed.\n");
	}

#ifdef BES2600_DUMP_FW_DPD_LOG
	if (!ret) {
		bes_devel("bes2600 read dpd log data.\n");
		ret = bes_read_dpd_log(fw_data);
		if (ret)
			bes_err("dump dpd log failed.\n");
	}
#endif

	/* for wifi non-signal mode, download second firmware directly */
	if (!ret && bes2600_chrdev_check_system_close()) {
		bes_devel("bes2600 device power down.\n");
		ret = bes2600_chrdev_do_system_close(fw_data->sbus_ops, fw_data->sbus_priv);
		if (ret)
			bes_err("device down fail.\n");
	} else if (!ret) {
		ret = bes_firmware_download(fw_data, fw_name_tbl[fw_type], true);
		if (ret)
			bes_err("download normal firmware failed.\n");
	}

	return ret;
}

static int bes2600_load_wifi_firmware_with_dpd(struct platform_fw_t *fw_data)
{
	int ret = 0;
	u32 dpd_data_len = 0;
	const u8 *dpd_data = NULL;
	const char *fw_name_tbl[3];
	int fw_type = bes2600_chrdev_get_fw_type();

	fw_name_tbl[0] = BES2600_LOAD_FW_NAME;
	fw_name_tbl[1] = BES2600_LOAD_NOSIGNAL_FW_NAME;
	fw_name_tbl[2] = BES2600_LOAD_BTRF_FW_NAME;

	dpd_data = bes2600_chrdev_get_dpd_data(&dpd_data_len);
	BUG_ON(!dpd_data);

	bes_devel("bes2600 download firmware with dpd.\n");
	ret = bes_firmware_download_write_mem(fw_data, BES2600_DPD_ADDR, dpd_data, dpd_data_len);
	if (ret)
		bes_err("download dpd data failed.\n");
	if (!ret) {
		ret = bes_firmware_download(fw_data, fw_name_tbl[fw_type], true);
		if (ret)
			bes_err("download firmware failed after dpd download.\n");
	}

	return ret;
}

static int bes2600_load_bt_firmware(struct platform_fw_t *fw_data)
{
	int ret = 0;

	/* for bt mode, don't need to download dpd cali firmware*/
	bes_devel("download bt test firmware.\n");
	ret = bes_firmware_download(fw_data, BES2600_LOAD_BTRF_FW_NAME, true);
	if (ret)
		bes_err("download normal firmware failed.\n");

	return ret;

}

int bes2600_load_firmware_sdio(struct sbus_ops *ops, struct sbus_priv *priv)
{
	int ret = 0;
	struct platform_fw_t *temp_fw_data;
	u32 dpd_data_len = 0;
	const u8 *dpd_data = NULL;
	int fw_type = bes2600_chrdev_get_fw_type();
#ifdef CONFIG_BES2600_CALIB_FROM_LINUX
	u8 *factory_data = NULL;
	u8 *file_buffer = NULL;
	u32 factory_data_len = 0;
#endif

	temp_fw_data = kzalloc(sizeof(struct platform_fw_t), GFP_KERNEL);
	if (!temp_fw_data)
		return -ENOMEM;

	init_completion(&temp_fw_data->completion_data);
	temp_fw_data->sbus_ops = ops;
	temp_fw_data->sbus_priv = priv;

	temp_fw_data->sbus_ops->irq_subscribe(temp_fw_data->sbus_priv,
			(sbus_irq_handler)bes_fw_irq_handler, temp_fw_data);

	bes_firmware_download_write_reg(temp_fw_data, 0x40100000, 0x802006);
	bes_firmware_download_write_reg(temp_fw_data, 0x4008602C, 0x3E00C000);

#ifdef CONFIG_BES2600_CALIB_FROM_LINUX
	if (!(file_buffer = bes2600_factory_get_file_buffer()))
		return -ENOMEM;

	bes2600_factory_lock();
	if (!(factory_data = bes2600_get_factory_cali_data(file_buffer, &factory_data_len, FACTORY_PATH))) {
		bes_warn("factory cali data get failed.\n");
	} else {
		bes2600_factory_data_check(factory_data);
		factory_little_endian_cvrt(factory_data);
		ret = bes_firmware_download_write_mem(temp_fw_data, BES2600_FACTORY_ADDR, factory_data, factory_data_len);
		if (ret)
			bes_err("download factory data failed.\n");
	}

	bes2600_factory_free_file_buffer(file_buffer);
	bes2600_factory_unlock();
#endif

	bes_devel("%s fw_type:%d.\n", __func__, fw_type);
	if(fw_type == BES2600_FW_TYPE_BT) {
		ret = bes2600_load_bt_firmware(temp_fw_data);
	} else {
		dpd_data = bes2600_chrdev_get_dpd_data(&dpd_data_len);
		if(dpd_data) {
			ret = bes2600_load_wifi_firmware_with_dpd(temp_fw_data);
		} else {
			ret = bes2600_load_wifi_firmware(temp_fw_data);
		}
	}


	/* don't register net device when wifi is closed */
	if(!ret && !bes2600_chrdev_is_wifi_opened()) {
		ret = 1;
	}


	temp_fw_data->sbus_ops->irq_unsubscribe(temp_fw_data->sbus_priv);
	kfree(temp_fw_data);

	bes_devel("download finished ,wifi_state:%d result is %d\n", bes2600_chrdev_is_wifi_opened(), ret);

	return ret;
}
