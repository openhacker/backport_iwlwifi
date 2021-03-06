// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/kernel.h>
#include <net/mac80211.h>
#include "mvm.h"
#include "fw/api/context.h"
#include "fw/api/datapath.h"

static u32 iwl_mvm_get_sec_sta_mask(struct iwl_mvm *mvm,
				    struct ieee80211_vif *vif,
				    struct ieee80211_sta *sta,
				    struct ieee80211_key_conf *keyconf)
{
	struct iwl_mvm_vif *mvmvif = iwl_mvm_vif_from_mac80211(vif);

	if (vif->type == NL80211_IFTYPE_AP &&
	    !(keyconf->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return BIT(mvmvif->mcast_sta.sta_id);

	if (sta) {
		struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);

		return BIT(mvmsta->sta_id);
	}

	if (vif->type == NL80211_IFTYPE_STATION &&
	    mvmvif->ap_sta_id != IWL_MVM_INVALID_STA)
		return BIT(mvmvif->ap_sta_id);

	/* invalid */
	return 0;
}

static u32 iwl_mvm_get_sec_flags(struct iwl_mvm *mvm,
				 struct ieee80211_vif *vif,
				 struct ieee80211_sta *sta,
				 struct ieee80211_key_conf *keyconf)
{
	struct iwl_mvm_vif *mvmvif = iwl_mvm_vif_from_mac80211(vif);
	u32 flags = 0;

	if (!(keyconf->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		flags |= IWL_SEC_KEY_FLAG_MCAST_KEY;

	switch (keyconf->cipher) {
	case WLAN_CIPHER_SUITE_WEP104:
		flags |= IWL_SEC_KEY_FLAG_KEY_SIZE;
		fallthrough;
	case WLAN_CIPHER_SUITE_WEP40:
		flags |= IWL_SEC_KEY_FLAG_CIPHER_WEP;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		flags |= IWL_SEC_KEY_FLAG_CIPHER_TKIP;
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
	case WLAN_CIPHER_SUITE_CCMP:
		flags |= IWL_SEC_KEY_FLAG_CIPHER_CCMP;
		break;
	case WLAN_CIPHER_SUITE_GCMP_256:
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
		flags |= IWL_SEC_KEY_FLAG_KEY_SIZE;
		fallthrough;
	case WLAN_CIPHER_SUITE_GCMP:
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
		flags |= IWL_SEC_KEY_FLAG_CIPHER_GCMP;
		break;
	}

	rcu_read_lock();
	if (!sta && vif->type == NL80211_IFTYPE_STATION &&
	    mvmvif->ap_sta_id != IWL_MVM_INVALID_STA) {
		u8 sta_id = mvmvif->ap_sta_id;

		sta = rcu_dereference_check(mvm->fw_id_to_mac_id[sta_id],
					    lockdep_is_held(&mvm->mutex));
	}

	if (!IS_ERR_OR_NULL(sta) && sta->mfp)
		flags |= IWL_SEC_KEY_FLAG_MFP;
	rcu_read_unlock();

	return flags;
}

int iwl_mvm_sec_key_add(struct iwl_mvm *mvm,
			struct ieee80211_vif *vif,
			struct ieee80211_sta *sta,
			struct ieee80211_key_conf *keyconf)
{
	u32 sta_mask = iwl_mvm_get_sec_sta_mask(mvm, vif, sta, keyconf);
	u32 key_flags = iwl_mvm_get_sec_flags(mvm, vif, sta, keyconf);
	u32 cmd_id = WIDE_ID(DATA_PATH_GROUP, SEC_KEY_CMD);
	struct iwl_sec_key_cmd cmd = {
		.action = cpu_to_le32(FW_CTXT_ACTION_ADD),
		.u.add.sta_mask = cpu_to_le32(sta_mask),
		.u.add.key_id = cpu_to_le32(keyconf->keyidx),
		.u.add.key_flags = cpu_to_le32(key_flags),
		.u.add.tx_seq = cpu_to_le64(atomic64_read(&keyconf->tx_pn)),
	};

	if (WARN_ON(keyconf->keylen > sizeof(cmd.u.add.key)))
		return -EINVAL;

	memcpy(cmd.u.add.key, keyconf->key, keyconf->keylen);
	if (keyconf->cipher == WLAN_CIPHER_SUITE_TKIP) {
		memcpy(cmd.u.add.tkip_mic_rx_key,
		       keyconf->key + NL80211_TKIP_DATA_OFFSET_RX_MIC_KEY,
		       8);
		memcpy(cmd.u.add.tkip_mic_tx_key,
		       keyconf->key + NL80211_TKIP_DATA_OFFSET_TX_MIC_KEY,
		       8);
	}

	return iwl_mvm_send_cmd_pdu(mvm, cmd_id, 0, sizeof(cmd), &cmd);
}

static int _iwl_mvm_sec_key_del(struct iwl_mvm *mvm,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				struct ieee80211_key_conf *keyconf,
				u32 flags)
{
	u32 sta_mask = iwl_mvm_get_sec_sta_mask(mvm, vif, sta, keyconf);
	u32 key_flags = iwl_mvm_get_sec_flags(mvm, vif, sta, keyconf);
	u32 cmd_id = WIDE_ID(DATA_PATH_GROUP, SEC_KEY_CMD);
	struct iwl_sec_key_cmd cmd = {
		.action = cpu_to_le32(FW_CTXT_ACTION_REMOVE),
		.u.remove.sta_mask = cpu_to_le32(sta_mask),
		.u.remove.key_id = cpu_to_le32(keyconf->keyidx),
		.u.remove.key_flags = cpu_to_le32(key_flags),
	};

	return iwl_mvm_send_cmd_pdu(mvm, cmd_id, flags, sizeof(cmd), &cmd);
}

int iwl_mvm_sec_key_del(struct iwl_mvm *mvm,
			struct ieee80211_vif *vif,
			struct ieee80211_sta *sta,
			struct ieee80211_key_conf *keyconf)
{
	return _iwl_mvm_sec_key_del(mvm, vif, sta, keyconf, 0);
}

static void iwl_mvm_sec_key_remove_ap_iter(struct ieee80211_hw *hw,
					   struct ieee80211_vif *vif,
					   struct ieee80211_sta *sta,
					   struct ieee80211_key_conf *key,
					   void *data)
{
	struct iwl_mvm *mvm = IWL_MAC80211_GET_MVM(hw);

	if (key->hw_key_idx == STA_KEY_IDX_INVALID)
		return;

	if (sta)
		return;

	_iwl_mvm_sec_key_del(mvm, vif, NULL, key, CMD_ASYNC);
	key->hw_key_idx = STA_KEY_IDX_INVALID;
}

void iwl_mvm_sec_key_remove_ap(struct iwl_mvm *mvm,
			       struct ieee80211_vif *vif)
{
	struct iwl_mvm_vif *mvmvif = iwl_mvm_vif_from_mac80211(vif);
	u32 sec_key_id = WIDE_ID(DATA_PATH_GROUP, SEC_KEY_CMD);
	u8 sec_key_ver = iwl_fw_lookup_cmd_ver(mvm->fw, sec_key_id, 0);

	if (WARN_ON(vif->type != NL80211_IFTYPE_STATION ||
		    mvmvif->ap_sta_id == IWL_MVM_INVALID_STA))
		return;

	if (!sec_key_ver)
		return;

	ieee80211_iter_keys_rcu(mvm->hw, vif,
				iwl_mvm_sec_key_remove_ap_iter,
				NULL);
}
