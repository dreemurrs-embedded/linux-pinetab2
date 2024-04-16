/*
 * Mac80211 driver for BES2600 device
 *
 * Copyright (c) 2010, Bestechnic
 * Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef BES2600_PLAT_H_INCLUDED
#define BES2600_PLAT_H_INCLUDED

#include <linux/ioport.h>

struct bes2600_platform_data_sdio {
	u16 ref_clk;                    /* REQUIRED (in KHz) */

	/* All others are optional */
	bool have_5ghz;
	bool no_nptb;       /* SDIO hardware does not support non-power-of-2-blocksizes */
	struct gpio_desc *reset;          /* GPIO to RSTn signal (0 disables) */
	struct gpio_desc *powerup;        /* GPIO to POWERUP signal (0 disables) */
	struct gpio_desc *wakeup;         /* GPIO to WAKEUP signal (0 disables) */
	struct gpio_desc *host_wakeup;    /* wifi GPIO to WAKEUP host signal (0 disables) */
	bool wlan_bt_hostwake_registered;/* wifi request_irq success or not */
	struct gpio_desc *gpio_irq;       /* IRQ line or 0 to use SDIO IRQ */
	int (*power_ctrl)(const struct bes2600_platform_data_sdio *pdata,
			  bool enable); /* Control 3v3 / 1v8 supply */
	int (*clk_ctrl)(const struct bes2600_platform_data_sdio *pdata,
			bool enable); /* Control CLK32K */
	const u8 *macaddr;  /* if NULL, use bes2600_mac_template module parameter */
	const char *sdd_file;  /* if NULL, will use default for detected hw type */
	bool wakeup_source; /* marks whether bes2600 is the wakeup souce or not */
	bool inited; /* platform data init flag */
};

#endif /* BES2600_PLAT_H_INCLUDED */
