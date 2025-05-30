// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2007 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Description: QE UCC Gigabit Ethernet Ethtool API Set
 *
 * Author: Li Yang <leoli@freescale.com>
 *
 * Limitation:
 * Can only get/set settings of the first queue.
 * Need to re-open the interface manually after changing some parameters.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/phy.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <linux/uaccess.h>
#include <asm/types.h>

#include "ucc_geth.h"

static const char hw_stat_gstrings[][ETH_GSTRING_LEN] = {
	"tx-64-frames",
	"tx-65-127-frames",
	"tx-128-255-frames",
	"rx-64-frames",
	"rx-65-127-frames",
	"rx-128-255-frames",
	"tx-bytes-ok",
	"tx-pause-frames",
	"tx-multicast-frames",
	"tx-broadcast-frames",
	"rx-frames",
	"rx-bytes-ok",
	"rx-bytes-all",
	"rx-multicast-frames",
	"rx-broadcast-frames",
	"stats-counter-carry",
	"stats-counter-mask",
	"rx-dropped-frames",
};

static const char tx_fw_stat_gstrings[][ETH_GSTRING_LEN] = {
	"tx-single-collision",
	"tx-multiple-collision",
	"tx-late-collision",
	"tx-aborted-frames",
	"tx-lost-frames",
	"tx-carrier-sense-errors",
	"tx-frames-ok",
	"tx-excessive-differ-frames",
	"tx-256-511-frames",
	"tx-512-1023-frames",
	"tx-1024-1518-frames",
	"tx-jumbo-frames",
};

static const char rx_fw_stat_gstrings[][ETH_GSTRING_LEN] = {
	"rx-crc-errors",
	"rx-alignment-errors",
	"rx-in-range-length-errors",
	"rx-out-of-range-length-errors",
	"rx-too-long-frames",
	"rx-runt",
	"rx-very-long-event",
	"rx-symbol-errors",
	"rx-busy-drop-frames",
	"reserved",
	"reserved",
	"rx-mismatch-drop-frames",
	"rx-small-than-64",
	"rx-256-511-frames",
	"rx-512-1023-frames",
	"rx-1024-1518-frames",
	"rx-jumbo-frames",
	"rx-mac-error-loss",
	"rx-pause-frames",
	"reserved",
	"rx-vlan-removed",
	"rx-vlan-replaced",
	"rx-vlan-inserted",
	"rx-ip-checksum-errors",
};

#define UEC_HW_STATS_LEN ARRAY_SIZE(hw_stat_gstrings)
#define UEC_TX_FW_STATS_LEN ARRAY_SIZE(tx_fw_stat_gstrings)
#define UEC_RX_FW_STATS_LEN ARRAY_SIZE(rx_fw_stat_gstrings)

static int
uec_get_ksettings(struct net_device *netdev, struct ethtool_link_ksettings *cmd)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);

	return phylink_ethtool_ksettings_get(ugeth->phylink, cmd);
}

static int
uec_set_ksettings(struct net_device *netdev,
		  const struct ethtool_link_ksettings *cmd)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);

	return phylink_ethtool_ksettings_set(ugeth->phylink, cmd);
}

static void
uec_get_pauseparam(struct net_device *netdev,
                     struct ethtool_pauseparam *pause)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);

	return phylink_ethtool_get_pauseparam(ugeth->phylink, pause);
}

static int
uec_set_pauseparam(struct net_device *netdev,
                     struct ethtool_pauseparam *pause)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);

	ugeth->ug_info->receiveFlowControl = pause->rx_pause;
	ugeth->ug_info->transmitFlowControl = pause->tx_pause;

	return phylink_ethtool_set_pauseparam(ugeth->phylink, pause);
}

static uint32_t
uec_get_msglevel(struct net_device *netdev)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	return ugeth->msg_enable;
}

static void
uec_set_msglevel(struct net_device *netdev, uint32_t data)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	ugeth->msg_enable = data;
}

static int
uec_get_regs_len(struct net_device *netdev)
{
	return sizeof(struct ucc_geth);
}

static void
uec_get_regs(struct net_device *netdev,
               struct ethtool_regs *regs, void *p)
{
	int i;
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	u32 __iomem *ug_regs = (u32 __iomem *)ugeth->ug_regs;
	u32 *buff = p;

	for (i = 0; i < sizeof(struct ucc_geth) / sizeof(u32); i++)
		buff[i] = in_be32(&ug_regs[i]);
}

static void
uec_get_ringparam(struct net_device *netdev,
		  struct ethtool_ringparam *ring,
		  struct kernel_ethtool_ringparam *kernel_ring,
		  struct netlink_ext_ack *extack)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	struct ucc_geth_info *ug_info = ugeth->ug_info;
	int queue = 0;

	ring->rx_max_pending = UCC_GETH_BD_RING_SIZE_MAX;
	ring->rx_mini_max_pending = UCC_GETH_BD_RING_SIZE_MAX;
	ring->rx_jumbo_max_pending = UCC_GETH_BD_RING_SIZE_MAX;
	ring->tx_max_pending = UCC_GETH_BD_RING_SIZE_MAX;

	ring->rx_pending = ug_info->bdRingLenRx[queue];
	ring->rx_mini_pending = ug_info->bdRingLenRx[queue];
	ring->rx_jumbo_pending = ug_info->bdRingLenRx[queue];
	ring->tx_pending = ug_info->bdRingLenTx[queue];
}

static int
uec_set_ringparam(struct net_device *netdev,
		  struct ethtool_ringparam *ring,
		  struct kernel_ethtool_ringparam *kernel_ring,
		  struct netlink_ext_ack *extack)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	struct ucc_geth_info *ug_info = ugeth->ug_info;
	int queue = 0, ret = 0;

	if (ring->rx_pending < UCC_GETH_RX_BD_RING_SIZE_MIN) {
		netdev_info(netdev, "RxBD ring size must be no smaller than %d\n",
			    UCC_GETH_RX_BD_RING_SIZE_MIN);
		return -EINVAL;
	}
	if (ring->rx_pending % UCC_GETH_RX_BD_RING_SIZE_ALIGNMENT) {
		netdev_info(netdev, "RxBD ring size must be multiple of %d\n",
			    UCC_GETH_RX_BD_RING_SIZE_ALIGNMENT);
		return -EINVAL;
	}
	if (ring->tx_pending < UCC_GETH_TX_BD_RING_SIZE_MIN) {
		netdev_info(netdev, "TxBD ring size must be no smaller than %d\n",
			    UCC_GETH_TX_BD_RING_SIZE_MIN);
		return -EINVAL;
	}

	if (netif_running(netdev))
		return -EBUSY;

	ug_info->bdRingLenRx[queue] = ring->rx_pending;
	ug_info->bdRingLenTx[queue] = ring->tx_pending;

	return ret;
}

static int uec_get_sset_count(struct net_device *netdev, int sset)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	u32 stats_mode = ugeth->ug_info->statisticsMode;
	int len = 0;

	switch (sset) {
	case ETH_SS_STATS:
		if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_HARDWARE)
			len += UEC_HW_STATS_LEN;
		if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_TX)
			len += UEC_TX_FW_STATS_LEN;
		if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_RX)
			len += UEC_RX_FW_STATS_LEN;

		return len;

	default:
		return -EOPNOTSUPP;
	}
}

static void uec_get_strings(struct net_device *netdev, u32 stringset, u8 *buf)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	u32 stats_mode = ugeth->ug_info->statisticsMode;
	int i;

	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_HARDWARE)
		for (i = 0; i < UEC_HW_STATS_LEN; i++)
			ethtool_puts(&buf, hw_stat_gstrings[i]);
	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_TX)
		for (i = 0; i < UEC_TX_FW_STATS_LEN; i++)
			ethtool_puts(&buf, tx_fw_stat_gstrings[i]);
	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_RX)
		for (i = 0; i < UEC_RX_FW_STATS_LEN; i++)
			ethtool_puts(&buf, rx_fw_stat_gstrings[i]);
}

static void uec_get_ethtool_stats(struct net_device *netdev,
		struct ethtool_stats *stats, uint64_t *data)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	u32 stats_mode = ugeth->ug_info->statisticsMode;
	u32 __iomem *base;
	int i, j = 0;

	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_HARDWARE) {
		if (ugeth->ug_regs)
			base = (u32 __iomem *)&ugeth->ug_regs->tx64;
		else
			base = NULL;

		for (i = 0; i < UEC_HW_STATS_LEN; i++)
			data[j++] = base ? in_be32(&base[i]) : 0;
	}
	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_TX) {
		base = (u32 __iomem *)ugeth->p_tx_fw_statistics_pram;
		for (i = 0; i < UEC_TX_FW_STATS_LEN; i++)
			data[j++] = base ? in_be32(&base[i]) : 0;
	}
	if (stats_mode & UCC_GETH_STATISTICS_GATHERING_MODE_FIRMWARE_RX) {
		base = (u32 __iomem *)ugeth->p_rx_fw_statistics_pram;
		for (i = 0; i < UEC_RX_FW_STATS_LEN; i++)
			data[j++] = base ? in_be32(&base[i]) : 0;
	}
}

/* Report driver information */
static void
uec_get_drvinfo(struct net_device *netdev,
                       struct ethtool_drvinfo *drvinfo)
{
	strscpy(drvinfo->driver, DRV_NAME, sizeof(drvinfo->driver));
	strscpy(drvinfo->bus_info, "QUICC ENGINE", sizeof(drvinfo->bus_info));
}

#ifdef CONFIG_PM

static void uec_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);

	phylink_ethtool_get_wol(ugeth->phylink, wol);

	if (qe_alive_during_sleep())
		wol->supported |= WAKE_MAGIC;

	wol->wolopts |= ugeth->wol_en;
}

static int uec_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ucc_geth_private *ugeth = netdev_priv(netdev);
	int ret = 0;

	ret = phylink_ethtool_set_wol(ugeth->phylink, wol);
	if (ret == -EOPNOTSUPP) {
		ugeth->phy_wol_en = 0;
	} else if (ret) {
		return ret;
	} else {
		ugeth->phy_wol_en = wol->wolopts;
		goto out;
	}

	/* If the PHY isn't handling the WoL and the MAC is asked to more than
	 * WAKE_MAGIC, error-out
	 */
	if (!ugeth->phy_wol_en &&
	    wol->wolopts & ~WAKE_MAGIC)
		return -EINVAL;

	if (wol->wolopts & WAKE_MAGIC &&
	    !qe_alive_during_sleep())
		return -EINVAL;

out:
	ugeth->wol_en = wol->wolopts;
	device_set_wakeup_enable(&netdev->dev, ugeth->wol_en);

	return 0;
}

#else
#define uec_get_wol NULL
#define uec_set_wol NULL
#endif /* CONFIG_PM */

static const struct ethtool_ops uec_ethtool_ops = {
	.get_drvinfo            = uec_get_drvinfo,
	.get_regs_len           = uec_get_regs_len,
	.get_regs               = uec_get_regs,
	.get_msglevel           = uec_get_msglevel,
	.set_msglevel           = uec_set_msglevel,
	.nway_reset             = phy_ethtool_nway_reset,
	.get_link               = ethtool_op_get_link,
	.get_ringparam          = uec_get_ringparam,
	.set_ringparam          = uec_set_ringparam,
	.get_pauseparam         = uec_get_pauseparam,
	.set_pauseparam         = uec_set_pauseparam,
	.get_sset_count		= uec_get_sset_count,
	.get_strings            = uec_get_strings,
	.get_ethtool_stats      = uec_get_ethtool_stats,
	.get_wol		= uec_get_wol,
	.set_wol		= uec_set_wol,
	.get_ts_info		= ethtool_op_get_ts_info,
	.get_link_ksettings	= uec_get_ksettings,
	.set_link_ksettings	= uec_set_ksettings,
};

void uec_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &uec_ethtool_ops;
}
