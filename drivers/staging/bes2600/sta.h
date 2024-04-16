/*
 * Mac80211 STA interface for BES2600 mac80211 drivers
 *
 * Copyright (c) 2010, Bestechnic
 * Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/version.h>
#ifndef STA_H_INCLUDED
#define STA_H_INCLUDED

/* ******************************************************************** */
/* mac80211 API								*/

int bes2600_start(struct ieee80211_hw *dev);
void bes2600_stop(struct ieee80211_hw *dev);
int bes2600_add_interface(struct ieee80211_hw *dev,
			 struct ieee80211_vif *vif);
void bes2600_remove_interface(struct ieee80211_hw *dev,
			     struct ieee80211_vif *vif);
int bes2600_change_interface(struct ieee80211_hw *dev,
				struct ieee80211_vif *vif,
				enum nl80211_iftype new_type,
				bool p2p);

int bes2600_config(struct ieee80211_hw *dev, u32 changed);
int bes2600_change_interface(struct ieee80211_hw *dev,
                                struct ieee80211_vif *vif,
                                enum nl80211_iftype new_type,
                                bool p2p);
void bes2600_configure_filter(struct ieee80211_hw *dev,
			     unsigned int changed_flags,
			     unsigned int *total_flags,
			     u64 multicast);
int bes2600_conf_tx(struct ieee80211_hw *dev, struct ieee80211_vif *vif,
		unsigned int link_id,
		u16 queue, const struct ieee80211_tx_queue_params *params);
int bes2600_get_stats(struct ieee80211_hw *dev,
		     struct ieee80211_low_level_stats *stats);
/* Not more a part of interface?
int bes2600_get_tx_stats(struct ieee80211_hw *dev,
			struct ieee80211_tx_queue_stats *stats);
*/
int bes2600_set_key(struct ieee80211_hw *dev, enum set_key_cmd cmd,
		   struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		   struct ieee80211_key_conf *key);

int bes2600_set_rts_threshold(struct ieee80211_hw *hw, u32 value);

void bes2600_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  u32 queues, bool drop);

int bes2600_remain_on_channel(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_channel *chan,
				 int duration,
				 enum ieee80211_roc_type type);

int bes2600_cancel_remain_on_channel(struct ieee80211_hw *hw
	, struct ieee80211_vif *vif
	);

int bes2600_set_arpreply(struct ieee80211_hw *hw, struct ieee80211_vif *vif);

u64 bes2600_prepare_multicast(struct ieee80211_hw *hw,
			     struct netdev_hw_addr_list *mc_list);

int bes2600_set_pm(struct bes2600_vif *priv, const struct wsm_set_pm *arg);

void bes2600_set_data_filter(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   void *data,
			   int len);

u32 bes2600_bh_get_encry_hdr_len(u32 cipherType);
/* ******************************************************************** */
/* WSM callbacks							*/

/* void bes2600_set_pm_complete_cb(struct bes2600_common *hw_priv,
	struct wsm_set_pm_complete *arg); */
void bes2600_channel_switch_cb(struct bes2600_common *hw_priv);

/* ******************************************************************** */
/* WSM events								*/

void bes2600_free_event_queue(struct bes2600_common *hw_priv);
void bes2600_event_handler(struct work_struct *work);
void bes2600_bss_loss_work(struct work_struct *work);
void bes2600_connection_loss_work(struct work_struct *work);
void bes2600_keep_alive_work(struct work_struct *work);
void bes2600_tx_failure_work(struct work_struct *work);
void bes2600_dynamic_opt_txrx_work(struct work_struct *work);

/* ******************************************************************** */
/* Internal API								*/

int bes2600_setup_mac(struct bes2600_common *hw_priv);
void bes2600_join_work(struct work_struct *work);
void bes2600_join_timeout(struct work_struct *work);
void bes2600_unjoin_work(struct work_struct *work);
void bes2600_offchannel_work(struct work_struct *work);
void bes2600_wep_key_work(struct work_struct *work);
void bes2600_update_filtering(struct bes2600_vif *priv);
void bes2600_update_filtering_work(struct work_struct *work);
int __bes2600_flush(struct bes2600_common *hw_priv, bool drop, int if_id);
void bes2600_set_beacon_wakeup_period_work(struct work_struct *work);
int bes2600_enable_listening(struct bes2600_vif *priv,
			struct ieee80211_channel *chan);
int bes2600_disable_listening(struct bes2600_vif *priv);
int bes2600_set_uapsd_param(struct bes2600_vif *priv,
				const struct wsm_edca_params *arg);
void bes2600_ba_work(struct work_struct *work);
void bes2600_ba_timer(struct timer_list *t);
const u8 *bes2600_get_ie(u8 *start, size_t len, u8 ie);
int bes2600_vif_setup(struct bes2600_vif *priv);
int bes2600_setup_mac_pvif(struct bes2600_vif *priv);
void bes2600_iterate_vifs(void *data, u8 *mac,
			 struct ieee80211_vif *vif);
void bes2600_rem_chan_timeout(struct work_struct *work);
int bes2600_set_macaddrfilter(struct bes2600_common *hw_priv, struct bes2600_vif *priv, u8 *data);
#ifdef IPV6_FILTERING
int bes2600_set_na(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif);
#endif /*IPV6_FILTERING*/
#ifdef CONFIG_BES2600_TESTMODE
void bes2600_device_power_calc(struct bes2600_common *priv,
			      s16 max_output_power, s16 fe_cor, u32 band);
int bes2600_testmode_cmd(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void *data, int len);
int bes2600_testmode_event(struct wiphy *wiphy, const u32 msg_id,
			 const void *data, int len, gfp_t gfp);
int bes2600_get_tx_power_range(struct ieee80211_hw *hw);
int bes2600_get_tx_power_level(struct ieee80211_hw *hw);
#endif /* CONFIG_BES2600_TESTMODE */
int bes2600_wifi_start(struct bes2600_common *hw_priv);
int bes2600_wifi_stop(struct bes2600_common *hw_priv);
#endif /* STA_H_INCLUDED */
