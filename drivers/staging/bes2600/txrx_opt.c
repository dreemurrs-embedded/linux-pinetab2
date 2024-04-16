/***************************************************************************
 *
 * Copyright 2015-2022 BES.
 * All rights reserved. All unpublished rights reserved.
 *
 * No part of this work may be used or reproduced in any form or by any
 * means, or stored in a database or retrieval system, without prior written
 * permission of BES.
 *
 * Use of this work is governed by a license granted by BES.
 * This work contains confidential and proprietary information of
 * BES. which is protected by copyright, trade secret,
 * trademark and other intellectual property rights.
 *
 ****************************************************************************/
#include <net/mac80211.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/timer.h>

#include "bes2600.h"
#include "wsm.h"
#include "bh.h"
#include "ap.h"
#include "debug.h"
#include "sta.h"
#include "sbus.h"
#include "bes_pwr.h"
#include "txrx_opt.h"

#define TXRX_OPT_CLOSE_EDCA     0
#define TXRX_OPT_EDCA_MAX_LEVEL 4
#define TX_AVG_TIME_COUNT       10
#define TXRX_OPT_PEROID         500
#define TXRX_OPT_DEBUG          1

#define TXRX_HIGH_TP_THRESHOLD_2G4    	30000   // unit is kbps
#define TXRX_HIGH_TP_THRESHOLD_5G    	40000   // unit is kbps
#define TXRX_HIGH_TP_DELTA_TIME_2G4   	8       // unit ms
#define TXRX_HIGH_TP_DELTA_TIME_5G   	6       // unit ms

#define TXRX_RTS_PROT_TRIG_THRESH		80		// percent * 100
#define TXRX_RTS_PROT_DURATION			10		// unit second

#define TXRX_RTS_PROT_OPEN(x)			(x = 512)
#define TXRX_RTS_PROT_CLOSE(x)			(x = 2437)
#define TXRX_RTS_PROT_OPENED(x)			(x < 1536)

static uint32_t tx_delta_time_arr[4][TX_AVG_TIME_COUNT];
static uint32_t tx_queue_arr[4] = {0};
static uint32_t tx_delta_time_total = 0;
static uint32_t tx_delta_time_total_cnt = 0;
static u8 cur_pwr_tbl = 1;
static u16 cur_rts_thres = 2437;
static unsigned long last_rts_set_time = -1;

void bes2600_add_tx_delta_time(uint32_t tx_delta_time)
{
	tx_delta_time_total += tx_delta_time;
	tx_delta_time_total_cnt++;
}

static uint32_t bes2600_get_tx_delta_time(void)
{
	if (tx_delta_time_total_cnt != 0)
		return tx_delta_time_total / tx_delta_time_total_cnt;
	else
		return 0;
}

static void bes2600_clear_tx_delta_time(void)
{
	tx_delta_time_total_cnt = 0;
	tx_delta_time_total = 0;
	return ;
}

static uint32_t bes2600_get_tx_ac_delta_time(int ac)
{
	uint32_t avg_time = 0;
	int i = 0;
	for (i = 0; i < TX_AVG_TIME_COUNT; i++) {
		avg_time += tx_delta_time_arr[ac][i];
	}
	return avg_time / TX_AVG_TIME_COUNT;
}

static void bes2600_clear_tx_ac_delta_time(int ac)
{
	int i = 0;
	for (i = 0; i < TX_AVG_TIME_COUNT; i++) {
		tx_delta_time_arr[ac][i] = 0;
	}
	return ;
}

void bes2600_add_tx_ac_delta_time(int ac, uint32_t del_time)
{
#if 0
	if (tx_queue_arr[ac] >= (TX_AVG_TIME_COUNT - 1)) {
		static int num = 0;
		if ((num ++ % 10) == 0)
			bes_devel( "%s %d %d %d %d %d del=%d\n\r", __func__, tx_delta_time_arr[ac][0],
				     tx_delta_time_arr[ac][2],  tx_delta_time_arr[ac][4],  tx_delta_time_arr[ac][6],
				     tx_delta_time_arr[ac][8], del_time);
	}
#endif
	tx_delta_time_arr[ac][tx_queue_arr[ac]] = del_time;
	tx_queue_arr[ac] = (tx_queue_arr[ac] >= (TX_AVG_TIME_COUNT - 1)) ? 0 : (tx_queue_arr[ac] + 1);

}


static int bes2600_set_txrx_opt_param(struct bes2600_common *hw_priv,
			       struct bes2600_vif *priv,
			       MIB_TXRX_OPT_PARAM  *para)
{

	int ret = 0;
	ret = WARN_ON(wsm_write_mib(hw_priv,
				    WSM_MIB_ID_EXT_TXRX_OPT_PARAM,
				    (u8 *)para,
				    sizeof(MIB_TXRX_OPT_PARAM),
				    priv->if_id));
	return ret;
}

static int bes2600_enable_tx_shortgi(struct bes2600_common *hw_priv,
			      struct bes2600_vif *priv,
			      u8 onoff)
{
	int ret = 0;
	static u8 en = 0xff;

	bes_devel( "%s onoff=%d\n\r", __func__, onoff);

	if (en != onoff) {
		en = onoff;
		ret = WARN_ON(wsm_write_mib(hw_priv,
					    WSM_MIB_ID_EXT_TX_SHORT_GI_ENABLED,
					    (u8 *)&onoff,
					    sizeof(onoff),
					    priv->if_id));
	}
	return ret;
}

void bes2600_rx_status(struct bes2600_vif *priv, struct sk_buff *skb)
{
	priv->dot11ReceivedFragmentCount++;
	priv->dot11ReceivedDataBytes += skb->len;
}

void bes2600_tx_status(struct bes2600_vif *priv, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	__le16 fc;
	int i;
	int retry_count = -1;
	fc = hdr->frame_control;

	if (ieee80211_is_data(fc)) {
		for (i = 0; i < IEEE80211_TX_MAX_RATES; i++) {
			if (info->status.rates[i].idx < 0) {
				break;
			}
			retry_count += info->status.rates[i].count;
		}
		if (retry_count < 0)
			retry_count = 0;

		if (info->flags & IEEE80211_TX_STAT_ACK) {
			priv->dot11TransmittedFrameCount++;
			priv->dot11TransmittedDataBytes += (skb->len + 4);
			if (retry_count > 0)
				priv->dot11RetryCount += retry_count;

		} else {
			/* tx fail.*/
			priv->dot11FailedCount++;
		}
	}

}
void bes2600_set_default_params(struct bes2600_common *hw_priv, struct bes2600_vif *priv);

static int bes2600_set_high_edca_params(struct bes2600_common *hw_priv, struct bes2600_vif *priv, int level)
{
	struct wsm_edca_params arg;
	int i = 0;
	static int lev = 0;
	bes_devel( "set edca level=%d\n\r", level);

	if (lev == level)
		return 0;

	lev = level;

	memcpy(&arg, &(priv->edca), sizeof(struct wsm_edca_params));

	if (level == 0) {
		bes2600_set_default_params(hw_priv, priv);
		return 0;
	} else if (level == 1) {
		for ( i = 0; i < 4; i++) {
			arg.params[i].aifns = 2;
			arg.params[i].cwMax = 7;
			arg.params[i].cwMin = 3;
			/*
			 * tx op must set 0
			 * set other, some AP may not response BA when rx data.
			*/
			arg.params[i].txOpLimit = 0;
			arg.params[i].maxReceiveLifetime = 0xc8;
		}
	} else if (level == 2) {
		for ( i = 0; i < 4; i++) {
			arg.params[i].aifns = 2;
			arg.params[i].cwMax = 5;
			arg.params[i].cwMin = 1;
			arg.params[i].txOpLimit = 0;
			arg.params[i].maxReceiveLifetime = 0xc8;
		}
	} else if (level == 3) {
		for ( i = 0; i < 4; i++) {
			arg.params[i].aifns = 2;
			arg.params[i].cwMax = 3;
			arg.params[i].cwMin = 1;
			arg.params[i].txOpLimit = 0;
			arg.params[i].maxReceiveLifetime = 0xc8;
		}
	} else if (level == 4) {
		for ( i = 0; i < 4; i++) {
			arg.params[i].aifns = 1;
			arg.params[i].cwMax = 3;
			arg.params[i].cwMin = 1;
			arg.params[i].txOpLimit = 0;
			arg.params[i].maxReceiveLifetime = 0xc8;
		}
	}
	wsm_set_edca_params(hw_priv, &arg, priv->if_id);
	return 0;
}
void bes2600_set_default_params(struct bes2600_common *hw_priv, struct bes2600_vif *priv)
{
	bes_devel( "set edca default\n\r");
	wsm_set_edca_params(hw_priv, &priv->edca, priv->if_id);
}
static void bes2600_set_cca_method(struct bes2600_common *hw_priv, struct bes2600_vif *priv, int value)
{
// todo set cca alg
}

static void bes2600_set_dynamic_agc(struct bes2600_common *hw_priv, struct bes2600_vif *priv, int value)
{
// todo set agc alg
}

static int bes2600_update_pwr_table(struct bes2600_common *hw_priv,
			      struct bes2600_vif *priv,
			      u8 pwr_tbl_idx)
{
	int ret = 0;
	static u8 cur_pwr_tbl_idx = 0xff;

	if (cur_pwr_tbl_idx != pwr_tbl_idx) {
		cur_pwr_tbl_idx = pwr_tbl_idx;
		ret = WARN_ON(wsm_write_mib(hw_priv,
					    WSM_MIB_ID_EXT_PWR_TBL_UPDATE,
					    (u8 *)&cur_pwr_tbl_idx,
					    sizeof(cur_pwr_tbl_idx),
					    priv->if_id));
		bes_devel( "%s pwr_tbl_idx=%d\n\r", __func__, pwr_tbl_idx);
	}
	return ret;
}

static int bes2600_get_tx_av_max_delta_time(void)
{
	int max_avg = 0;
	int i = 0;

	for (i = 0; i < 4; i++) {
		if (max_avg < bes2600_get_tx_ac_delta_time(i)) {
			max_avg = bes2600_get_tx_ac_delta_time(i);
		}
		//bes2600_clear_tx_ac_delta_time(i);
	}
	return max_avg;
}

static bool bes2600_station_is_ap_ht40(struct bes2600_common *hw_priv)
{

	if (hw_priv->hw) {
		struct ieee80211_conf *conf = &hw_priv->hw->conf;
		if (conf !=  NULL)
			if (conf->chandef.width == NL80211_CHAN_WIDTH_40)
				return true;
	}
	return false;
}

void bes2600_dynamic_opt_rxtx(struct bes2600_common *hw_priv, struct bes2600_vif *priv, int rssi)
{
	u32 succPro = 0, tx_cnt, tx_retry, rx_cnt, tx_fail;
	static u32 l_tx_cnt = 0, l_tx_fail = 0, l_tx_retry = 0, l_rx_cnt = 0;
	static u32 tx_bps = 0, rx_bps = 0;
	u32 total_kbps = 0;
	static int level;

	/* calculate real time throughput */
	if (hw_priv == NULL || priv == NULL) {
		return;
	}
	tx_bps = abs (priv->dot11TransmittedDataBytes - tx_bps);
	rx_bps = abs (priv->dot11ReceivedDataBytes  - rx_bps);
	total_kbps = (tx_bps / 128 + rx_bps / 128);

	total_kbps *= 1000;
	total_kbps /= TXRX_OPT_PEROID;

	/*  if tx/rx < 100k/s, close*/
	if (total_kbps < 100) {
		level = 0;
		last_rts_set_time = -1;
		TXRX_RTS_PROT_CLOSE(cur_rts_thres);
		goto txrx_opt_clear;
	}

	/* calculate tx_cnt, tx_retry, rx_cnt */
	tx_cnt = (priv->dot11TransmittedFrameCount - l_tx_cnt);
	tx_fail = (priv->dot11FailedCount - l_tx_fail);
	tx_retry = (priv->dot11RetryCount - l_tx_retry);
	rx_cnt = (priv->dot11ReceivedFragmentCount - l_rx_cnt);
	( (tx_cnt + tx_retry) > 0 ) ? (succPro = tx_cnt * 100 / (tx_cnt + tx_retry)) : (succPro = 0);

	bes_devel( "%s, tx_cnt:%d prob:%d\n", __func__, tx_cnt, succPro);

	/* set rts/cts protection dynamically */
	if (tx_cnt > 50 && succPro != 0) {
		if (succPro > TXRX_RTS_PROT_TRIG_THRESH &&
			TXRX_RTS_PROT_OPENED(cur_rts_thres) &&
		    time_after(jiffies, last_rts_set_time + TXRX_RTS_PROT_DURATION * HZ)) {
			TXRX_RTS_PROT_CLOSE(cur_rts_thres);
		} else if (succPro <= TXRX_RTS_PROT_TRIG_THRESH){
			TXRX_RTS_PROT_OPEN(cur_rts_thres);
			last_rts_set_time = jiffies;
		}
	}

	/* dynamic set edca param */
	if (succPro != 0) {
		if (bes2600_station_is_ap_ht40(hw_priv)) {
			if (bes2600_get_tx_delta_time() > 8 || bes2600_get_tx_av_max_delta_time() > 8) {
				if (level < 4)
					level++;
			} else {
				if (level > 0)
					level--;
			}
			/* high throughput force level = 0 */
			if (total_kbps > TXRX_HIGH_TP_THRESHOLD_5G && level > 0 && priv->hw_value > 19) {
				level = 0;
			}

		} else {//shiled room 13, office 8
			if (bes2600_get_tx_delta_time() > (TXRX_HIGH_TP_DELTA_TIME_5G + total_kbps / 8000)
			    || bes2600_get_tx_av_max_delta_time() > (TXRX_HIGH_TP_DELTA_TIME_5G + total_kbps / 8000)) {
				if (level < 4)
					level++;
			} else {
				if (level > 0)
					level--;
			}
			/* high throughput force level = 0 */
			if (total_kbps > TXRX_HIGH_TP_THRESHOLD_2G4 && level > 0) {
				level = 0;
			}

		}
	}

	/* dynamic set power table */
	if (rssi <= BES2600_TX_RSSI_LOW)
		cur_pwr_tbl = 2;	// use high power table
	else if(rssi >= BES2600_TX_RSSI_HIGH)
		cur_pwr_tbl = 1;	// use standard power table

#if TXRX_OPT_CLOSE_EDCA
	level = 0;
#endif
	if (level > TXRX_OPT_EDCA_MAX_LEVEL)
		level = TXRX_OPT_EDCA_MAX_LEVEL;

	bes_devel( "txrx_opt: tx(cnt=%d retry=%d psr=%d tx_fail=%d (wsm level=%d) tx=%dk/s)\n\r",
	       tx_cnt, tx_retry, succPro, tx_fail, level, tx_bps / 128);
	bes_devel( "txrx_opt: rx(cnt=%d  rx=%dk/s) total=%dk/s\n\r", rx_cnt, rx_bps / 128, total_kbps);
	bes_devel( "txrx_opt: tx_delta_time=%d [%d %d %d %d] hw_value=%d ht=%d maxtxcnt=%d\n\r",
	       bes2600_get_tx_delta_time(), bes2600_get_tx_ac_delta_time(0), bes2600_get_tx_ac_delta_time(1),
	       bes2600_get_tx_ac_delta_time(2), bes2600_get_tx_ac_delta_time(3), priv->hw_value,
	       bes2600_station_is_ap_ht40(hw_priv), hw_priv->long_frame_max_tx_count);


	/* dynamic set cca */
	bes2600_set_cca_method(hw_priv, priv, 0);
	/* dynamic set agc */
	bes2600_set_dynamic_agc(hw_priv, priv, 0);
	bes2600_update_pwr_table(hw_priv, priv, cur_pwr_tbl);
txrx_opt_clear:
	bes2600_set_high_edca_params(hw_priv, priv, level);
	bes2600_set_rts_threshold(hw_priv->hw, cur_rts_thres);
	bes2600_clear_tx_delta_time();
	bes2600_clear_tx_ac_delta_time(0);
	bes2600_clear_tx_ac_delta_time(1);
	bes2600_clear_tx_ac_delta_time(2);
	bes2600_clear_tx_ac_delta_time(3);
	tx_bps = priv->dot11TransmittedDataBytes;
	rx_bps = priv->dot11ReceivedDataBytes;
	l_tx_cnt = priv->dot11TransmittedFrameCount;
	l_tx_fail = priv->dot11FailedCount;
	l_tx_retry = priv->dot11RetryCount;
	l_rx_cnt = priv->dot11ReceivedFragmentCount;
	return ;
}

static struct bes2600_common *txrx_hw_priv = NULL;

static bool bes2600_is_sta_connected(void)
{
	if (txrx_hw_priv == NULL)
		return false;
	else
		return true;
}

void bes2600_txrx_opt_timer_restore(void)
{
	if (bes2600_is_sta_connected()) {
		mod_timer(&txrx_hw_priv->txrx_opt_timer, jiffies + msecs_to_jiffies(TXRX_OPT_PEROID));
	}
}

static void txrx_opt_timer_callback(struct timer_list* data)
{
	bes_devel( "####Timer callback function Called time = %lu\n", jiffies);
	queue_work(txrx_hw_priv->workqueue, &txrx_hw_priv->dynamic_opt_txrx_work);
}

static void txrx_opt_timer_start(struct bes2600_common *hw_priv)
{
	mod_timer(&hw_priv->txrx_opt_timer, jiffies + msecs_to_jiffies(TXRX_OPT_PEROID));
}

static void txrx_opt_timer_stop(struct bes2600_common *hw_priv)
{
	del_timer_sync(&hw_priv->txrx_opt_timer);
}

static int bes2600_set_txrx_opt_default_param(struct bes2600_common * hw_priv)
{
	MIB_TXRX_OPT_PARAM g_txrx_param = {2, (PROCTECT_MODE_RTS_CTS | PROCTECT_MODE_RTS_CTS_RETRY), 3000};
	struct bes2600_vif *priv = __cw12xx_hwpriv_to_vifpriv(hw_priv, 0);
	struct ieee80211_sta *sta = NULL;

	if (priv == NULL)
		return 0;

	/* reset states */
	cur_pwr_tbl = 1;
	TXRX_RTS_PROT_CLOSE(cur_rts_thres);
	last_rts_set_time = -1;
	memcpy(&hw_priv->txrx_opt_param, &g_txrx_param, sizeof(MIB_TXRX_OPT_PARAM));

	/* reset device states */
	bes2600_set_txrx_opt_param(hw_priv, priv, &hw_priv->txrx_opt_param);
	bes2600_set_rts_threshold(hw_priv->hw, cur_rts_thres);	// close rts/cts
	bes2600_update_pwr_table(hw_priv, priv, cur_pwr_tbl);	// use standard pwr table

	if (priv->join_status == BES2600_JOIN_STATUS_STA) {
		sta = ieee80211_find_sta(priv->vif, priv->vif->bss_conf.bssid);
		if (sta->deflink.ht_cap.ht_supported &&
		    ((priv->vif->bss_conf.chandef.width == NL80211_CHAN_WIDTH_20 &&
			 sta->deflink.ht_cap.cap & IEEE80211_HT_CAP_SGI_20) ||
			(priv->vif->bss_conf.chandef.width == NL80211_CHAN_WIDTH_40 &&
			 sta->deflink.ht_cap.cap & IEEE80211_HT_CAP_SGI_40))) {
			bes_devel( "open short gi tx\n");
			bes2600_enable_tx_shortgi(hw_priv, priv, 1);
		} else {
			bes_devel( "close short gi tx\n");
			bes2600_enable_tx_shortgi(hw_priv, priv, 0);
		}
	}

	return 0;
}

static int bes2600_set_txrx_opt_unjoin_param(struct bes2600_common * hw_priv)
{
	MIB_TXRX_OPT_PARAM g_txrx_param = {1, 0, 2002};
	struct bes2600_vif *priv = __cw12xx_hwpriv_to_vifpriv(hw_priv, 0);
	if (priv == NULL)
		return 0;

	/* reset states */
	cur_pwr_tbl = 1;
	bes2600_update_pwr_table(hw_priv, priv, cur_pwr_tbl);
	memcpy(&hw_priv->txrx_opt_param, &g_txrx_param, sizeof(MIB_TXRX_OPT_PARAM));
	bes2600_set_txrx_opt_param(hw_priv, priv, &hw_priv->txrx_opt_param);
	return 0;
}

void bes2600_txrx_opt_multivif_connected_handler(struct bes2600_common *hw_priv, bool multivif_connected)
{
	struct bes2600_vif *priv = __cw12xx_hwpriv_to_vifpriv(hw_priv, 0);

	if (multivif_connected) {
		bes2600_set_txrx_opt_default_param(hw_priv);
	} else {
		bes_devel("%s, rssi:%d\n", __func__, priv->signal);
		bes2600_dynamic_opt_rxtx(hw_priv, priv, priv->signal);
		mod_timer(&hw_priv->txrx_opt_timer, jiffies + msecs_to_jiffies(TXRX_OPT_PEROID));
	}
}

int txrx_opt_timer_init(struct bes2600_vif *priv)
{
	struct bes2600_common *hw_priv = cw12xx_vifpriv_to_hwpriv(priv);
	bes_devel( "txrx_opt_timer_init:%p", txrx_hw_priv);
	if (priv->if_id != 0)
		return 0;

	if (!txrx_hw_priv) {
		txrx_hw_priv = hw_priv;
		bes_devel( "####Timer init hw_priv = %p\n", txrx_hw_priv);
		timer_setup(&hw_priv->txrx_opt_timer, txrx_opt_timer_callback, 0);
		bes2600_set_txrx_opt_default_param(hw_priv);
	}

	mod_timer(&hw_priv->txrx_opt_timer, jiffies + msecs_to_jiffies(TXRX_OPT_PEROID));
	bes2600_pwr_register_en_lp_cb(hw_priv, txrx_opt_timer_stop);
	bes2600_pwr_register_exit_lp_cb(hw_priv, txrx_opt_timer_start);
	return 0;
}

void txrx_opt_timer_exit(struct bes2600_vif *priv)
{
	struct bes2600_common *hw_priv = cw12xx_vifpriv_to_hwpriv(priv);
	bes_devel( "txrx_opt_timer_exit");

	if (priv->if_id == 0) {
		del_timer_sync(&hw_priv->txrx_opt_timer);
		cancel_work_sync(&hw_priv->dynamic_opt_txrx_work);
		bes2600_pwr_unregister_en_lp_cb(hw_priv, txrx_opt_timer_stop);
		bes2600_pwr_unregister_exit_lp_cb(hw_priv, txrx_opt_timer_start);
		txrx_hw_priv = NULL;
		bes2600_set_txrx_opt_unjoin_param(hw_priv);
	} else if (priv->if_id == 1) {
		bes2600_txrx_opt_timer_restore();
	}
}


