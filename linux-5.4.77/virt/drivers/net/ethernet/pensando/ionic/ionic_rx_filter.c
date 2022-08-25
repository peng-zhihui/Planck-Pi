// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2019 Pensando Systems, Inc */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "ionic.h"
#include "ionic_lif.h"
#include "ionic_rx_filter.h"

void ionic_rx_filter_free(struct ionic_lif *lif, struct ionic_rx_filter *f)
{
	struct device *dev = lif->ionic->dev;

	hlist_del(&f->by_id);
	hlist_del(&f->by_hash);
	devm_kfree(dev, f);
}

int ionic_rx_filter_del(struct ionic_lif *lif, struct ionic_rx_filter *f)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_del = {
			.opcode = IONIC_CMD_RX_FILTER_DEL,
			.filter_id = cpu_to_le32(f->filter_id),
		},
	};

	return ionic_adminq_post_wait(lif, &ctx);
}

int ionic_rx_filters_init(struct ionic_lif *lif)
{
	unsigned int i;

	spin_lock_init(&lif->rx_filters.lock);

	spin_lock_bh(&lif->rx_filters.lock);
	for (i = 0; i < IONIC_RX_FILTER_HLISTS; i++) {
		INIT_HLIST_HEAD(&lif->rx_filters.by_hash[i]);
		INIT_HLIST_HEAD(&lif->rx_filters.by_id[i]);
	}
	spin_unlock_bh(&lif->rx_filters.lock);

	return 0;
}

void ionic_rx_filters_deinit(struct ionic_lif *lif)
{
	struct ionic_rx_filter *f;
	struct hlist_head *head;
	struct hlist_node *tmp;
	unsigned int i;

	spin_lock_bh(&lif->rx_filters.lock);
	for (i = 0; i < IONIC_RX_FILTER_HLISTS; i++) {
		head = &lif->rx_filters.by_id[i];
		hlist_for_each_entry_safe(f, tmp, head, by_id)
			ionic_rx_filter_free(lif, f);
	}
	spin_unlock_bh(&lif->rx_filters.lock);
}

int ionic_rx_filter_save(struct ionic_lif *lif, u32 flow_id, u16 rxq_index,
			 u32 hash, struct ionic_admin_ctx *ctx)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_rx_filter_add_cmd *ac;
	struct ionic_rx_filter *f;
	struct hlist_head *head;
	unsigned int key;

	ac = &ctx->cmd.rx_filter_add;

	switch (le16_to_cpu(ac->match)) {
	case IONIC_RX_FILTER_MATCH_VLAN:
		key = le16_to_cpu(ac->vlan.vlan);
		break;
	case IONIC_RX_FILTER_MATCH_MAC:
		key = *(u32 *)ac->mac.addr;
		break;
	case IONIC_RX_FILTER_MATCH_MAC_VLAN:
		key = le16_to_cpu(ac->mac_vlan.vlan);
		break;
	default:
		return -EINVAL;
	}

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	f->flow_id = flow_id;
	f->filter_id = le32_to_cpu(ctx->comp.rx_filter_add.filter_id);
	f->rxq_index = rxq_index;
	memcpy(&f->cmd, ac, sizeof(f->cmd));
	netdev_dbg(lif->netdev, "rx_filter add filter_id %d\n", f->filter_id);

	INIT_HLIST_NODE(&f->by_hash);
	INIT_HLIST_NODE(&f->by_id);

	spin_lock_bh(&lif->rx_filters.lock);

	key = hash_32(key, IONIC_RX_FILTER_HASH_BITS);
	head = &lif->rx_filters.by_hash[key];
	hlist_add_head(&f->by_hash, head);

	key = f->filter_id & IONIC_RX_FILTER_HLISTS_MASK;
	head = &lif->rx_filters.by_id[key];
	hlist_add_head(&f->by_id, head);

	spin_unlock_bh(&lif->rx_filters.lock);

	return 0;
}

struct ionic_rx_filter *ionic_rx_filter_by_vlan(struct ionic_lif *lif, u16 vid)
{
	struct ionic_rx_filter *f;
	struct hlist_head *head;
	unsigned int key;

	key = hash_32(vid, IONIC_RX_FILTER_HASH_BITS);
	head = &lif->rx_filters.by_hash[key];

	hlist_for_each_entry(f, head, by_hash) {
		if (le16_to_cpu(f->cmd.match) != IONIC_RX_FILTER_MATCH_VLAN)
			continue;
		if (le16_to_cpu(f->cmd.vlan.vlan) == vid)
			return f;
	}

	return NULL;
}

struct ionic_rx_filter *ionic_rx_filter_by_addr(struct ionic_lif *lif,
						const u8 *addr)
{
	struct ionic_rx_filter *f;
	struct hlist_head *head;
	unsigned int key;

	key = hash_32(*(u32 *)addr, IONIC_RX_FILTER_HASH_BITS);
	head = &lif->rx_filters.by_hash[key];

	hlist_for_each_entry(f, head, by_hash) {
		if (le16_to_cpu(f->cmd.match) != IONIC_RX_FILTER_MATCH_MAC)
			continue;
		if (memcmp(addr, f->cmd.mac.addr, ETH_ALEN) == 0)
			return f;
	}

	return NULL;
}
