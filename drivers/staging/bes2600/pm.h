/*
 * Mac80211 power management interface for BES2600 mac80211 drivers
 *
 * Copyright (c) 2011, Bestechnic
 * Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef PM_H_INCLUDED
#define PM_H_INCLUDED

#ifdef CONFIG_PM

/* extern */  struct bes2600_common;
/* private */ struct bes2600_suspend_state;

struct bes2600_pm_state_vif {
	struct bes2600_suspend_state *suspend_state;
};

void bes2600_suspend_status_set(struct bes2600_common *hw_priv, bool status);
bool bes2600_suspend_status_get(struct bes2600_common *hw_priv);
void bes2600_pending_unjoin_reset(struct bes2600_common *hw_priv);
void bes2600_pending_unjoin_set(struct bes2600_common *hw_priv, int if_id);
bool bes2600_pending_unjoin_get(struct bes2600_common *hw_priv, int if_id);
void bes2600_register_pm_notifier(struct bes2600_common *hw_priv);
void bes2600_unregister_pm_notifier(struct bes2600_common *hw_priv);
int bes2600_can_suspend(struct bes2600_common *priv);
int bes2600_wow_suspend(struct ieee80211_hw *hw,
		       struct cfg80211_wowlan *wowlan);
int bes2600_wow_resume(struct ieee80211_hw *hw);
#else
static inline int bes2600_can_suspend(struct bes2600_common *priv)
{
	return 0;
}
#endif /* CONFIG_PM */

#endif
