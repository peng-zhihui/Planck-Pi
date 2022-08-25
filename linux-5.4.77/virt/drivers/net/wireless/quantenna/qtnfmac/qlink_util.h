/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2015-2016 Quantenna Communications. All rights reserved. */

#ifndef _QTN_FMAC_QLINK_UTIL_H_
#define _QTN_FMAC_QLINK_UTIL_H_

#include <linux/types.h>
#include <linux/skbuff.h>
#include <net/cfg80211.h>

#include "qlink.h"

static inline void
qtnf_cmd_skb_put_buffer(struct sk_buff *skb, const u8 *buf_src, size_t len)
{
	skb_put_data(skb, buf_src, len);
}

static inline void qtnf_cmd_skb_put_tlv_arr(struct sk_buff *skb,
					    u16 tlv_id, const u8 arr[],
					    size_t arr_len)
{
	struct qlink_tlv_hdr *hdr = skb_put(skb, sizeof(*hdr) + arr_len);

	hdr->type = cpu_to_le16(tlv_id);
	hdr->len = cpu_to_le16(arr_len);
	memcpy(hdr->val, arr, arr_len);
}

static inline void qtnf_cmd_skb_put_tlv_tag(struct sk_buff *skb, u16 tlv_id)
{
	struct qlink_tlv_hdr *hdr = skb_put(skb, sizeof(*hdr));

	hdr->type = cpu_to_le16(tlv_id);
	hdr->len = cpu_to_le16(0);
}

static inline void qtnf_cmd_skb_put_tlv_u8(struct sk_buff *skb, u16 tlv_id,
					   u8 value)
{
	struct qlink_tlv_hdr *hdr = skb_put(skb, sizeof(*hdr) + sizeof(value));

	hdr->type = cpu_to_le16(tlv_id);
	hdr->len = cpu_to_le16(sizeof(value));
	*hdr->val = value;
}

static inline void qtnf_cmd_skb_put_tlv_u16(struct sk_buff *skb,
					    u16 tlv_id, u16 value)
{
	struct qlink_tlv_hdr *hdr = skb_put(skb, sizeof(*hdr) + sizeof(value));
	__le16 tmp = cpu_to_le16(value);

	hdr->type = cpu_to_le16(tlv_id);
	hdr->len = cpu_to_le16(sizeof(value));
	memcpy(hdr->val, &tmp, sizeof(tmp));
}

static inline void qtnf_cmd_skb_put_tlv_u32(struct sk_buff *skb,
					    u16 tlv_id, u32 value)
{
	struct qlink_tlv_hdr *hdr = skb_put(skb, sizeof(*hdr) + sizeof(value));
	__le32 tmp = cpu_to_le32(value);

	hdr->type = cpu_to_le16(tlv_id);
	hdr->len = cpu_to_le16(sizeof(value));
	memcpy(hdr->val, &tmp, sizeof(tmp));
}

u16 qlink_iface_type_to_nl_mask(u16 qlink_type);
u8 qlink_chan_width_mask_to_nl(u16 qlink_mask);
void qlink_chandef_q2cfg(struct wiphy *wiphy,
			 const struct qlink_chandef *qch,
			 struct cfg80211_chan_def *chdef);
void qlink_chandef_cfg2q(const struct cfg80211_chan_def *chdef,
			 struct qlink_chandef *qch);
enum qlink_hidden_ssid qlink_hidden_ssid_nl2q(enum nl80211_hidden_ssid nl_val);
bool qtnf_utils_is_bit_set(const u8 *arr, unsigned int bit,
			   unsigned int arr_max_len);
void qlink_acl_data_cfg2q(const struct cfg80211_acl_data *acl,
			  struct qlink_acl_data *qacl);
enum qlink_band qlink_utils_band_cfg2q(enum nl80211_band band);
enum qlink_dfs_state qlink_utils_dfs_state_cfg2q(enum nl80211_dfs_state state);
u32 qlink_utils_chflags_cfg2q(u32 cfgflags);
void qlink_utils_regrule_q2nl(struct ieee80211_reg_rule *rule,
			      const struct qlink_tlv_reg_rule *tlv_rule);

#endif /* _QTN_FMAC_QLINK_UTIL_H_ */
