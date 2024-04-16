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
#include<linux/module.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/crc32.h>
#include <linux/version.h>
#include "bes2600.h"
#include "sbus.h"
#include "hwio.h"
#include "fwio.h"
#include "bes_chardev.h"
#include "tx_loop.h"
#include "bes_log.h"

enum wait_state {
	BES2600_BOOT_WAIT_NONE = 0,
	BES2600_BOOT_WAIT_PROBE_DONE,
	BES2600_BOOT_WAIT_CLOSE,
};

enum bus_probe_state {
	BES2600_BUS_PROBE_NONE = 0,
	BES2600_BUS_PROBE_START,
	BES2600_BUS_PROBE_OK,
	BES2600_BUS_PROBE_TIMEOUT,
};

struct bes_cdev {
	struct cdev cdev;
	dev_t dev_id;
	int major;
	int minor;
	struct class *class;
	struct device *device;
	atomic_t num_proc;
	wait_queue_head_t open_wq;
	spinlock_t status_lock;
	bool wifi_opened;
	bool bt_opened;
	bool bton_pending;
	bool dpd_calied;
	u8 *dpd_data;
	u32 dpd_len;
	enum wait_state wait_state;
	wait_queue_head_t probe_done_wq;
	const struct sbus_ops *sbus_ops;
	struct sbus_priv *sbus_priv;
	bool sig_mode;
	int fw_type;
	bool bus_error;
	bool halt_dev;
	struct delayed_work probe_timeout_work;
	enum bus_probe_state bus_probe;
	struct work_struct wifi_force_close_work;
#ifdef BES2600_WRITE_DPD_TO_FILE
	int no_dpd;
#endif
	enum pend_read_op read_flag;
	enum wakeup_event wakeup_by_event;	/* used to filter unwanted event wakeup reason report */
	u16 wakeup_state; /* for userspace check wakeup reason */
	wait_queue_head_t wakeup_reason_wq;
	u16 src_port;
#ifdef BES2600_DUMP_FW_DPD_LOG
	u8 *dpd_log;
	u16 dpd_log_len;
#endif
};

struct bes2600_op_map {
	char op[20];	// operation
	int op_len;	// operation length, used for effiency
	int (*handler)(const char *cmd); // handler
};

static struct bes_cdev bes2600_cdev;
module_param_named(fw_type, bes2600_cdev.fw_type, int, 0644);
#ifdef BES2600_WRITE_DPD_TO_FILE
module_param_named(no_dpd, bes2600_cdev.no_dpd, int, 0644);
#endif

extern int bes2600_register_net_dev(struct sbus_priv *bus_priv);
extern int bes2600_unregister_net_dev(struct sbus_priv *bus_priv);
extern bool bes2600_is_net_dev_created(struct sbus_priv *bus_priv);

static bool bes2600_bootup_end(void)
{
	bool end;

	spin_lock(&bes2600_cdev.status_lock);
	end = (bes2600_cdev.bus_probe == BES2600_BUS_PROBE_TIMEOUT ||
	       bes2600_cdev.sbus_priv != NULL ||
	       bes2600_cdev.bus_error);
	spin_unlock(&bes2600_cdev.status_lock);

	return end;
}

static int bes2600_chrdev_switch_subsys(int wake_flag, int subsys, bool active)
{
	int ret = 0;

	if (bes2600_cdev.sbus_priv == NULL)
		return -EFAULT;

	if (active) {
		if (bes2600_cdev.sbus_ops->gpio_wake)
			bes2600_cdev.sbus_ops->gpio_wake(bes2600_cdev.sbus_priv, wake_flag);

		if (bes2600_cdev.sbus_ops->sbus_active)
			ret = bes2600_cdev.sbus_ops->sbus_active(bes2600_cdev.sbus_priv, subsys);

		if (bes2600_cdev.sbus_ops->gpio_sleep)
			bes2600_cdev.sbus_ops->gpio_sleep(bes2600_cdev.sbus_priv, wake_flag);
	} else {
		if (bes2600_cdev.sbus_ops->gpio_wake)
			bes2600_cdev.sbus_ops->gpio_wake(bes2600_cdev.sbus_priv, wake_flag);

		if (bes2600_cdev.sbus_ops->sbus_deactive)
			ret = bes2600_cdev.sbus_ops->sbus_deactive(bes2600_cdev.sbus_priv, subsys);

		if (bes2600_cdev.sbus_ops->gpio_sleep)
			bes2600_cdev.sbus_ops->gpio_sleep(bes2600_cdev.sbus_priv, wake_flag);
	}

	return ret;
}

static int bes2600_switch_wifi(bool on)
{
	int ret = 0;
	long status = 0;

	if (bes2600_cdev.wifi_opened == on)
		return 0;

	if (on) {
		if (bes2600_chrdev_check_system_close()) {
			bes_devel("power up bes2600 when active wifi.\n");
			/* reset bus error status when restart bes2600 */
			spin_lock(&bes2600_cdev.status_lock);
			bes2600_cdev.bus_error = false;
			bes2600_cdev.halt_dev = false;
			bes2600_cdev.bus_probe = BES2600_BUS_PROBE_NONE;
			spin_unlock(&bes2600_cdev.status_lock);

			/* power up bes2600, trigger system to execute probe function */
			bes2600_cdev.wifi_opened = true;
			bes2600_cdev.sbus_ops->power_switch(NULL, 1);

			/* wait probe done event */
			status = wait_event_timeout(bes2600_cdev.probe_done_wq,
					bes2600_bootup_end(), HZ * 8);
			WARN_ON(status <= 0);
			ret = (status <= 0 || bes2600_chrdev_is_bus_error()) ? -1 : 0;
		} else {
			/* bes2600 is already powered up, we just need to create net device */
			if (bes2600_cdev.sbus_priv) {
				if (!bes2600_is_net_dev_created(bes2600_cdev.sbus_priv)) {
					ret = bes2600_register_net_dev(bes2600_cdev.sbus_priv);
				}
			} else {
				ret = -EFAULT;
			}
		}
	} else {
		if (bes2600_cdev.sbus_priv && bes2600_is_net_dev_created(bes2600_cdev.sbus_priv)) {
			bes2600_unregister_net_dev(bes2600_cdev.sbus_priv);
		}
	}

	if (!ret) {
		bes2600_cdev.wifi_opened = on;
	} else {
		bes2600_cdev.wifi_opened = false;
		if (on)
			bes_info("open wifi failed\n");
	}

	return ret;
}

static int bes2600_switch_bt(bool on)
{
	int ret = 0;
	long status = 0;

	if (bes2600_cdev.bt_opened == on)
		return 0;

	if (on) {
		if (bes2600_chrdev_check_system_close()) {
			bes_devel("power up bes2600 when active bt.\n");
			/* reset bus error status when restart bes2600 */
			spin_lock(&bes2600_cdev.status_lock);
			bes2600_cdev.bus_error = false;
			bes2600_cdev.halt_dev = false;
			bes2600_cdev.bus_probe = BES2600_BUS_PROBE_NONE;
			spin_unlock(&bes2600_cdev.status_lock);

			/* set opend state in advance */
			bes2600_cdev.bt_opened = true;
			bes2600_cdev.bton_pending = true;

			/* power up bes2600, trigger system to execute probe function */
			bes2600_cdev.sbus_ops->power_switch(NULL, 1);

			/* wait bootup process end */
			status = wait_event_timeout(bes2600_cdev.probe_done_wq,
				bes2600_bootup_end(), HZ * 8);
			WARN_ON(status <= 0);

			/* check if there is a error when bootup */
			ret = (status <= 0 || bes2600_chrdev_is_bus_error()) ? -1 : 0;
		} else {
			bes_devel("bes2600 activate bt.\n");
			ret = bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_ON, SUBSYSTEM_BT, true);
		}
	} else {
		bes_devel("bes2600 deactivate bt.\n");
		bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_OFF, SUBSYSTEM_BT, false);
	}

	if (!ret) {
		bes2600_cdev.bt_opened = on;
	} else {
		bes2600_cdev.bt_opened = false;
		bes2600_cdev.bton_pending = false;
		if (ret)
			bes_info("open bt failed\n");
	}

	return ret;
}

static int bes2600_get_cmd_and_ifname(const char *str, char **result)
{
	int cmd_len = 0;
	int ifname_len = 0;
	char *sp = NULL;
	char *tmp_ptr = NULL;
	char *cmd_ptr = NULL;

	/* check if input arguments is valid */
	if (!str || strncmp(str, "ifname:", 7) != 0)
		return -1;

	sp = strchr(str, ' ');
	if (strncmp(sp + 1, "cmd:", 4) != 0)
		return -1;

	/* extract interface name */
	ifname_len = sp - str - 7;
	tmp_ptr = kmalloc(ifname_len + 1, GFP_KERNEL);
	if (!tmp_ptr) {
		return -2;
	}

	strncpy(tmp_ptr, str+7, ifname_len);
	tmp_ptr[ifname_len] = '\0';
	result[0] = tmp_ptr;

	/* get command length */
	cmd_ptr = strstr(str, "cmd:");
	cmd_ptr += 4;
	sp = strchr(cmd_ptr, ' ');
	if (!sp) {	/* the command don't have any parameter */
		cmd_len = strlen(cmd_ptr);
		if (cmd_ptr[cmd_len - 1] == '\n')
			--cmd_len;
	} else {	/* the command have one or more parameter */
		cmd_len = sp - cmd_ptr;
	}

	/* copy command to out buffer */
	tmp_ptr = kmalloc( cmd_len + 1, GFP_KERNEL);
	if (!tmp_ptr) {
		kfree(result[0]);
		result[0] = NULL;
		return -3;
	}

	strncpy(tmp_ptr, cmd_ptr, cmd_len);
	tmp_ptr[cmd_len] = '\0';
	result[1] = tmp_ptr;

	return 0;
}

static void bes2600_recyle_cmd_and_ifname_mem(char **info)
{
	if (info[0]) {
		kfree(info[0]);
		info[0] = NULL;
	}

	if (info[1]) {
		kfree(info[1]);
		info[1] = NULL;
	}

}

static int bes2600_op_default_handler(const char *str)
{
	char *info[2] = {0};

	if (bes2600_get_cmd_and_ifname(str, info) == 0) {
		bes_devel("cmd(%s) on %s not handled\n", info[1], info[0]);
	} else {
		bes_err("%s get command fail, the origin string is %s\n", __func__, str);
	}

	bes2600_recyle_cmd_and_ifname_mem(info);

	return 0;
}

static int bes2600_op_wifi_bt_on_off(const char *str)
{
	char *info[2] = {0};
	int ret = 0;
	enum wait_state wait_state;
	enum bus_probe_state probe_state;
	unsigned long status = 0;

	spin_lock(&bes2600_cdev.status_lock);
	probe_state = bes2600_cdev.bus_probe;
	wait_state = bes2600_cdev.wait_state;
	spin_unlock(&bes2600_cdev.status_lock);

	/* only work for wifi signal mode */
	if (bes2600_cdev.fw_type != BES2600_FW_TYPE_WIFI_SIGNAL)
		return -EFAULT;

	/* wait bus probe operation end */
	if (probe_state == BES2600_BUS_PROBE_START) {
		bes_devel("wait bus probe operation end\n");
		status = wait_event_timeout(bes2600_cdev.probe_done_wq,
					(bes2600_cdev.bus_probe > BES2600_BUS_PROBE_START),
					HZ);
		WARN_ON(status <= 0);
	}

	/* must wait previous operation end in critical section */
	if (wait_state != BES2600_BOOT_WAIT_NONE) {
		bes_devel("wait previous operation end\n");
		status = wait_event_timeout(bes2600_cdev.probe_done_wq,
					(bes2600_cdev.wait_state == BES2600_BOOT_WAIT_NONE),
					HZ * 8);
		WARN_ON(status <= 0);
	}

	/* if dpd calibration is doing, modify wifi and bt state directly */
	spin_lock(&bes2600_cdev.status_lock);
	if (bes2600_cdev.bus_probe == BES2600_BUS_PROBE_OK && !bes2600_cdev.dpd_calied) {
		if (bes2600_get_cmd_and_ifname(str, info) == 0) {
			if (strncmp(info[1], "WIFI_ON", 7) == 0) {
				bes2600_cdev.wifi_opened = true;
			} else if (strncmp(info[1], "WIFI_OFF", 8) == 0) {
				bes2600_cdev.wifi_opened = false;
			} else if (strncmp(info[1], "BT_ON", 5) == 0) {
				bes2600_cdev.bt_opened = true;
				bes2600_cdev.bton_pending = true;
			} else if (strncmp(info[1], "BT_OFF", 6) == 0) {
				bes2600_cdev.bt_opened = false;
				bes2600_cdev.bton_pending = false;
			}
		}
		bes2600_recyle_cmd_and_ifname_mem(info);
		spin_unlock(&bes2600_cdev.status_lock);

		/* wait probe done event */
		status = wait_event_timeout(bes2600_cdev.probe_done_wq,
				bes2600_bootup_end(), HZ * 8);
		WARN_ON(status <= 0);

		return (status <= 0 || bes2600_chrdev_is_bus_error()) ? -EFAULT : 0;
	}
	spin_unlock(&bes2600_cdev.status_lock);

	/* process wifi/bt on/off operation */
	if (bes2600_get_cmd_and_ifname(str, info) == 0) {
		if (strncmp(info[1], "WIFI_ON", 7) == 0) {
			ret = bes2600_switch_wifi(1);
		} else if (strncmp(info[1], "WIFI_OFF", 8) == 0) {
			ret = bes2600_switch_wifi(0);
		} else if (strncmp(info[1], "BT_ON", 5) == 0) {
			ret = bes2600_switch_bt(1);
		} else if (strncmp(info[1], "BT_OFF", 6) == 0) {
			ret = bes2600_switch_bt(0);
		}
	}

	if (!ret && bes2600_chrdev_check_system_close())
		ret = bes2600_chrdev_do_system_close(bes2600_cdev.sbus_ops,
						bes2600_cdev.sbus_priv);

	bes2600_recyle_cmd_and_ifname_mem(info);

	return ret ;
}


static int bes2600_op_change_fw_type(const char *str)
{
	int ret = 0;
	int temp = 0;
	long status = 0;
	char *cmd_ptr = NULL;
	char fw_type[5] = {0};
	bool sys_closed = bes2600_chrdev_check_system_close();

	bes_devel("%s is called, arg:%s\n", __func__, str);

	if (!bes2600_cdev.sbus_ops->power_switch && !bes2600_cdev.sbus_ops->reboot)
		return -EPERM;

	/* check if user input is valid */
	cmd_ptr = strstr(str, "CHANGE_FW_TYPE ");
	if (strlen(str) < 16 || !cmd_ptr) {
		bes_err("the format of \"%s\" is error\n", str);
		return -EINVAL;
	}

	/* convert fw_type from string to int */
	strncpy(fw_type, cmd_ptr + 14, 4);
	fw_type[0] = '+';
	ret = kstrtoint(fw_type, 10, &temp);
	if (ret < 0) {
		bes_err("%s parse error\n", __func__);
		return -EINVAL;
	}

	/* no need to realod firmware if new fw_type is equal to the old */
	if (temp == bes2600_cdev.fw_type ) {
		bes_devel("fw type is equal\n");
		return 0;
	}

	/* close wifi net device */
	if (bes2600_cdev.sbus_priv
	    && bes2600_is_net_dev_created(bes2600_cdev.sbus_priv)) {
		bes2600_unregister_net_dev(bes2600_cdev.sbus_priv);
	}

	/* update firmware type */
	bes2600_cdev.fw_type = temp;
	bes2600_chrdev_update_signal_mode();

	if (!sys_closed) {
		/* close device to call disconnect function */
		if (bes2600_cdev.sbus_ops->power_switch)
			bes2600_cdev.sbus_ops->power_switch(bes2600_cdev.sbus_priv, 0);
		else if (bes2600_cdev.sbus_ops->reboot)
			bes2600_cdev.sbus_ops->reboot(bes2600_cdev.sbus_priv);
	}

	if (bes2600_cdev.sbus_ops->reboot)
		bes2600_chrdev_start_bus_probe();

	/* wait disconnect event */
	status = wait_event_timeout(bes2600_cdev.probe_done_wq, (bes2600_cdev.sbus_priv == NULL), HZ * 10);
	WARN_ON(status <= 0);

	if (bes2600_cdev.dpd_calied
	   && bes2600_chrdev_check_system_close()) {
		bes_devel("no need to reload firmware\n");
		return 0;
	}

	bes_devel("reload firmware...\n");
	/* power on device to call probe function */
	if (bes2600_cdev.sbus_ops->power_switch)
		bes2600_cdev.sbus_ops->power_switch(NULL, 1);

	/* wait probe done event */
	status = wait_event_timeout(bes2600_cdev.probe_done_wq,
			bes2600_bootup_end(), HZ * 10);
	WARN_ON(status <= 0);

	ret = (status <= 0 || bes2600_chrdev_is_bus_error()) ? -1 : 0;


	return ret;
}

static int bes2600_op_bt_wakeup(const char *str)
{
	int ret = 0;
	unsigned long status = 0;

	spin_lock(&bes2600_cdev.status_lock);
	if (!bes2600_cdev.bt_opened) {
		spin_unlock(&bes2600_cdev.status_lock);
		return -EFAULT;
	}
	spin_unlock(&bes2600_cdev.status_lock);

	/* wait probe done event */
	status = wait_event_timeout(bes2600_cdev.probe_done_wq,
			bes2600_bootup_end(), HZ * 8);
	if (status <= 0 || bes2600_chrdev_is_bus_error())
		return -EFAULT;

	bes_devel("bes2600 wakeup bt.\n");
	ret = bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_LP_ON, SUBSYSTEM_BT_LP, true);

	return ret;
}

static int bes2600_op_bt_sleep(const char *str)
{
	int ret = 0;
	unsigned long status = 0;

	spin_lock(&bes2600_cdev.status_lock);
	if (!bes2600_cdev.bt_opened) {
		spin_unlock(&bes2600_cdev.status_lock);
		return -EFAULT;
	}
	spin_unlock(&bes2600_cdev.status_lock);

	/* wait probe done event */
	status = wait_event_timeout(bes2600_cdev.probe_done_wq,
			bes2600_bootup_end(), HZ * 8);
	if (status <= 0 || bes2600_chrdev_is_bus_error())
		return -EFAULT;

	bes_devel("bes2600 allow bt sleep.\n");
	ret = bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_LP_OFF, SUBSYSTEM_BT_LP, false);

	return ret;
}

static int bes2600_op_set_wakeup_read_flag(const char *str)
{
	bes_devel("%s is called, arg:%s\n", __func__, str);
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.read_flag = BES_CDEV_READ_WAKEUP_STATE;
	spin_unlock(&bes2600_cdev.status_lock);

	return 0;
}

#ifdef FW_DOWNLOAD_UART_DAEMON
int bes2600_load_uevent(char *env[])
{
	return kobject_uevent_env(&bes2600_cdev.device->kobj, KOBJ_CHANGE, env);
}
#endif

static struct bes2600_op_map bes2600_op_map_tab[] ={
	/*op			op_len	handler				*/
	{"P2P_SET_NOA", 	11,	bes2600_op_default_handler},
	{"P2P_SET_PS", 		10,	bes2600_op_default_handler},
	{"SET_AP_WPS_P2P_IE",	17, 	bes2600_op_default_handler},
	{"LINKSPEED", 		9,	bes2600_op_default_handler},
	{"RSSI",		4,	bes2600_op_default_handler},
	{"GETBAND", 		7,	bes2600_op_default_handler},
	{"WLS_BATCHING", 	12,	bes2600_op_default_handler},
	{"MACADDR",		7,	bes2600_op_default_handler},
	{"RXFILTER-START",	14,	bes2600_op_default_handler},
	{"RXFILTER-STOP",	13,	bes2600_op_default_handler},
	{"RXFILTER-ADD",	12,	bes2600_op_default_handler},
	{"RXFILTER-REMOVE",	15,	bes2600_op_default_handler},
	{"BTCOEXMODE",		10,	bes2600_op_default_handler},
	{"BTCOEXSCAN-START",	16,	bes2600_op_default_handler},
	{"BTCOEXSCAN-STOP",	15,	bes2600_op_default_handler},
	{"SETSUSPENDMODE",	14,	bes2600_op_default_handler},
	{"COUNTRY",		7,	bes2600_op_default_handler},
	{"WIFI_ON", 		7,	bes2600_op_wifi_bt_on_off},
	{"WIFI_OFF", 		8,	bes2600_op_wifi_bt_on_off},
	{"BT_ON", 		5,	bes2600_op_wifi_bt_on_off},
	{"BT_OFF", 		6,	bes2600_op_wifi_bt_on_off},
	{"CHANGE_FW_TYPE",	14,	bes2600_op_change_fw_type},
	{"BT_WAKEUP",		9,	bes2600_op_bt_wakeup},
	{"BT_SLEEP",		8,	bes2600_op_bt_sleep},
	{"WAKEUP_STATE",	12,	bes2600_op_set_wakeup_read_flag},
};

static int bes2600_chrdev_check_system_close_internal(void)
{
	return (bes2600_cdev.sbus_ops->power_switch != NULL &&
		bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_SIGNAL)
		&&(bes2600_cdev.bt_opened == false)
		&& (bes2600_cdev.wifi_opened == false);
}

static int bes2600_chrdev_open(struct inode *inode, struct file *filp)
{
	if (atomic_read(&bes2600_cdev.num_proc) > 0) {
		wait_event_timeout(bes2600_cdev.open_wq,
			(atomic_read(&bes2600_cdev.num_proc) == 0),
			MAX_SCHEDULE_TIMEOUT);
	}

	bes_devel("bes2600 char device is opened\n");
	atomic_inc(&bes2600_cdev.num_proc);

        return 0;
}

static ssize_t bes2600_chrdev_read(struct file *file, char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	char buf[64] = {0};
	unsigned int len;
	long status = 0;

	switch (bes2600_cdev.read_flag) {
	case BES_CDEV_READ_WAKEUP_STATE:
		if (bes2600_chrdev_wakeup_by_event_get() > WAKEUP_EVENT_NONE) {
			status = wait_event_timeout(bes2600_cdev.wakeup_reason_wq,
				bes2600_chrdev_wakeup_by_event_get() ==  WAKEUP_EVENT_NONE, HZ * 2);
			WARN_ON(status <= 0);
		}
		len = sprintf(buf, "wakeup_reason: %u, src_port: %u\n",
		              bes2600_cdev.wakeup_state, bes2600_cdev.src_port);
		break;
	default:
		len = sprintf(buf, "dpd_calied:%d wifi_opened:%d bt_opened:%d fw_type:%d\n",
				bes2600_cdev.dpd_calied,
				bes2600_cdev.wifi_opened,
				bes2600_cdev.bt_opened,
				bes2600_cdev.fw_type);
		break;
	}

	len = sizeof(buf);
	/* reset read flag */
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.read_flag = BES_CDEV_READ_NUM_MAX;
	spin_unlock(&bes2600_cdev.status_lock);

	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t bes2600_chrdev_write(struct file *file,
		 const char __user *user_buf, size_t count, loff_t *ppos)
{
	int i = 0;
	int cmd_num = ARRAY_SIZE(bes2600_op_map_tab);
	int cmd_len = 0;
	int ret = 0;
	char *info[2] = {0};
	char *buf = NULL;

	/* copy content from user space to kernel */
	/* message format:"ifname:wlanx cmd:xxx arg1 arg2 ..." */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	/* add terminal character */
	buf[count] = '\0';

	/* extract comand and interface */
	if (bes2600_get_cmd_and_ifname(buf, info) != 0) {
		bes_err("%s get command fail, the origin string is %s\n", __func__, buf);
		kfree(buf);
		return -EINVAL;
	}

	/* match operation item and execure its handler */
	cmd_len = strlen(info[1]);
	for (i = 0; i < cmd_num; i++) {
		if (cmd_len < bes2600_op_map_tab[i].op_len)
			continue;

		if (strncasecmp(info[1], bes2600_op_map_tab[i].op, bes2600_op_map_tab[i].op_len) == 0) {
			ret = bes2600_op_map_tab[i].handler(buf);
			break;
		}
	}

	/* operation item mismatch */
	if (i == cmd_num) {
		bes_err("cmd(%s) mismatch\n", info[1]);
	}

	bes2600_recyle_cmd_and_ifname_mem(info);
	kfree(buf);

	return (ret == 0) ? count : ret;
}

static int bes2600_chrdev_release (struct inode *inode, struct file *file)
{
	if (atomic_dec_and_test(&bes2600_cdev.num_proc)) {
		wake_up(&bes2600_cdev.open_wq);
	}

	bes_devel("bes2600 char device is closed\n");

	return 0;
}

static struct file_operations bes2600_chardev_fops =
{
	.owner = THIS_MODULE,
	.open = bes2600_chrdev_open,
	.read = bes2600_chrdev_read,
	.write = bes2600_chrdev_write,
	.release = bes2600_chrdev_release,
};

#ifdef BES2600_WRITE_DPD_TO_FILE
static int bes2600_chrdev_write_dpd_data_to_file(const char *path, void *buffer, int size)
{
	int ret = 0;
	struct file *fp;

	if (buffer == NULL || size == 0)
		return 0;

	fp = filp_open(path, O_TRUNC | O_CREAT | O_RDWR, S_IRUSR);
	if (IS_ERR(fp)) {
		bes_err("BES2600 : can't open %s\n",path);
		return -1;
	}

	ret = kernel_write(fp, buffer, size, &fp->f_pos);
	if (ret < 0)
		bes_err("write dpd to file failed\n");

	filp_close(fp,NULL);

	bes_devel("write dpd to %s\n", path);

	return ret;
}

static bool bes2600_chrdev_dpd_is_vaild(u8 *dpd_data)
{
	u32 cal_crc = 0;
	u32 dpd_crc = le32_to_cpup((__le32 *)(dpd_data));
	u32 dpd_ver = le32_to_cpup((__le32 *)(dpd_data + DPD_VERSION_OFFSET));

	/* check version */
	if (dpd_ver < DPD_CUR_VERSION)
		return false;

	cal_crc ^= 0xffffffffL;
	cal_crc = crc32_le(cal_crc, dpd_data + 4, DPD_BIN_SIZE - 4);
	cal_crc ^= 0xffffffffL;

	/* check if the dpd data is valid */
	if (cal_crc != dpd_crc) {
		bes_err(
			"bes2600 dpd data from file check failed, calc_crc:0x%08x dpd_crc: 0x%08x\n",
			cal_crc, dpd_crc);
		return false;
	}

	return true;
}

static int bes2600_chrdev_read_and_check_dpd_data(const char *file, u8 **data, u32 *len)
{
	int ret = 0;
	u8* read_data = NULL;
	struct file *fp;

	/* open file */
	fp = filp_open(file, O_RDONLY, 0);//S_IRUSR
	if (IS_ERR(fp)) {
		bes_devel("BES2600 : can't open %s\n",file);
		return -1;
	}

#ifdef BES2600_WRITE_DPD_TO_FILE
	if (fp->f_inode->i_size != DPD_BIN_FILE_SIZE) {
		bes_err(
			"bes2600 dpd data file size check failed, read_size: %lld file_size: %d\n",
			fp->f_inode->i_size, DPD_BIN_FILE_SIZE);
		filp_close(fp, NULL);
		return -1;
	}
#endif

	/* allocate memory for storing reading data */
	read_data = kmalloc(fp->f_inode->i_size, GFP_KERNEL);
	if (read_data == NULL) {
		bes_devel("%s alloc mem fail\n", __func__);
		goto err1;
	}

	/* read data  from file */
	ret = kernel_read(fp, read_data, fp->f_inode->i_size, &fp->f_pos);
	if (ret < DPD_BIN_SIZE) {
		bes_err("%s read fail, ret=%d\n", __func__, ret);
		goto err2;
	}

	/* check dpd version and crc */
	if (!bes2600_chrdev_dpd_is_vaild(read_data))
		goto err2;

	/* close file */
	filp_close(fp, NULL);

	/* copy data to external */
	*data = read_data;
	*len = DPD_BIN_SIZE;;

	/* output debug information */
	bes_devel("read dpd data from %s\n", file);

	return 0;

err2:
	kfree(read_data);
err1:
	filp_close(fp, NULL);
	*data = NULL;
	*len = 0;
	return -1;
}
#endif

const u8* bes2600_chrdev_get_dpd_data(u32 *len)
{
#ifdef BES2600_WRITE_DPD_TO_FILE
	if (!bes2600_cdev.dpd_calied && bes2600_cdev.no_dpd) {
		/* read dpd data from file that stores factory dpd calibration data */
		if ((bes2600_chrdev_read_and_check_dpd_data(BES2600_DPD_GOLDEN_PATH,
		   	&bes2600_cdev.dpd_data, &bes2600_cdev.dpd_len) < 0) &&
		   (bes2600_chrdev_read_and_check_dpd_data(BES2600_DEFAULT_DPD_PATH,
			&bes2600_cdev.dpd_data, &bes2600_cdev.dpd_len) < 0)) {
			bes_err("%s read dpd data fail\n", __func__);
			return NULL;
		} else {
			bes2600_cdev.dpd_calied = true;
		}
	}
#endif

	if (!bes2600_cdev.dpd_calied)
		return NULL;
	if (len)
		*len = bes2600_cdev.dpd_len;

	return bes2600_cdev.dpd_data;
}

u8* bes2600_chrdev_get_dpd_buffer(u32 size)
{
	if (bes2600_cdev.dpd_data)
		kfree(bes2600_cdev.dpd_data);

	bes2600_cdev.dpd_data = kmalloc(size, GFP_KERNEL);
	if (!bes2600_cdev.dpd_data) {
		return NULL;
	}

	bes2600_cdev.dpd_len = DPD_BIN_SIZE;

	return bes2600_cdev.dpd_data;
}

void bes2600_chrdev_free_dpd_data(void)
{
	if (bes2600_cdev.dpd_data)
		kfree(bes2600_cdev.dpd_data);

	bes2600_cdev.dpd_data = NULL;
	bes2600_cdev.dpd_len = 0;
}

int bes2600_chrdev_update_dpd_data(void)
{
	u32 cal_crc = 0;
	u32 dpd_crc = le32_to_cpup((__le32 *)(bes2600_cdev.dpd_data));

	/* check if the dpd data is valid */
	cal_crc ^= 0xffffffffL;
	cal_crc = crc32_le(cal_crc, bes2600_cdev.dpd_data + 4, bes2600_cdev.dpd_len - 4);
	cal_crc ^= 0xffffffffL;
	if (cal_crc != dpd_crc) {
		bes_err(
			"bes2600 dpd data check failed, calc_crc:0x%08x dpd_crc: 0x%08x\n",
			cal_crc, dpd_crc);
		return -1;
	}

	bes_devel("bes2600 dpd cali pass.\n");

	/* update dpd calibration and wait state */
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.dpd_calied = true;
	if (bes2600_chrdev_check_system_close_internal()) {
		bes2600_cdev.wait_state = BES2600_BOOT_WAIT_CLOSE;
	} else {
		bes2600_cdev.wait_state = BES2600_BOOT_WAIT_PROBE_DONE;
	}
	spin_unlock(&bes2600_cdev.status_lock);

#ifdef BES2600_WRITE_DPD_TO_FILE
	/* write dpd data to file */
	memset(bes2600_cdev.dpd_data + DPD_BIN_SIZE, 0, DPD_BIN_FILE_SIZE - DPD_BIN_SIZE);
	bes2600_chrdev_write_dpd_data_to_file(BES2600_DPD_PATH,
		bes2600_cdev.dpd_data, DPD_BIN_FILE_SIZE);
#endif


	return 0;
}

#ifdef BES2600_DUMP_FW_DPD_LOG
void bes2600_free_dpd_log_buffer(void)
{
	if (bes2600_cdev.dpd_log)
		kfree(bes2600_cdev.dpd_log);

	bes2600_cdev.dpd_log_len = 0;
	bes2600_cdev.dpd_log = NULL;
}

u8* bes2600_alloc_dpd_log_buffer(u16 len)
{
	bes2600_cdev.dpd_log_len = len;

	if (bes2600_cdev.dpd_log)
		kfree(bes2600_cdev.dpd_log);

	bes2600_cdev.dpd_log = kzalloc(len, GFP_KERNEL);
	if (!bes2600_cdev.dpd_log) {
		bes2600_cdev.dpd_log_len = 0;
		return NULL;
	}

	return bes2600_cdev.dpd_log;
}

void bes2600_get_dpd_log(char **data, size_t *len)
{
	if (!bes2600_cdev.dpd_log) {
		*data = NULL;
		*len = 0;
	} else {
		*data = bes2600_cdev.dpd_log;
		*len = (size_t)bes2600_cdev.dpd_log_len;
	}
}
#endif /* BES2600_DUMP_FW_DPD_LOG */

void bes2600_chrdev_set_sbus_priv_data(struct sbus_priv *priv, bool error)
{
	bes2600_cdev.sbus_priv = priv;
	if (priv) {
		if (bes2600_cdev.bton_pending) {
			bes_devel("execute pending bt on operation.\n");
			bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_ON, SUBSYSTEM_BT, true);

			bes2600_cdev.bton_pending = false;
		}

		spin_lock(&bes2600_cdev.status_lock);
		if (bes2600_cdev.wait_state == BES2600_BOOT_WAIT_PROBE_DONE) {
			bes2600_cdev.wait_state = BES2600_BOOT_WAIT_NONE;
		}
		spin_unlock(&bes2600_cdev.status_lock);

		bes_devel("wakup proc on wq of probe_done.\n");
	} else {
		spin_lock(&bes2600_cdev.status_lock);
		bes2600_cdev.wait_state = BES2600_BOOT_WAIT_NONE;
		bes2600_cdev.bus_error = error;
		if (bes2600_cdev.bus_error) {
			bes2600_cdev.wifi_opened = false;
			bes2600_cdev.bt_opened = false;
			bes2600_cdev.bton_pending = false;
			bes2600_cdev.bus_probe = BES2600_BUS_PROBE_NONE;
		}
		spin_unlock(&bes2600_cdev.status_lock);
		bes_devel("wakup proc on wq of disconnect_done.\n");
	}

	wake_up(&bes2600_cdev.probe_done_wq);
}

struct sbus_priv * bes2600_chrdev_get_sbus_priv_data(void)
{
	return bes2600_cdev.sbus_priv;
}

int bes2600_chrdev_check_system_close(void)
{
	bool sys_closed = false;

	spin_lock(&bes2600_cdev.status_lock);
	sys_closed = bes2600_chrdev_check_system_close_internal();
	spin_unlock(&bes2600_cdev.status_lock);

	return sys_closed;
}

int bes2600_chrdev_do_system_close(const struct sbus_ops *sbus_ops, struct sbus_priv *priv)
{
	int ret = 0;
	long status = 0;

	if (!sbus_ops || !priv) {
		bes_warn("abort power down device.\n");
		return 0;
	}

	if (!sbus_ops->power_switch)
		return 0;

	bes_devel("power down bes2600.\n");

	/* trigger system to execute disconnect function */
	ret = sbus_ops->power_switch(priv, 0);

	/* wait disconnect event */
	status = wait_event_timeout(bes2600_cdev.probe_done_wq, (bes2600_cdev.sbus_priv == NULL), HZ * 3);
	WARN_ON(status <= 0);

	return ret;
}

bool bes2600_chrdev_is_wifi_opened(void)
{
	bool wifi_opened = false;

	spin_lock(&bes2600_cdev.status_lock);
	wifi_opened = bes2600_cdev.wifi_opened;
	spin_unlock(&bes2600_cdev.status_lock);

	if (bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_NO_SIGNAL)
		return true;
	else if (bes2600_cdev.fw_type == BES2600_FW_TYPE_BT)
		return false;
	else if (bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_SIGNAL)
		return wifi_opened;

	return false;
}

bool bes2600_chrdev_is_bt_opened(void)
{
	bool bt_opened = false;

	spin_lock(&bes2600_cdev.status_lock);
	bt_opened = bes2600_cdev.bt_opened;
	spin_unlock(&bes2600_cdev.status_lock);

	if (bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_NO_SIGNAL)
		return false;
	else if (bes2600_cdev.fw_type == BES2600_FW_TYPE_BT)
		return true;
	else if (bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_SIGNAL)
		return bt_opened;

	return false;
}

void bes2600_chrdev_wakeup_bt(void)
{
	int ret = 0;

	if (bes2600_cdev.bt_opened && bes2600_cdev.sbus_priv) {
		bes_devel("wakeup bt in resume flow\n");
		ret = bes2600_chrdev_switch_subsys(GPIO_WAKE_FLAG_BT_LP_ON, SUBSYSTEM_BT_LP, true);

		if (ret)
			bes_err("Wakeup BT fail in resume\n");
	}
}

int bes2600_chrdev_get_fw_type(void)
{
	return bes2600_cdev.fw_type;
}

bool bes2600_chrdev_is_signal_mode(void)
{
	return bes2600_cdev.sig_mode;
}

bool bes2600_chrdev_is_bus_error(void)
{
	bool error = false;

	spin_lock(&bes2600_cdev.status_lock);
	error = (bes2600_cdev.bus_error || bes2600_cdev.bus_probe != BES2600_BUS_PROBE_OK);
	spin_unlock(&bes2600_cdev.status_lock);

	return error;
}

void bes2600_chrdev_update_signal_mode(void)
{
	if (bes2600_cdev.fw_type >= BES2600_FW_TYPE_MAX_NUM) {
		bes2600_cdev.fw_type = BES2600_FW_TYPE_WIFI_SIGNAL;
		bes_warn("unexpected fw type, switch to wifi signal mode\n");
	}

	if (bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_SIGNAL) {
		bes2600_cdev.sig_mode = true;
	} else if ((bes2600_cdev.fw_type == BES2600_FW_TYPE_WIFI_NO_SIGNAL)
		|| (bes2600_cdev.fw_type == BES2600_FW_TYPE_BT)) {
		bes2600_cdev.sig_mode = false;
	}
}

static void bes2600_chrdev_wifi_force_close_work(struct work_struct *work)
{
	char wifi_state[15];
	char bt_state[15];
	char fw_type[15];
	char *env[] = { wifi_state, bt_state, fw_type, NULL };
	int ret;

	if (bes2600_chrdev_is_wifi_opened()) {
		bes_devel("system exeception, force wifi down\n");

		/* halt device if needed */
		if (bes2600_cdev.halt_dev && bes2600_cdev.sbus_ops->halt_device) {
			bes2600_cdev.sbus_ops->halt_device(bes2600_cdev.sbus_priv);
		}

		/* unregister wifi */
		bes2600_switch_wifi(0);

		/* power down device if wifi is only opened */
		if (bes2600_chrdev_check_system_close()) {
			bes2600_chrdev_do_system_close(bes2600_cdev.sbus_ops,
						bes2600_cdev.sbus_priv);
		}

		/* notify userspace */
		snprintf(wifi_state, sizeof(wifi_state), "WIFI_OPENED=%d", bes2600_cdev.wifi_opened);
		snprintf(bt_state, sizeof(bt_state), "BT_OPENED=%d", bes2600_cdev.bt_opened);
		snprintf(fw_type, sizeof(fw_type), "FW_TYPE=%d", bes2600_cdev.fw_type);
		ret = kobject_uevent_env(&bes2600_cdev.device->kobj, KOBJ_CHANGE, env);
		if (!ret)
			bes_err("bes2600 notify userspace failed\n");
	}
}

void bes2600_chrdev_wifi_force_close(struct bes2600_common *hw_priv, bool halt_dev)
{
	if (hw_priv == NULL)
		return;

	if (bes2600_chrdev_is_wifi_opened() &&
	   !work_pending(&bes2600_cdev.wifi_force_close_work)) {
		spin_lock(&bes2600_cdev.status_lock);
		bes2600_cdev.bus_error = true;
		bes2600_cdev.halt_dev = halt_dev;
		spin_unlock(&bes2600_cdev.status_lock);

		bes2600_tx_loop_set_enable(hw_priv, true);
		schedule_work(&bes2600_cdev.wifi_force_close_work);
	}
}

void bes2600_chrdev_usb_remove(struct bes2600_common *hw_priv)
{
	if (hw_priv == NULL)
		return;

	if (bes2600_chrdev_is_wifi_opened() &&
	   !work_pending(&bes2600_cdev.wifi_force_close_work)) {
		spin_lock(&bes2600_cdev.status_lock);
		bes2600_cdev.bus_error = true;
		spin_unlock(&bes2600_cdev.status_lock);

		bes2600_tx_loop_set_enable(hw_priv, false);
		bes2600_chrdev_wifi_force_close_work(&bes2600_cdev.wifi_force_close_work);
	}
}

static void bes2600_probe_timeout_work(struct work_struct *work)
{
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.bus_probe = BES2600_BUS_PROBE_TIMEOUT;
	bes2600_cdev.wifi_opened = false;
	bes2600_cdev.bt_opened = false;
	bes2600_cdev.bton_pending = false;
	spin_unlock(&bes2600_cdev.status_lock);

	bes_devel("bus probe timeout\n");
	wake_up(&bes2600_cdev.probe_done_wq);
}

void bes2600_chrdev_start_bus_probe(void)
{
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.bus_probe = BES2600_BUS_PROBE_START;
	spin_unlock(&bes2600_cdev.status_lock);

	cancel_delayed_work_sync(&bes2600_cdev.probe_timeout_work);
	schedule_delayed_work(&bes2600_cdev.probe_timeout_work, 3 * HZ);
}

void bes2600_chrdev_bus_probe_notify(void)
{
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.bus_probe = BES2600_BUS_PROBE_OK;
	spin_unlock(&bes2600_cdev.status_lock);
	cancel_delayed_work_sync(&bes2600_cdev.probe_timeout_work);

	wake_up(&bes2600_cdev.probe_done_wq);
}

void bes2600_chrdev_wifi_update_wakeup_reason(u16 reason, u16 port)
{
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.wakeup_state = reason;
	bes2600_cdev.src_port = port;
	spin_unlock(&bes2600_cdev.status_lock);
}

void bes2600_chrdev_wakeup_by_event_set(enum wakeup_event wakeup_event)
{
	spin_lock(&bes2600_cdev.status_lock);
	bes2600_cdev.wakeup_by_event = wakeup_event;
	spin_unlock(&bes2600_cdev.status_lock);
	if (wakeup_event == WAKEUP_EVENT_NONE)
		wake_up(&bes2600_cdev.wakeup_reason_wq);
}

int bes2600_chrdev_wakeup_by_event_get(void)
{
	return bes2600_cdev.wakeup_by_event;
}

int bes2600_chrdev_init(struct sbus_ops *ops)
{
	int ret = 0;

	/* allocate devide id */
	ret = alloc_chrdev_region(&bes2600_cdev.dev_id, 0, 1, "bes2600_chrdev");
	if (ret < 0){
		bes_err("bes2600 alloc device id fail\n");
		ret =  -EFAULT;
		goto fail;
	}

	/* extract major and minor device id */
	bes2600_cdev.major = MAJOR(bes2600_cdev.dev_id);
	bes2600_cdev.minor = MINOR(bes2600_cdev.dev_id);

	/* add char device and bind operation function */
	bes2600_cdev.cdev.owner = THIS_MODULE;
	cdev_init(&bes2600_cdev.cdev, &bes2600_chardev_fops);
	ret = cdev_add(&bes2600_cdev.cdev, bes2600_cdev.dev_id, 1);
	if (ret < 0){
		bes_err("bes2600 char device add fail\n");
		ret =  -EFAULT;
		goto fail1;
	}

	/* create class for creating device node */
	bes2600_cdev.class = class_create("bes2600_chrdev");
	if (IS_ERR(bes2600_cdev.class)){
		bes_err("bes2600 char device add fail\n");
		ret = -EFAULT;
		goto fail2;
	}

	/* get char device pointer */
	bes2600_cdev.device = device_create(bes2600_cdev.class, NULL, bes2600_cdev.dev_id, NULL, "bes2600");
	if (IS_ERR(bes2600_cdev.device)){
		bes_err("bes2600 char device create fail\n");
		ret =  -EFAULT;
		goto fail3;
	}

	/* initialise global variable */
	atomic_set(&bes2600_cdev.num_proc, 0);
	init_waitqueue_head(&bes2600_cdev.open_wq);
	spin_lock_init(&bes2600_cdev.status_lock);
	init_waitqueue_head(&bes2600_cdev.probe_done_wq);
	INIT_WORK(&bes2600_cdev.wifi_force_close_work, bes2600_chrdev_wifi_force_close_work);
	INIT_DELAYED_WORK(&bes2600_cdev.probe_timeout_work, bes2600_probe_timeout_work);
	init_waitqueue_head(&bes2600_cdev.wakeup_reason_wq);
	bes2600_chrdev_wakeup_by_event_set(WAKEUP_EVENT_NONE);
#ifdef CONFIG_BES2600_WIFI_BOOT_ON
	bes2600_cdev.wifi_opened = true;
#else
	bes2600_cdev.wifi_opened = false;
#endif
#ifdef CONFIG_BES2600_BT_BOOT_ON
	bes2600_cdev.bt_opened = true;
	bes2600_cdev.bton_pending = true;
#else
	bes2600_cdev.bt_opened = false;
	bes2600_cdev.bton_pending = false;
#endif
	bes2600_cdev.dpd_calied = false;
	bes2600_cdev.wait_state = BES2600_BOOT_WAIT_NONE;
	bes2600_cdev.sbus_ops = ops;
	bes2600_cdev.bus_error = false;
	bes2600_cdev.halt_dev = false;
	bes2600_cdev.read_flag = BES_CDEV_READ_NUM_MAX;
	bes2600_cdev.wakeup_by_event = WAKEUP_EVENT_NONE;
	bes_devel("%s done\n", __func__);

	return 0;

fail3:
	class_destroy(bes2600_cdev.class);
fail2:
	cdev_del(&bes2600_cdev.cdev);
fail1:
	unregister_chrdev_region(bes2600_cdev.dev_id, 1);
fail:
	return ret;
}

void bes2600_chrdev_free(void)
{
	cancel_delayed_work_sync(&bes2600_cdev.probe_timeout_work);
#ifdef BES2600_DUMP_FW_DPD_LOG
	bes2600_free_dpd_log_buffer();
#endif
	bes2600_chrdev_free_dpd_data();
	cdev_del(&bes2600_cdev.cdev);
	unregister_chrdev_region(bes2600_cdev.dev_id, 1);
	device_destroy(bes2600_cdev.class, bes2600_cdev.dev_id);
	class_destroy(bes2600_cdev.class);
	bes_devel("%s done\n", __func__);
}
