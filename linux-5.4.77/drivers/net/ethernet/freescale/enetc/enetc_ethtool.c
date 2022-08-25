// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/* Copyright 2017-2019 NXP */

#include <linux/net_tstamp.h>
#include <linux/module.h>
#include "enetc.h"

static const u32 enetc_si_regs[] = {
	ENETC_SIMR, ENETC_SIPMAR0, ENETC_SIPMAR1, ENETC_SICBDRMR,
	ENETC_SICBDRSR,	ENETC_SICBDRBAR0, ENETC_SICBDRBAR1, ENETC_SICBDRPIR,
	ENETC_SICBDRCIR, ENETC_SICBDRLENR, ENETC_SICAPR0, ENETC_SICAPR1,
	ENETC_SIUEFDCR
};

static const u32 enetc_txbdr_regs[] = {
	ENETC_TBMR, ENETC_TBSR, ENETC_TBBAR0, ENETC_TBBAR1,
	ENETC_TBPIR, ENETC_TBCIR, ENETC_TBLENR, ENETC_TBIER
};

static const u32 enetc_rxbdr_regs[] = {
	ENETC_RBMR, ENETC_RBSR, ENETC_RBBSR, ENETC_RBCIR, ENETC_RBBAR0,
	ENETC_RBBAR1, ENETC_RBPIR, ENETC_RBLENR, ENETC_RBICIR0, ENETC_RBIER
};

static const u32 enetc_port_regs[] = {
	ENETC_PMR, ENETC_PSR, ENETC_PSIPMR, ENETC_PSIPMAR0(0),
	ENETC_PSIPMAR1(0), ENETC_PTXMBAR, ENETC_PCAPR0, ENETC_PCAPR1,
	ENETC_PSICFGR0(0), ENETC_PRFSCAPR, ENETC_PTCMSDUR(0),
	ENETC_PM0_CMD_CFG, ENETC_PM0_MAXFRM, ENETC_PM0_IF_MODE
};

static int enetc_get_reglen(struct net_device *ndev)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_hw *hw = &priv->si->hw;
	int len;

	len = ARRAY_SIZE(enetc_si_regs);
	len += ARRAY_SIZE(enetc_txbdr_regs) * priv->num_tx_rings;
	len += ARRAY_SIZE(enetc_rxbdr_regs) * priv->num_rx_rings;

	if (hw->port)
		len += ARRAY_SIZE(enetc_port_regs);

	len *= sizeof(u32) * 2; /* store 2 entries per reg: addr and value */

	return len;
}

static void enetc_get_regs(struct net_device *ndev, struct ethtool_regs *regs,
			   void *regbuf)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_hw *hw = &priv->si->hw;
	u32 *buf = (u32 *)regbuf;
	int i, j;
	u32 addr;

	for (i = 0; i < ARRAY_SIZE(enetc_si_regs); i++) {
		*buf++ = enetc_si_regs[i];
		*buf++ = enetc_rd(hw, enetc_si_regs[i]);
	}

	for (i = 0; i < priv->num_tx_rings; i++) {
		for (j = 0; j < ARRAY_SIZE(enetc_txbdr_regs); j++) {
			addr = ENETC_BDR(TX, i, enetc_txbdr_regs[j]);

			*buf++ = addr;
			*buf++ = enetc_rd(hw, addr);
		}
	}

	for (i = 0; i < priv->num_rx_rings; i++) {
		for (j = 0; j < ARRAY_SIZE(enetc_rxbdr_regs); j++) {
			addr = ENETC_BDR(RX, i, enetc_rxbdr_regs[j]);

			*buf++ = addr;
			*buf++ = enetc_rd(hw, addr);
		}
	}

	if (!hw->port)
		return;

	for (i = 0; i < ARRAY_SIZE(enetc_port_regs); i++) {
		addr = ENETC_PORT_BASE + enetc_port_regs[i];
		*buf++ = addr;
		*buf++ = enetc_rd(hw, addr);
	}
}

static const struct {
	int reg;
	char name[ETH_GSTRING_LEN];
} enetc_si_counters[] =  {
	{ ENETC_SIROCT, "SI rx octets" },
	{ ENETC_SIRFRM, "SI rx frames" },
	{ ENETC_SIRUCA, "SI rx u-cast frames" },
	{ ENETC_SIRMCA, "SI rx m-cast frames" },
	{ ENETC_SITOCT, "SI tx octets" },
	{ ENETC_SITFRM, "SI tx frames" },
	{ ENETC_SITUCA, "SI tx u-cast frames" },
	{ ENETC_SITMCA, "SI tx m-cast frames" },
	{ ENETC_RBDCR(0), "Rx ring  0 discarded frames" },
	{ ENETC_RBDCR(1), "Rx ring  1 discarded frames" },
	{ ENETC_RBDCR(2), "Rx ring  2 discarded frames" },
	{ ENETC_RBDCR(3), "Rx ring  3 discarded frames" },
	{ ENETC_RBDCR(4), "Rx ring  4 discarded frames" },
	{ ENETC_RBDCR(5), "Rx ring  5 discarded frames" },
	{ ENETC_RBDCR(6), "Rx ring  6 discarded frames" },
	{ ENETC_RBDCR(7), "Rx ring  7 discarded frames" },
	{ ENETC_RBDCR(8), "Rx ring  8 discarded frames" },
	{ ENETC_RBDCR(9), "Rx ring  9 discarded frames" },
	{ ENETC_RBDCR(10), "Rx ring 10 discarded frames" },
	{ ENETC_RBDCR(11), "Rx ring 11 discarded frames" },
	{ ENETC_RBDCR(12), "Rx ring 12 discarded frames" },
	{ ENETC_RBDCR(13), "Rx ring 13 discarded frames" },
	{ ENETC_RBDCR(14), "Rx ring 14 discarded frames" },
	{ ENETC_RBDCR(15), "Rx ring 15 discarded frames" },
};

static const struct {
	int reg;
	char name[ETH_GSTRING_LEN];
} enetc_port_counters[] = {
	{ ENETC_PM0_REOCT,  "MAC rx ethernet octets" },
	{ ENETC_PM0_RALN,   "MAC rx alignment errors" },
	{ ENETC_PM0_RXPF,   "MAC rx valid pause frames" },
	{ ENETC_PM0_RFRM,   "MAC rx valid frames" },
	{ ENETC_PM0_RFCS,   "MAC rx fcs errors" },
	{ ENETC_PM0_RVLAN,  "MAC rx VLAN frames" },
	{ ENETC_PM0_RERR,   "MAC rx frame errors" },
	{ ENETC_PM0_RUCA,   "MAC rx unicast frames" },
	{ ENETC_PM0_RMCA,   "MAC rx multicast frames" },
	{ ENETC_PM0_RBCA,   "MAC rx broadcast frames" },
	{ ENETC_PM0_RDRP,   "MAC rx dropped packets" },
	{ ENETC_PM0_RPKT,   "MAC rx packets" },
	{ ENETC_PM0_RUND,   "MAC rx undersized packets" },
	{ ENETC_PM0_R64,    "MAC rx 64 byte packets" },
	{ ENETC_PM0_R127,   "MAC rx 65-127 byte packets" },
	{ ENETC_PM0_R255,   "MAC rx 128-255 byte packets" },
	{ ENETC_PM0_R511,   "MAC rx 256-511 byte packets" },
	{ ENETC_PM0_R1023,  "MAC rx 512-1023 byte packets" },
	{ ENETC_PM0_R1518,  "MAC rx 1024-1518 byte packets" },
	{ ENETC_PM0_R1519X, "MAC rx 1519 to max-octet packets" },
	{ ENETC_PM0_ROVR,   "MAC rx oversized packets" },
	{ ENETC_PM0_RJBR,   "MAC rx jabber packets" },
	{ ENETC_PM0_RFRG,   "MAC rx fragment packets" },
	{ ENETC_PM0_RCNP,   "MAC rx control packets" },
	{ ENETC_PM0_RDRNTP, "MAC rx fifo drop" },
	{ ENETC_PM0_TEOCT,  "MAC tx ethernet octets" },
	{ ENETC_PM0_TOCT,   "MAC tx octets" },
	{ ENETC_PM0_TCRSE,  "MAC tx carrier sense errors" },
	{ ENETC_PM0_TXPF,   "MAC tx valid pause frames" },
	{ ENETC_PM0_TFRM,   "MAC tx frames" },
	{ ENETC_PM0_TFCS,   "MAC tx fcs errors" },
	{ ENETC_PM0_TVLAN,  "MAC tx VLAN frames" },
	{ ENETC_PM0_TERR,   "MAC tx frames" },
	{ ENETC_PM0_TUCA,   "MAC tx unicast frames" },
	{ ENETC_PM0_TMCA,   "MAC tx multicast frames" },
	{ ENETC_PM0_TBCA,   "MAC tx broadcast frames" },
	{ ENETC_PM0_TPKT,   "MAC tx packets" },
	{ ENETC_PM0_TUND,   "MAC tx undersized packets" },
	{ ENETC_PM0_T127,   "MAC tx 65-127 byte packets" },
	{ ENETC_PM0_T1023,  "MAC tx 512-1023 byte packets" },
	{ ENETC_PM0_T1518,  "MAC tx 1024-1518 byte packets" },
	{ ENETC_PM0_TCNP,   "MAC tx control packets" },
	{ ENETC_PM0_TDFR,   "MAC tx deferred packets" },
	{ ENETC_PM0_TMCOL,  "MAC tx multiple collisions" },
	{ ENETC_PM0_TSCOL,  "MAC tx single collisions" },
	{ ENETC_PM0_TLCOL,  "MAC tx late collisions" },
	{ ENETC_PM0_TECOL,  "MAC tx excessive collisions" },
	{ ENETC_UFDMF,      "SI MAC nomatch u-cast discards" },
	{ ENETC_MFDMF,      "SI MAC nomatch m-cast discards" },
	{ ENETC_PBFDSIR,    "SI MAC nomatch b-cast discards" },
	{ ENETC_PUFDVFR,    "SI VLAN nomatch u-cast discards" },
	{ ENETC_PMFDVFR,    "SI VLAN nomatch m-cast discards" },
	{ ENETC_PBFDVFR,    "SI VLAN nomatch b-cast discards" },
	{ ENETC_PFDMSAPR,   "SI pruning discarded frames" },
	{ ENETC_PICDR(0),   "ICM DR0 discarded frames" },
	{ ENETC_PICDR(1),   "ICM DR1 discarded frames" },
	{ ENETC_PICDR(2),   "ICM DR2 discarded frames" },
	{ ENETC_PICDR(3),   "ICM DR3 discarded frames" },
};

static const char rx_ring_stats[][ETH_GSTRING_LEN] = {
	"Rx ring %2d frames",
	"Rx ring %2d alloc errors",
};

static const char tx_ring_stats[][ETH_GSTRING_LEN] = {
	"Tx ring %2d frames",
};

static int enetc_get_sset_count(struct net_device *ndev, int sset)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);

	if (sset == ETH_SS_STATS)
		return ARRAY_SIZE(enetc_si_counters) +
			ARRAY_SIZE(tx_ring_stats) * priv->num_tx_rings +
			ARRAY_SIZE(rx_ring_stats) * priv->num_rx_rings +
			(enetc_si_is_pf(priv->si) ?
			ARRAY_SIZE(enetc_port_counters) : 0);

	return -EOPNOTSUPP;
}

static void enetc_get_strings(struct net_device *ndev, u32 stringset, u8 *data)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	u8 *p = data;
	int i, j;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(enetc_si_counters); i++) {
			strlcpy(p, enetc_si_counters[i].name, ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		for (i = 0; i < priv->num_tx_rings; i++) {
			for (j = 0; j < ARRAY_SIZE(tx_ring_stats); j++) {
				snprintf(p, ETH_GSTRING_LEN, tx_ring_stats[j],
					 i);
				p += ETH_GSTRING_LEN;
			}
		}
		for (i = 0; i < priv->num_rx_rings; i++) {
			for (j = 0; j < ARRAY_SIZE(rx_ring_stats); j++) {
				snprintf(p, ETH_GSTRING_LEN, rx_ring_stats[j],
					 i);
				p += ETH_GSTRING_LEN;
			}
		}

		if (!enetc_si_is_pf(priv->si))
			break;

		for (i = 0; i < ARRAY_SIZE(enetc_port_counters); i++) {
			strlcpy(p, enetc_port_counters[i].name,
				ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	}
}

static void enetc_get_ethtool_stats(struct net_device *ndev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_hw *hw = &priv->si->hw;
	int i, o = 0;

	for (i = 0; i < ARRAY_SIZE(enetc_si_counters); i++)
		data[o++] = enetc_rd64(hw, enetc_si_counters[i].reg);

	for (i = 0; i < priv->num_tx_rings; i++)
		data[o++] = priv->tx_ring[i]->stats.packets;

	for (i = 0; i < priv->num_rx_rings; i++) {
		data[o++] = priv->rx_ring[i]->stats.packets;
		data[o++] = priv->rx_ring[i]->stats.rx_alloc_errs;
	}

	if (!enetc_si_is_pf(priv->si))
		return;

	for (i = 0; i < ARRAY_SIZE(enetc_port_counters); i++)
		data[o++] = enetc_port_rd(hw, enetc_port_counters[i].reg);
}

#define ENETC_RSSHASH_L3 (RXH_L2DA | RXH_VLAN | RXH_L3_PROTO | RXH_IP_SRC | \
			  RXH_IP_DST)
#define ENETC_RSSHASH_L4 (ENETC_RSSHASH_L3 | RXH_L4_B_0_1 | RXH_L4_B_2_3)
static int enetc_get_rsshash(struct ethtool_rxnfc *rxnfc)
{
	static const u32 rsshash[] = {
			[TCP_V4_FLOW]    = ENETC_RSSHASH_L4,
			[UDP_V4_FLOW]    = ENETC_RSSHASH_L4,
			[SCTP_V4_FLOW]   = ENETC_RSSHASH_L4,
			[AH_ESP_V4_FLOW] = ENETC_RSSHASH_L3,
			[IPV4_FLOW]      = ENETC_RSSHASH_L3,
			[TCP_V6_FLOW]    = ENETC_RSSHASH_L4,
			[UDP_V6_FLOW]    = ENETC_RSSHASH_L4,
			[SCTP_V6_FLOW]   = ENETC_RSSHASH_L4,
			[AH_ESP_V6_FLOW] = ENETC_RSSHASH_L3,
			[IPV6_FLOW]      = ENETC_RSSHASH_L3,
			[ETHER_FLOW]     = 0,
	};

	if (rxnfc->flow_type >= ARRAY_SIZE(rsshash))
		return -EINVAL;

	rxnfc->data = rsshash[rxnfc->flow_type];

	return 0;
}

/* current HW spec does byte reversal on everything including MAC addresses */
static void ether_addr_copy_swap(u8 *dst, const u8 *src)
{
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		dst[i] = src[ETH_ALEN - i - 1];
}

static int enetc_set_cls_entry(struct enetc_si *si,
			       struct ethtool_rx_flow_spec *fs, bool en)
{
	struct ethtool_tcpip4_spec *l4ip4_h, *l4ip4_m;
	struct ethtool_usrip4_spec *l3ip4_h, *l3ip4_m;
	struct ethhdr *eth_h, *eth_m;
	struct enetc_cmd_rfse rfse = { {0} };

	if (!en)
		goto done;

	switch (fs->flow_type & 0xff) {
	case TCP_V4_FLOW:
		l4ip4_h = &fs->h_u.tcp_ip4_spec;
		l4ip4_m = &fs->m_u.tcp_ip4_spec;
		goto l4ip4;
	case UDP_V4_FLOW:
		l4ip4_h = &fs->h_u.udp_ip4_spec;
		l4ip4_m = &fs->m_u.udp_ip4_spec;
		goto l4ip4;
	case SCTP_V4_FLOW:
		l4ip4_h = &fs->h_u.sctp_ip4_spec;
		l4ip4_m = &fs->m_u.sctp_ip4_spec;
l4ip4:
		rfse.sip_h[0] = l4ip4_h->ip4src;
		rfse.sip_m[0] = l4ip4_m->ip4src;
		rfse.dip_h[0] = l4ip4_h->ip4dst;
		rfse.dip_m[0] = l4ip4_m->ip4dst;
		rfse.sport_h = ntohs(l4ip4_h->psrc);
		rfse.sport_m = ntohs(l4ip4_m->psrc);
		rfse.dport_h = ntohs(l4ip4_h->pdst);
		rfse.dport_m = ntohs(l4ip4_m->pdst);
		if (l4ip4_m->tos)
			netdev_warn(si->ndev, "ToS field is not supported and was ignored\n");
		rfse.ethtype_h = ETH_P_IP; /* IPv4 */
		rfse.ethtype_m = 0xffff;
		break;
	case IP_USER_FLOW:
		l3ip4_h = &fs->h_u.usr_ip4_spec;
		l3ip4_m = &fs->m_u.usr_ip4_spec;

		rfse.sip_h[0] = l3ip4_h->ip4src;
		rfse.sip_m[0] = l3ip4_m->ip4src;
		rfse.dip_h[0] = l3ip4_h->ip4dst;
		rfse.dip_m[0] = l3ip4_m->ip4dst;
		if (l3ip4_m->tos)
			netdev_warn(si->ndev, "ToS field is not supported and was ignored\n");
		rfse.ethtype_h = ETH_P_IP; /* IPv4 */
		rfse.ethtype_m = 0xffff;
		break;
	case ETHER_FLOW:
		eth_h = &fs->h_u.ether_spec;
		eth_m = &fs->m_u.ether_spec;

		ether_addr_copy_swap(rfse.smac_h, eth_h->h_source);
		ether_addr_copy_swap(rfse.smac_m, eth_m->h_source);
		ether_addr_copy_swap(rfse.dmac_h, eth_h->h_dest);
		ether_addr_copy_swap(rfse.dmac_m, eth_m->h_dest);
		rfse.ethtype_h = ntohs(eth_h->h_proto);
		rfse.ethtype_m = ntohs(eth_m->h_proto);
		break;
	default:
		return -EOPNOTSUPP;
	}

	rfse.mode |= ENETC_RFSE_EN;
	if (fs->ring_cookie != RX_CLS_FLOW_DISC) {
		rfse.mode |= ENETC_RFSE_MODE_BD;
		rfse.result = fs->ring_cookie;
	}
done:
	return enetc_set_fs_entry(si, &rfse, fs->location);
}

static int enetc_get_rxnfc(struct net_device *ndev, struct ethtool_rxnfc *rxnfc,
			   u32 *rule_locs)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	int i, j;

	switch (rxnfc->cmd) {
	case ETHTOOL_GRXRINGS:
		rxnfc->data = priv->num_rx_rings;
		break;
	case ETHTOOL_GRXFH:
		/* get RSS hash config */
		return enetc_get_rsshash(rxnfc);
	case ETHTOOL_GRXCLSRLCNT:
		/* total number of entries */
		rxnfc->data = priv->si->num_fs_entries;
		/* number of entries in use */
		rxnfc->rule_cnt = 0;
		for (i = 0; i < priv->si->num_fs_entries; i++)
			if (priv->cls_rules[i].used)
				rxnfc->rule_cnt++;
		break;
	case ETHTOOL_GRXCLSRULE:
		if (rxnfc->fs.location >= priv->si->num_fs_entries)
			return -EINVAL;

		/* get entry x */
		rxnfc->fs = priv->cls_rules[rxnfc->fs.location].fs;
		break;
	case ETHTOOL_GRXCLSRLALL:
		/* total number of entries */
		rxnfc->data = priv->si->num_fs_entries;
		/* array of indexes of used entries */
		j = 0;
		for (i = 0; i < priv->si->num_fs_entries; i++) {
			if (!priv->cls_rules[i].used)
				continue;
			if (j == rxnfc->rule_cnt)
				return -EMSGSIZE;
			rule_locs[j++] = i;
		}
		/* number of entries in use */
		rxnfc->rule_cnt = j;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int enetc_set_rxnfc(struct net_device *ndev, struct ethtool_rxnfc *rxnfc)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	int err;

	switch (rxnfc->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		if (rxnfc->fs.location >= priv->si->num_fs_entries)
			return -EINVAL;

		if (rxnfc->fs.ring_cookie >= priv->num_rx_rings &&
		    rxnfc->fs.ring_cookie != RX_CLS_FLOW_DISC)
			return -EINVAL;

		err = enetc_set_cls_entry(priv->si, &rxnfc->fs, true);
		if (err)
			return err;
		priv->cls_rules[rxnfc->fs.location].fs = rxnfc->fs;
		priv->cls_rules[rxnfc->fs.location].used = 1;
		break;
	case ETHTOOL_SRXCLSRLDEL:
		if (rxnfc->fs.location >= priv->si->num_fs_entries)
			return -EINVAL;

		err = enetc_set_cls_entry(priv->si, &rxnfc->fs, false);
		if (err)
			return err;
		priv->cls_rules[rxnfc->fs.location].used = 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static u32 enetc_get_rxfh_key_size(struct net_device *ndev)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);

	/* return the size of the RX flow hash key.  PF only */
	return (priv->si->hw.port) ? ENETC_RSSHASH_KEY_SIZE : 0;
}

static u32 enetc_get_rxfh_indir_size(struct net_device *ndev)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);

	/* return the size of the RX flow hash indirection table */
	return priv->si->num_rss;
}

static int enetc_get_rxfh(struct net_device *ndev, u32 *indir, u8 *key,
			  u8 *hfunc)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_hw *hw = &priv->si->hw;
	int err = 0, i;

	/* return hash function */
	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;

	/* return hash key */
	if (key && hw->port)
		for (i = 0; i < ENETC_RSSHASH_KEY_SIZE / 4; i++)
			((u32 *)key)[i] = enetc_port_rd(hw, ENETC_PRSSK(i));

	/* return RSS table */
	if (indir)
		err = enetc_get_rss_table(priv->si, indir, priv->si->num_rss);

	return err;
}

void enetc_set_rss_key(struct enetc_hw *hw, const u8 *bytes)
{
	int i;

	for (i = 0; i < ENETC_RSSHASH_KEY_SIZE / 4; i++)
		enetc_port_wr(hw, ENETC_PRSSK(i), ((u32 *)bytes)[i]);
}

static int enetc_set_rxfh(struct net_device *ndev, const u32 *indir,
			  const u8 *key, const u8 hfunc)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_hw *hw = &priv->si->hw;
	int err = 0;

	/* set hash key, if PF */
	if (key && hw->port)
		enetc_set_rss_key(hw, key);

	/* set RSS table */
	if (indir)
		err = enetc_set_rss_table(priv->si, indir, priv->si->num_rss);

	return err;
}

static void enetc_get_ringparam(struct net_device *ndev,
				struct ethtool_ringparam *ring)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);

	ring->rx_pending = priv->rx_bd_count;
	ring->tx_pending = priv->tx_bd_count;

	/* do some h/w sanity checks for BDR length */
	if (netif_running(ndev)) {
		struct enetc_hw *hw = &priv->si->hw;
		u32 val = enetc_rxbdr_rd(hw, 0, ENETC_RBLENR);

		if (val != priv->rx_bd_count)
			netif_err(priv, hw, ndev, "RxBDR[RBLENR] = %d!\n", val);

		val = enetc_txbdr_rd(hw, 0, ENETC_TBLENR);

		if (val != priv->tx_bd_count)
			netif_err(priv, hw, ndev, "TxBDR[TBLENR] = %d!\n", val);
	}
}

static int enetc_get_ts_info(struct net_device *ndev,
			     struct ethtool_ts_info *info)
{
	int *phc_idx;

	phc_idx = symbol_get(enetc_phc_index);
	if (phc_idx) {
		info->phc_index = *phc_idx;
		symbol_put(enetc_phc_index);
	} else {
		info->phc_index = -1;
	}

#ifdef CONFIG_FSL_ENETC_HW_TIMESTAMPING
	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;

	info->tx_types = (1 << HWTSTAMP_TX_OFF) |
			 (1 << HWTSTAMP_TX_ON);
	info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
			   (1 << HWTSTAMP_FILTER_ALL);
#else
	info->so_timestamping = SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE;
#endif
	return 0;
}

static const struct ethtool_ops enetc_pf_ethtool_ops = {
	.get_regs_len = enetc_get_reglen,
	.get_regs = enetc_get_regs,
	.get_sset_count = enetc_get_sset_count,
	.get_strings = enetc_get_strings,
	.get_ethtool_stats = enetc_get_ethtool_stats,
	.get_rxnfc = enetc_get_rxnfc,
	.set_rxnfc = enetc_set_rxnfc,
	.get_rxfh_key_size = enetc_get_rxfh_key_size,
	.get_rxfh_indir_size = enetc_get_rxfh_indir_size,
	.get_rxfh = enetc_get_rxfh,
	.set_rxfh = enetc_set_rxfh,
	.get_ringparam = enetc_get_ringparam,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
	.get_link = ethtool_op_get_link,
	.get_ts_info = enetc_get_ts_info,
};

static const struct ethtool_ops enetc_vf_ethtool_ops = {
	.get_regs_len = enetc_get_reglen,
	.get_regs = enetc_get_regs,
	.get_sset_count = enetc_get_sset_count,
	.get_strings = enetc_get_strings,
	.get_ethtool_stats = enetc_get_ethtool_stats,
	.get_rxnfc = enetc_get_rxnfc,
	.set_rxnfc = enetc_set_rxnfc,
	.get_rxfh_indir_size = enetc_get_rxfh_indir_size,
	.get_rxfh = enetc_get_rxfh,
	.set_rxfh = enetc_set_rxfh,
	.get_ringparam = enetc_get_ringparam,
	.get_link = ethtool_op_get_link,
	.get_ts_info = enetc_get_ts_info,
};

void enetc_set_ethtool_ops(struct net_device *ndev)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);

	if (enetc_si_is_pf(priv->si))
		ndev->ethtool_ops = &enetc_pf_ethtool_ops;
	else
		ndev->ethtool_ops = &enetc_vf_ethtool_ops;
}
