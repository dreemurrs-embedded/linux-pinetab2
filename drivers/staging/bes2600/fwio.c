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

#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/firmware.h>

#include "bes2600.h"
#include "fwio.h"
#include "hwio.h"
#include "sbus.h"
#include "bh.h"

extern int bes2600_load_firmware_sdio(struct sbus_ops *ops, struct sbus_priv *priv);

int bes2600_load_firmware(struct sbus_ops *ops, struct sbus_priv *priv)
{
	return bes2600_load_firmware_sdio(ops, priv);
}
