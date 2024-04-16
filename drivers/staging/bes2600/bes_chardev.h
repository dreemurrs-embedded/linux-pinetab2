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
#ifndef __BES_CHARDEV_H__
#define __BES_CHARDEV_H__

#define BES2600_FW_TYPE_WIFI_SIGNAL	0
#define BES2600_FW_TYPE_WIFI_NO_SIGNAL	1
#define BES2600_FW_TYPE_BT		2
#define BES2600_FW_TYPE_MAX_NUM     3

#define DPD_VERSION_OFFSET      0x3AF4
#define DPD_BIN_SIZE            0x3B14
#define DPD_BIN_FILE_SIZE       0x4000
#define DPD_CUR_VERSION         7

enum pend_read_op {
	BES_CDEV_READ_WAKEUP_STATE = 0,
	/* add new here */

	BES_CDEV_READ_NUM_MAX,
};

enum wifi_wakeup_reason_code {
	WAKEUP_REASON_WIFI_DEAUTH_DISASSOC = 0x1000,
	WAKEUP_REASON_WIFI_BSSLOST,
	/* add new here */
};

enum bt_wakeup_reason_code {
	WAKEUP_REASON_BT_PLAY = 0x0100,
	/* add new here */
};

enum wakeup_event {
	WAKEUP_EVENT_NONE = 0,
	WAKEUP_EVENT_SETTING,
	WAKEUP_EVENT_WSME,
	WAKEUP_EVENT_PEER_DETACH,
	/* add new here */
};

/* dpd management */
u8* bes2600_chrdev_get_dpd_buffer(u32 size);
int bes2600_chrdev_update_dpd_data(void);
const u8* bes2600_chrdev_get_dpd_data(u32 *len);
void bes2600_chrdev_free_dpd_data(void);

/* get/set subs_priv instance from/to bes_chrdev module */
void bes2600_chrdev_set_sbus_priv_data(struct sbus_priv *priv, bool error);
struct sbus_priv *bes2600_chrdev_get_sbus_priv_data(void);

/* used to control device power down */
int bes2600_chrdev_check_system_close(void);
int bes2600_chrdev_do_system_close(const struct sbus_ops *sbus_ops, struct sbus_priv *priv);
void bes2600_chrdev_wakeup_bt(void);
void bes2600_chrdev_wifi_force_close(struct bes2600_common *hw_priv, bool halt_dev);
void bes2600_chrdev_usb_remove(struct bes2600_common *hw_priv);

/* get and set internal state */
bool bes2600_chrdev_is_wifi_opened(void);
bool bes2600_chrdev_is_bt_opened(void);
int bes2600_chrdev_get_fw_type(void);
bool bes2600_chrdev_is_signal_mode(void);
void bes2600_chrdev_update_signal_mode(void);
bool bes2600_chrdev_is_bus_error(void);

/* bus probe check */
void bes2600_chrdev_start_bus_probe(void);
void bes2600_chrdev_bus_probe_notify(void);

/* set wifi wakeup state */
void bes2600_chrdev_wifi_update_wakeup_reason(u16 reason, u16 port);
void bes2600_chrdev_wakeup_by_event_set(enum wakeup_event wakeup_event);
int bes2600_chrdev_wakeup_by_event_get(void);

/* init and deinit module */
int bes2600_chrdev_init(struct sbus_ops *ops);
void bes2600_chrdev_free(void);

#ifdef BES2600_DUMP_FW_DPD_LOG
void bes2600_free_dpd_log_buffer(void);
u8* bes2600_alloc_dpd_log_buffer(u16 len);
void bes2600_get_dpd_log(char **data, size_t *len);
#endif

#endif /* __BES_CHARDEV_H__ */
