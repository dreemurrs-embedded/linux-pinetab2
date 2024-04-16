/*
 * Mac80211 power management API for BES2600 drivers
 *
 * Copyright (c) 2011, Bestechnic
 * Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/if_ether.h>
#include <linux/suspend.h>
#include "bes2600.h"
#include "pm.h"
#include "sta.h"
#include "bh.h"
#include "sbus.h"
#include "bes_chardev.h"
#include "bes_log.h"

#define BES2600_BEACON_SKIPPING_MULTIPLIER 3

struct bes2600_udp_port_filter {
	struct wsm_udp_port_filter_hdr hdr;
	struct wsm_udp_port_filter dhcp;
	struct wsm_udp_port_filter upnp;
	struct wsm_udp_port_filter mdns;
} __packed;

struct bes2600_ether_type_filter {
	struct wsm_ether_type_filter_hdr hdr;
	struct wsm_ether_type_filter pae;
	struct wsm_ether_type_filter wapi;
	struct wsm_ether_type_filter append;
} __packed;

static struct bes2600_udp_port_filter bes2600_udp_port_filter_on = {
	.hdr.nrFilters = 3,
	.dhcp = {
		.filterAction = WSM_FILTER_ACTION_FILTER_OUT,
		.portType = WSM_FILTER_PORT_TYPE_DST,
		.udpPort = __cpu_to_le16(67),
	},
	.upnp = {
		.filterAction = WSM_FILTER_ACTION_FILTER_OUT,
		.portType = WSM_FILTER_PORT_TYPE_DST,
		.udpPort = __cpu_to_le16(1900),
	},
	.mdns = {
		.filterAction = WSM_FILTER_ACTION_FILTER_OUT,
		.portType = WSM_FILTER_PORT_TYPE_DST,
		.udpPort = __cpu_to_le16(5353),
	},
	/* Please add other known ports to be filtered out here and
	 * update nrFilters field in the header.
	 * Up to 4 filters are allowed. */
};

static struct wsm_udp_port_filter_hdr bes2600_udp_port_filter_off = {
	.nrFilters = 0,
};

#ifndef ETH_P_WAPI
#define ETH_P_WAPI     0x88B4
#endif

#define ETH_P_UNKNOWN 0xFFFF
static struct bes2600_ether_type_filter bes2600_ether_type_filter_on = {
	.hdr.nrFilters = 3,
	.hdr.extFlags = WSM_ETH_FILTER_EXT_DISABLE_IPV6_MATCH, // patch for disable lmac SUSPEND_MODE_IPV6_FIX
	.pae = {
		.filterAction = WSM_FILTER_ACTION_FILTER_IN,
		.etherType = __cpu_to_le16(ETH_P_PAE),
	},
	.wapi = {
		.filterAction = WSM_FILTER_ACTION_FILTER_IN,
		.etherType = __cpu_to_le16(ETH_P_WAPI),
	},
	// add for lmac ether filter strategy: If every filtermode is FilterIN, discard all the frame which is mismatched
	.append = {
		.filterAction = WSM_FILTER_ACTION_FILTER_OUT,
		.etherType = __cpu_to_le16(ETH_P_UNKNOWN),
	},
	/* Please add other known ether types to be filtered out here and
	 * update nrFilters field in the header.
	 * Up to 4 filters are allowed. */
};

static struct wsm_ether_type_filter_hdr bes2600_ether_type_filter_off = {
	.nrFilters = 0,
};

#ifdef IPV6_FILTERING
static struct wsm_ipv6_filter bes2600_ipv6_filter_on = {
	.hdr.numfilter = 1,
	.hdr.action_mode = WSM_FILTER_ACTION_FILTER_IN,
	.ipv6filter[0] = {
		.filter_mode = WSM_FILTER_ACTION_FILTER_IN,
		.address_mode = WSM_IP_DATA_FRAME_ADDRMODE_DEST,
		/* a random ipvd addr, in order to filter all ipv6 packet */
		.ipv6 = {0x01, 0x28, 0x35, 0xde, 0xbf, 0x34, 0x9d, 0x8a,
				 0x47, 0x62, 0x85, 0x69, 0x7e, 0x8c, 0x29, 0x38},
	}
};

static struct wsm_ipv6_filter bes2600_ipv6_filter_off = {
	.hdr.numfilter = 0,
	.hdr.action_mode = WSM_FILTER_ACTION_IGNORE,
};
#endif

static int __bes2600_wow_suspend(struct bes2600_vif *priv,
				struct cfg80211_wowlan *wowlan);
static int __bes2600_wow_resume(struct bes2600_vif *priv);


/* private */
struct bes2600_suspend_state {
	unsigned long bss_loss_tmo;
	unsigned long connection_loss_tmo;
	unsigned long join_tmo;
	unsigned long direct_probe;
	unsigned long link_id_gc;
};

void bes2600_suspend_status_set(struct bes2600_common *hw_priv, bool status)
{
	hw_priv->suspend_in_progress = status;
}

bool bes2600_suspend_status_get(struct bes2600_common *hw_priv)
{
	return hw_priv->suspend_in_progress;
}

void bes2600_pending_unjoin_reset(struct bes2600_common *hw_priv)
{
	hw_priv->unjoin_if_id_slots = 0x00;
}

void bes2600_pending_unjoin_set(struct bes2600_common *hw_priv, int if_id)
{
	if(if_id > 1)
		bes_err("unexpected if_id: %d\n", if_id);
	else
		hw_priv->unjoin_if_id_slots |= (1 << if_id);
}

bool bes2600_pending_unjoin_get(struct bes2600_common *hw_priv, int if_id)
{
	if(if_id > 1) {
		bes_err("unexpected if_id: %d\n", if_id);
		return false;
	} else
		return hw_priv->unjoin_if_id_slots & (1 << if_id);
}

static int bes2600_pm_notifier(struct notifier_block *notifier,
			       unsigned long pm_event,
			       void *unused)
{
	int if_id;
	struct bes2600_vif *priv;
	struct bes2600_common *hw_priv = container_of(notifier,
						    struct bes2600_common,
						    pm_notify);

	switch (pm_event) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		bes2600_suspend_status_set(hw_priv, true);
		break;

	case PM_POST_RESTORE:
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		bes2600_suspend_status_set(hw_priv, false);
		if(hw_priv->unjoin_if_id_slots) {
			for(if_id = 0; if_id < 2; if_id++) {
				if(bes2600_pending_unjoin_get(hw_priv, if_id)) {
					priv = __cw12xx_hwpriv_to_vifpriv(hw_priv, if_id);
					ieee80211_connection_loss(priv->vif);
				}
			}
			bes2600_pending_unjoin_reset(hw_priv);
		}
		break;

	case PM_RESTORE_PREPARE:
	default:
		break;
	}

	return NOTIFY_DONE;
}

void bes2600_register_pm_notifier(struct bes2600_common *hw_priv)
{
	hw_priv->pm_notify.notifier_call = bes2600_pm_notifier;
	register_pm_notifier(&hw_priv->pm_notify);
}

void bes2600_unregister_pm_notifier(struct bes2600_common *hw_priv)
{
	unregister_pm_notifier(&hw_priv->pm_notify);
}

static long bes2600_suspend_work(struct delayed_work *work)
{
	int ret = cancel_delayed_work(work);
	long tmo;
	if (ret > 0) {
		/* Timer is pending */
		tmo = work->timer.expires - jiffies;
		if (tmo < 0)
			tmo = 0;
	} else {
		tmo = -1;
	}
	return tmo;
}

static int bes2600_resume_work(struct bes2600_common *hw_priv,
			       struct delayed_work *work,
			       unsigned long tmo)
{
	if ((long)tmo < 0)
		return 1;

	return queue_delayed_work(hw_priv->workqueue, work, tmo);
}

int bes2600_can_suspend(struct bes2600_common *priv)
{
	if (atomic_read(&priv->bh_rx)) {
		wiphy_dbg(priv->hw->wiphy, "Suspend interrupted.\n");
		return 0;
	}
	return 1;
}
EXPORT_SYMBOL_GPL(bes2600_can_suspend);

int bes2600_wow_suspend(struct ieee80211_hw *hw, struct cfg80211_wowlan *wowlan)
{
	struct bes2600_common *hw_priv = hw->priv;
	struct bes2600_vif *priv;
	int i, ret = 0;
	unsigned long begin, end, diff;
	char *busy_event_buffer = NULL;

	bes_devel("bes2600_wow_suspend enter\n");

	WARN_ON(!atomic_read(&hw_priv->num_vifs));

	/* reset wakeup reason to default */
	bes2600_chrdev_wifi_update_wakeup_reason(0, 0);

#ifdef ROAM_OFFLOAD
	bes2600_for_each_vif(hw_priv, priv, i) {
#ifdef P2P_MULTIVIF
		if ((i == (CW12XX_MAX_VIFS - 1)) || !priv)
#else
		if (!priv)
#endif
			continue;
		if((priv->vif->type == NL80211_IFTYPE_STATION)
		&& (priv->join_status == BES2600_JOIN_STATUS_STA)) {
			down(&hw_priv->scan.lock);
			hw_priv->scan.if_id = priv->if_id;
			bes2600_sched_scan_work(&hw_priv->scan.swork);
		}
	}
#endif /*ROAM_OFFLOAD*/

	/* Do not suspend when datapath is not idle */
	if (hw_priv->tx_queue_stats.num_queued[0]
			+ hw_priv->tx_queue_stats.num_queued[1])
		return -EBUSY;


	/* Make sure there is no configuration requests in progress. */
	if (down_trylock(&hw_priv->conf_lock))
		return -EBUSY;

	/* Do not suspend when scanning or ROC*/
	if (down_trylock(&hw_priv->scan.lock))
		goto revert1;

	/* Do not suspend when probe is doing */
	if (delayed_work_pending(&hw_priv->scan.probe_work))
		goto revert2;

	/* record suspend start time */
	begin = jiffies;

	/* wait uitil bes2600 finish current pending operation */
	if (!bes2600_pwr_device_is_idle(hw_priv)) {
		/* clear power busy event */
		bes2600_pwr_set_busy_event_with_timeout(hw_priv, BES_PWR_LOCK_ON_TX, 10);

		/* wait device enter lp mode */
		if (wait_event_timeout(hw_priv->bes_power.dev_lp_wq,
			bes2600_pwr_device_is_idle(hw_priv), HZ * 5) <= 0) {
			bes_err("wait device idle timeout\n");
			busy_event_buffer = kmalloc(4096, GFP_KERNEL);

			if(!busy_event_buffer)
				goto revert2;

			if(bes2600_pwr_busy_event_record(hw_priv, busy_event_buffer, 4096) == 0) {
				bes_devel("%s\n", busy_event_buffer);
			} else {
				bes_err("busy event show failed\n");
			}

			kfree(busy_event_buffer);
			goto revert2;
		}
	}

	/* Lock TX. */
	wsm_lock_tx_async(hw_priv);

	/* mark suspend start to avoid device to exit ps mode when setting device */
	bes2600_pwr_suspend_start(hw_priv);

	/* set filters and offload based on interface */
	bes2600_for_each_vif(hw_priv, priv, i) {
#ifdef P2P_MULTIVIF
		if ((i == (CW12XX_MAX_VIFS - 1)) || !priv)
#else
		if (!priv)
#endif
			continue;

		ret = __bes2600_wow_suspend(priv,
						wowlan);
		if (ret) {
			for (; i >= 0; i--) {
				if (!hw_priv->vif_list[i])
					continue;
				priv = (struct bes2600_vif *)
					hw_priv->vif_list[i]->drv_priv;
				__bes2600_wow_resume(priv);
			}
			goto revert3;
		}
	}

	/* mark suspend end */
	bes2600_pwr_suspend_end(hw_priv);

	/* Stop serving thread */
	if (bes2600_bh_suspend(hw_priv)) {
		bes_err("%s: bes2600_bh_suspend failed\n",
				__func__);
		bes2600_wow_resume(hw);
		return -EBUSY;
	}

	/* Force resume if event is coming from the device. */
	if (atomic_read(&hw_priv->bh_rx)) {
		bes_devel("%s: incoming event present - resume\n",
				__func__);
		bes2600_wow_resume(hw);
		return -EAGAIN;
	}

	/* calculate the time consumed by bes2600 suspend flow */
	end = jiffies;
	diff = end - begin;
	bes_devel("%s consume %d ms\n", __func__, jiffies_to_msecs(diff));

	return 0;

revert3:
	bes2600_pwr_suspend_end(hw_priv);
	wsm_unlock_tx(hw_priv);
revert2:
	up(&hw_priv->scan.lock);
revert1:
	up(&hw_priv->conf_lock);
	return -EBUSY;
}

static void bes2600_set_ehter_and_udp_filter(struct bes2600_common *hw_priv,
				struct wsm_ether_type_filter_hdr *ether_type, struct wsm_udp_port_filter_hdr *udp_type,
				int if_id)
{
	/* Set UDP filter */
	wsm_set_udp_port_filter(hw_priv, udp_type, if_id);

	/* Set ethernet frame type filter */
	wsm_set_ether_type_filter(hw_priv, ether_type, if_id);
}

static int __bes2600_wow_suspend(struct bes2600_vif *priv,
				struct cfg80211_wowlan *wowlan)
{
	struct bes2600_common *hw_priv = cw12xx_vifpriv_to_hwpriv(priv);
	struct bes2600_pm_state_vif *pm_state_vif = &priv->pm_state_vif;
	struct bes2600_suspend_state *state;
	int ret;

#ifdef MCAST_FWDING
	struct wsm_forwarding_offload fwdoffload = {
		.fwenable = 0x1,
		.flags = 0x1,
	};
#endif

	/* Do not suspend when join work is scheduled */
	if (work_pending(&priv->join_work))
		goto revert1;

	bes2600_set_ehter_and_udp_filter(hw_priv, &bes2600_ether_type_filter_on.hdr,
				&bes2600_udp_port_filter_on.hdr, priv->if_id);

	/* Set ipv6 filer */
#ifdef IPV6_FILTERING
	wsm_set_ipv6_filter(hw_priv, &bes2600_ipv6_filter_on.hdr, priv->if_id);
#endif

  	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_keepalive_filter(priv, true));

	/* Set Multicast Address Filter */
	if (priv->multicast_filter.numOfAddresses) {
		priv->multicast_filter.enable = __cpu_to_le32(2);
		wsm_set_multicast_filter(hw_priv, &priv->multicast_filter, priv->if_id);
	}

#ifdef MCAST_FWDING
	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_forwarding_offlad(hw_priv,
				&fwdoffload,priv->if_id));
#endif

	/* Allocate state */
	state = kzalloc(sizeof(struct bes2600_suspend_state), GFP_KERNEL);
	if (!state)
		goto revert2;

	/* Store delayed work states. */
	state->bss_loss_tmo =
		bes2600_suspend_work(&priv->bss_loss_work);
	state->connection_loss_tmo =
		bes2600_suspend_work(&priv->connection_loss_work);
	state->join_tmo =
		bes2600_suspend_work(&priv->join_timeout);
	state->link_id_gc =
		bes2600_suspend_work(&priv->link_id_gc_work);

	ret = timer_pending(&priv->mcast_timeout);
	if (ret)
		goto revert3;

	/* Store suspend state */
	pm_state_vif->suspend_state = state;

	return 0;

revert3:
	bes2600_resume_work(hw_priv, &priv->bss_loss_work,
			state->bss_loss_tmo);
	bes2600_resume_work(hw_priv, &priv->connection_loss_work,
			state->connection_loss_tmo);
	bes2600_resume_work(hw_priv, &priv->join_timeout,
			state->join_tmo);
	bes2600_resume_work(hw_priv, &priv->link_id_gc_work,
			state->link_id_gc);
	kfree(state);
revert2:
	wsm_set_udp_port_filter(hw_priv, &bes2600_udp_port_filter_off,
				priv->if_id);
	wsm_set_ether_type_filter(hw_priv, &bes2600_ether_type_filter_off,
				  priv->if_id);

	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_keepalive_filter(priv, false));

	/* Set Multicast Address Filter */
	if (priv->multicast_filter.numOfAddresses) {
		priv->multicast_filter.enable = __cpu_to_le32(1);
		wsm_set_multicast_filter(hw_priv, &priv->multicast_filter, priv->if_id);
	}


#ifdef MCAST_FWDING
	fwdoffload.flags = 0x0;
	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_forwarding_offlad(hw_priv, &fwdoffload,priv->if_id));
#endif
revert1:
	up(&hw_priv->conf_lock);
	return -EBUSY;
}

int bes2600_wow_resume(struct ieee80211_hw *hw)
{
	struct bes2600_common *hw_priv = hw->priv;
	struct bes2600_vif *priv;
	int i, ret = 0;

	bes_devel("bes2600_wow_resume enter\n");
	WARN_ON(!atomic_read(&hw_priv->num_vifs));

	up(&hw_priv->scan.lock);

	/* Resume BH thread */
	WARN_ON(bes2600_bh_resume(hw_priv));

	/* mark resume start to avoid device to exit ps mode when setting device */
	bes2600_pwr_resume_start(hw_priv);

	/* set filters and offload based on interface */
	bes2600_for_each_vif(hw_priv, priv, i) {
#ifdef P2P_MULTIVIF
		if ((i == (CW12XX_MAX_VIFS - 1)) || !priv)
#else
		if (!priv)
#endif
			continue;
		ret = __bes2600_wow_resume(priv);
		if (ret)
			break;
	}

	/* mark resume end */
	bes2600_pwr_resume_end(hw_priv);

	wsm_unlock_tx(hw_priv);
	/* Unlock configuration mutex */
	up(&hw_priv->conf_lock);

	return ret;
}

static int __bes2600_wow_resume(struct bes2600_vif *priv)
{
	struct bes2600_common *hw_priv = cw12xx_vifpriv_to_hwpriv(priv);
	struct bes2600_pm_state_vif *pm_state_vif = &priv->pm_state_vif;
	struct bes2600_suspend_state *state;

#ifdef MCAST_FWDING
	struct wsm_forwarding_offload fwdoffload = {
		.fwenable = 0x1,
		.flags = 0x0,
	};
#endif
	state = pm_state_vif->suspend_state;
	pm_state_vif->suspend_state = NULL;

#ifdef ROAM_OFFLOAD
	if((priv->vif->type == NL80211_IFTYPE_STATION)
	&& (priv->join_status == BES2600_JOIN_STATUS_STA))
		bes2600_hw_sched_scan_stop(hw_priv);
#endif /*ROAM_OFFLOAD*/

	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_keepalive_filter(priv, false));

	/* Set Multicast Address Filter */
	if (priv->multicast_filter.numOfAddresses) {
		priv->multicast_filter.enable = __cpu_to_le32(1);
		wsm_set_multicast_filter(hw_priv, &priv->multicast_filter, priv->if_id);
	}

#ifdef MCAST_FWDING
	if (priv->join_status == BES2600_JOIN_STATUS_AP)
		WARN_ON(wsm_set_forwarding_offlad(hw_priv, &fwdoffload,priv->if_id));
#endif

	/* Resume delayed work */
	bes2600_resume_work(hw_priv, &priv->bss_loss_work,
			state->bss_loss_tmo);
	bes2600_resume_work(hw_priv, &priv->connection_loss_work,
			state->connection_loss_tmo);
	bes2600_resume_work(hw_priv, &priv->join_timeout,
			state->join_tmo);
	bes2600_resume_work(hw_priv, &priv->link_id_gc_work,
			state->link_id_gc);

	/* Remove UDP port filter */
	wsm_set_udp_port_filter(hw_priv, &bes2600_udp_port_filter_off,
				priv->if_id);

	/* Remove ethernet frame type filter */
	wsm_set_ether_type_filter(hw_priv, &bes2600_ether_type_filter_off,
				  priv->if_id);

	/* Remove ipv6 filer */
#ifdef IPV6_FILTERING
	wsm_set_ipv6_filter(hw_priv, &bes2600_ipv6_filter_off.hdr, priv->if_id);
#endif

	/* Free memory */
	kfree(state);

	return 0;
}
