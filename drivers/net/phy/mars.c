// SPDX-License-Identifier: GPL-2.0

/* Driver for Centec PHYs
 * Author: liuht
 *
 * Copyright 2002-2021, Centec Networks (Suzhou) Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/io.h>
#include <asm/irq.h>
#include <linux/uaccess.h>
#include <linux/version.h>

 /* Mask used for ID comparisons */
#define CTC_PHY_ID_MASK             0xffffffff

/* Known PHY IDs */
#define CTC_PHY_ID_MARS1S_V1        0x00782013
#define CTC_PHY_ID_MARS1S           0x01E04013
#define CTC_PHY_ID_MARS1P_V1        0x00782011
#define CTC_PHY_ID_MARS1P           0x01E04011
#define CTC_PHY_IMASK                     0x12
#define CTC_PHY_IEVENT                    0x13

#define CTC_PHY_IMASK_INIT              0x6c00
#define CTC_PHY_IMASK_CLEAR             0x0000

#define CTC_PHY_REG_SPACE                    0
#define CTC_SDS_REG_SPACE                    1

/* Mars page register */
#define CTC_MARS_PAGE_REG               0xa000
#define CTC_MARS_CHIP_CFG_REG           0xa001

#define CTC_PHY_GLB_DISABLE                  0
#define CTC_PHY_GLB_ENABLE                   1

/* Mars specific status register */
#define CTC_MARS_SSREG                    0x11

/* Interrupt Enable Register */
#define CTC_MARS_INTR_REG                 0x12
/* WOL Event Interrupt Enable */
#define CTC_MARS_WOL_INTR               BIT(6)
/* Magic Packet MAC address registers */
#define CTC_MARS_MAGIC_PACKET_MAC_ADDR2 0xa007
#define CTC_MARS_MAGIC_PACKET_MAC_ADDR1 0xa008
#define CTC_MARS_MAGIC_PACKET_MAC_ADDR0 0xa009
/* Mars wol config register */
#define CTC_MARS_WOL_CFG_REG            0xa00a
/* WOL TYPE */
#define CTC_MARS_WOL_TYPE               BIT(0)
/* WOL Enable */
#define CTC_MARS_WOL_EN                 BIT(3)
/* WOL Event Interrupt Enable */
#define CTC_MARS_WOL_INTR_SEL           BIT(6)
/* WOL Pulse Width */
#define CTC_MARS_WOL_WIDTH1             BIT(1)
#define CTC_MARS_WOL_WIDTH2             BIT(2)
/* WOL Enable Flag: disable by default */
// #define CTC_MARS_WOL_ENABLE

static int g_port_type;
static int g_port_status;

enum mars_port_type_e {
	MARS_PORT_TYPE_UTP,
	MARS_PORT_TYPE_FIBER,
	MARS_PORT_TYPE_COMBO,
	MARS_PORT_TYPE_MAX
};

enum mars_wol_type_e {
	MARS_WOL_TYPE_LEVEL,
	MARS_WOL_TYPE_PULSE,
	MARS_WOL_TYPE_MAX
};

enum mars_wol_width_e {
	MARS_WOL_WIDTH_84MS,
	MARS_WOL_WIDTH_168MS,
	MARS_WOL_WIDTH_336MS,
	MARS_WOL_WIDTH_672MS,
	MARS_WOL_WIDTH_MAX
};

struct mars_wol_cfg_t {
	int enable;
	int type;
	int width;
};

static int mars_ext_read(struct phy_device *phydev, u32 regnum)
{
	int ret;

	ret = phy_write(phydev, 0x1e, regnum);
	if (ret < 0)
		return ret;

	return phy_read(phydev, 0x1f);
}

static int mars_ext_write(struct phy_device *phydev, u32 regnum, u16 val)
{
	int ret;

	ret = phy_write(phydev, 0x1e, regnum);
	if (ret < 0)
		return ret;

	return phy_write(phydev, 0x1f, val);
}

static int mars_select_reg_space(struct phy_device *phydev, int space)
{
	int ret;

	if (space == CTC_PHY_REG_SPACE)
		ret = mars_ext_write(phydev, 0xa000, 0x0);
	else
		ret = mars_ext_write(phydev, 0xa000, 0x2);

	return ret;
}

static int mars_page_read(struct phy_device *phydev, int page, u32 regnum)
{
	int ret, oldpage, val;

	oldpage = mars_ext_read(phydev, CTC_MARS_PAGE_REG);
	if (oldpage < 0)
		return oldpage;

	ret = mars_select_reg_space(phydev, page);
	if (ret < 0)
		return ret;

	val = phy_read(phydev, regnum);
	/* Recover to old page */
	ret = mars_select_reg_space(phydev, (oldpage & 0x2));
	if (ret < 0)
		return ret;

	return val;
}

static int mars_page_write(struct phy_device *phydev, int page, u32 regnum,
			   u16 value)
{
	int ret, oldpage, val;

	oldpage = mars_ext_read(phydev, CTC_MARS_PAGE_REG);
	if (oldpage < 0)
		return oldpage;

	ret = mars_select_reg_space(phydev, page);
	if (ret < 0)
		return ret;

	val = phy_write(phydev, regnum, value);
	/* Recover to old page */
	ret = mars_select_reg_space(phydev, (oldpage & 0x2));
	if (ret < 0)
		return ret;

	return val;
}

static int mars_page_ext_write(struct phy_device *phydev, int page, u32 regnum,
			       u16 value)
{
	int ret, oldpage, val;

	oldpage = mars_ext_read(phydev, CTC_MARS_PAGE_REG);
	if (oldpage < 0)
		return oldpage;

	ret = mars_select_reg_space(phydev, page);
	if (ret < 0)
		return ret;

	val = mars_ext_write(phydev, regnum, value);
	/* Recover to old page */
	ret = mars_select_reg_space(phydev, (oldpage & 0x2));
	if (ret < 0)
		return ret;

	return val;
}

static int mars_setup_forced(struct phy_device *phydev)
{
	int ctl = 0;
	int ret = 0;

	if (g_port_type == MARS_PORT_TYPE_UTP ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		ctl = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMCR);
		if (ctl < 0)
			return ctl;
		ctl &= BMCR_LOOPBACK | BMCR_ISOLATE | BMCR_PDOWN;
		phydev->pause = 0;
		phydev->asym_pause = 0;

		if (phydev->speed == SPEED_1000)
			ctl |= BMCR_SPEED1000;
		else if (phydev->speed == SPEED_100)
			ctl |= BMCR_SPEED100;

		if (phydev->duplex == DUPLEX_FULL)
			ctl |= BMCR_FULLDPLX;

		ret = mars_page_write(phydev, CTC_PHY_REG_SPACE, MII_BMCR, ctl);
		if (ret < 0)
			return ret;
	}

	if (g_port_type == MARS_PORT_TYPE_FIBER ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		ctl = mars_page_read(phydev, CTC_SDS_REG_SPACE, MII_BMCR);
		if (ctl < 0)
			return ctl;
		ctl &= ~BMCR_ANENABLE;
		ret = mars_page_write(phydev, CTC_SDS_REG_SPACE, MII_BMCR, ctl);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int mars_restart_aneg(struct phy_device *phydev)
{
	int ctl = 0;
	int ret = 0;

	if (g_port_type == MARS_PORT_TYPE_UTP ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		ctl = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMCR);

		if (ctl < 0)
			return ctl;

		ctl |= BMCR_ANENABLE | BMCR_ANRESTART;

		/* Don't isolate the PHY if we're negotiating */
		ctl &= ~BMCR_ISOLATE;

		ret = mars_page_write(phydev, CTC_PHY_REG_SPACE, MII_BMCR, ctl);
		if (ret < 0)
			return ret;
	}

	if (g_port_type == MARS_PORT_TYPE_FIBER ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		ctl = mars_page_read(phydev, CTC_SDS_REG_SPACE, MII_BMCR);
		if (ctl < 0)
			return ctl;
		ctl |= BMCR_ANENABLE;
		ret = mars_page_write(phydev, CTC_SDS_REG_SPACE, MII_BMCR, ctl);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int mars_config_advert(struct phy_device *phydev)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	__ETHTOOL_DECLARE_LINK_MODE_MASK(advertise);
	u32 advertise_legacy;
#else
	u32 advertise;
#endif
	int oldadv, adv, bmsr;
	int err, changed = 0;

	/* Only allow advertising what this PHY supports */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	linkmode_and(phydev->advertising, phydev->advertising,
		     phydev->supported);
	linkmode_copy(advertise, phydev->advertising);
#else
	phydev->advertising &= phydev->supported;
	advertise = phydev->advertising;
#endif

	/* Setup standard advertisement */
	adv = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_ADVERTISE);
	if (adv < 0)
		return adv;

	oldadv = adv;
	adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP |
		 ADVERTISE_PAUSE_ASYM);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	ethtool_convert_link_mode_to_legacy_u32(&advertise_legacy, advertise);
	adv |= ethtool_adv_to_mii_adv_t(advertise_legacy);
#else
	adv |= ethtool_adv_to_mii_adv_t(advertise);
#endif

	if (adv != oldadv) {
		err =
		    mars_page_write(phydev, CTC_PHY_REG_SPACE, MII_ADVERTISE,
				    adv);

		if (err < 0)
			return err;
		changed = 1;
	}

	bmsr = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	/* Per 802.3-2008, Section 22.2.4.2.16 Extended status all
	 * 1000Mbits/sec capable PHYs shall have the BMSR_ESTATEN bit set to a
	 * logical 1.
	 */
	if (!(bmsr & BMSR_ESTATEN))
		return changed;

	/* Configure gigabit if it's supported */
	adv = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_CTRL1000);
	if (adv < 0)
		return adv;

	oldadv = adv;
	adv &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, phydev->supported) ||
	    linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported)) {
		ethtool_convert_link_mode_to_legacy_u32(&advertise_legacy,
							advertise);
		adv |= ethtool_adv_to_mii_ctrl1000_t(advertise_legacy);
	}
#else
	if (phydev->supported & (SUPPORTED_1000baseT_Half |
				 SUPPORTED_1000baseT_Full)) {
		adv |= ethtool_adv_to_mii_ctrl1000_t(advertise);
	}
#endif

	if (adv != oldadv)
		changed = 1;

	err = mars_page_write(phydev, CTC_PHY_REG_SPACE, MII_CTRL1000, adv);
	if (err < 0)
		return err;

	return changed;
}

int mars1s_config_aneg(struct phy_device *phydev)
{
	int err, changed = 0;
	int ret = 0;

	if (g_port_type == MARS_PORT_TYPE_UTP ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		if (phydev->autoneg != AUTONEG_ENABLE)
			ret = mars_setup_forced(phydev);
		if (ret < 0)
			return ret;

		err = mars_config_advert(phydev);
		if (err < 0)	/* error */
			return err;

		changed |= err;

		if (changed == 0) {
			/* Advertisement hasn't changed,
			 * but maybe aneg was never on to
			 * begin with?  Or maybe phy was isolated?
			 */
			int ctl =
			    mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMCR);

			if (ctl < 0)
				return ctl;

			if (!(ctl & BMCR_ANENABLE) || (ctl & BMCR_ISOLATE))
				changed = 1;	/* do restart aneg */
		}

		/* Only restart aneg if we are advertising something different
		 * than we were before.
		 */
		if (changed > 0)
			ret = mars_restart_aneg(phydev);
		if (ret < 0)
			return ret;
	}

	if (g_port_type == MARS_PORT_TYPE_FIBER ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		if (phydev->autoneg != AUTONEG_ENABLE) {
			ret = mars_setup_forced(phydev);
			return ret;
		}

		ret = mars_restart_aneg(phydev);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int mars_ack_interrupt(struct phy_device *phydev)
{
	int err;

	/* Clear the interrupts by reading the reg */
	err = mars_page_read(phydev, CTC_PHY_REG_SPACE, CTC_PHY_IEVENT);
	if (err < 0)
		return err;

	return 0;
}

static int mars_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		err =
		    mars_page_write(phydev, CTC_PHY_REG_SPACE, CTC_PHY_IMASK,
				    CTC_PHY_IMASK_INIT);
	else
		err =
		    mars_page_write(phydev, CTC_PHY_REG_SPACE, CTC_PHY_IMASK,
				    CTC_PHY_IMASK_CLEAR);
	return err;
}

static int mars_update_link(struct phy_device *phydev)
{
	int ret, status;

	if (g_port_type == MARS_PORT_TYPE_UTP ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		/* Do a fake read */
		status = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMSR);
		if (status < 0)
			return status;

		/* Read link and autonegotiation status */
		status = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMSR);
		if (status < 0)
			return status;

		if ((status & BMSR_LSTATUS) == 0) {
			phydev->link = 0;
		} else {
			phydev->link = 1;
			ret = mars_select_reg_space(phydev, CTC_PHY_REG_SPACE);
			if (ret < 0)
				return ret;

			g_port_status = MARS_PORT_TYPE_UTP;
			return 0;
		}
	}

	if (g_port_type == MARS_PORT_TYPE_FIBER ||
	    g_port_type == MARS_PORT_TYPE_COMBO) {
		/* Do a fake read */
		status = mars_page_read(phydev, CTC_SDS_REG_SPACE, MII_BMSR);
		if (status < 0)
			return status;

		/* Read link and autonegotiation status */
		status = mars_page_read(phydev, CTC_SDS_REG_SPACE, MII_BMSR);
		if (status < 0)
			return status;

		if ((status & BMSR_LSTATUS) == 0) {
			phydev->link = 0;
		} else {
			phydev->link = 1;
			ret = mars_select_reg_space(phydev, CTC_SDS_REG_SPACE);
			if (ret < 0)
				return ret;

			g_port_status = MARS_PORT_TYPE_FIBER;
			return 0;
		}
	}

	return 0;
}

static int mars_read_status(struct phy_device *phydev)
{
	int val = 0;
	int ret = 0;
	int lpa, page;

	/* Update the link, but return if there was an error */
	ret = mars_update_link(phydev);
	if (ret < 0)
		return ret;
	if (g_port_status)
		page = CTC_SDS_REG_SPACE;
	else
		page = CTC_PHY_REG_SPACE;

	phydev->speed = SPEED_10;
	phydev->duplex = DUPLEX_HALF;
	phydev->pause = 0;
	phydev->asym_pause = 0;

	val = mars_page_read(phydev, page, CTC_MARS_SSREG);
	if (val < 0)
		return val;

	lpa = mars_page_read(phydev, page, MII_LPA);
	if (lpa < 0)
		return lpa;

	if (val & 0x8000) {
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
	} else if (val & 0x4000) {
		phydev->speed = SPEED_100;
		if (val & 0x2000)
			phydev->duplex = DUPLEX_FULL;
	} else if (val & 0x2000) {
		phydev->duplex = DUPLEX_FULL;
	}

	if (phydev->duplex == DUPLEX_FULL) {
		phydev->pause = lpa & LPA_PAUSE_CAP ? 1 : 0;
		phydev->asym_pause = lpa & LPA_PAUSE_ASYM ? 1 : 0;
	}

	return 0;
}

static int mars_set_link_timer_2_6ms(struct phy_device *phydev)
{
	int ret = 0;

	ret = mars_page_ext_write(phydev, CTC_SDS_REG_SPACE, 0xa5, 0x5);

	return ret;
}

static int mars_wol_en_cfg(struct phy_device *phydev,
			   struct mars_wol_cfg_t wol_cfg)
{
	int ret = 0;
	int val = 0;

	val = mars_ext_read(phydev, CTC_MARS_WOL_CFG_REG);
	if (val < 0)
		return val;

	if (wol_cfg.enable) {
		val |= CTC_MARS_WOL_EN;

		if (wol_cfg.type == MARS_WOL_TYPE_LEVEL) {
			val &= ~CTC_MARS_WOL_TYPE;
			val &= ~CTC_MARS_WOL_INTR_SEL;
		} else if (wol_cfg.type == MARS_WOL_TYPE_PULSE) {
			val |= CTC_MARS_WOL_TYPE;
			val |= CTC_MARS_WOL_INTR_SEL;

			if (wol_cfg.width == MARS_WOL_WIDTH_84MS) {
				val &= ~CTC_MARS_WOL_WIDTH1;
				val &= ~CTC_MARS_WOL_WIDTH2;
			} else if (wol_cfg.width == MARS_WOL_WIDTH_168MS) {
				val |= CTC_MARS_WOL_WIDTH1;
				val &= ~CTC_MARS_WOL_WIDTH2;
			} else if (wol_cfg.width == MARS_WOL_WIDTH_336MS) {
				val &= ~CTC_MARS_WOL_WIDTH1;
				val |= CTC_MARS_WOL_WIDTH2;
			} else if (wol_cfg.width == MARS_WOL_WIDTH_672MS) {
				val |= CTC_MARS_WOL_WIDTH1;
				val |= CTC_MARS_WOL_WIDTH2;
			}
		}
	} else {
		val &= ~CTC_MARS_WOL_EN;
		val &= ~CTC_MARS_WOL_INTR_SEL;
	}

	ret = mars_ext_write(phydev, CTC_MARS_WOL_CFG_REG, val);
	if (ret < 0)
		return ret;

	return 0;
}

static void mars_get_wol(struct phy_device *phydev, struct ethtool_wolinfo *wol)
{
	int val = 0;

	wol->supported = WAKE_MAGIC;
	wol->wolopts = 0;

	val = mars_ext_read(phydev, CTC_MARS_WOL_CFG_REG);
	if (val < 0)
		return;

	if (val & CTC_MARS_WOL_EN)
		wol->wolopts |= WAKE_MAGIC;
}

static int mars_set_wol(struct phy_device *phydev, struct ethtool_wolinfo *wol)
{
	int ret, val;
	struct mars_wol_cfg_t wol_cfg;

	memset(&wol_cfg, 0, sizeof(struct mars_wol_cfg_t));

	if (wol->wolopts & WAKE_MAGIC) {
		/* Enable the WOL interrupt */
		val =
		    mars_page_read(phydev, CTC_PHY_REG_SPACE,
				   CTC_MARS_INTR_REG);
		val |= CTC_MARS_WOL_INTR;
		ret =
		    mars_page_write(phydev, CTC_PHY_REG_SPACE,
				    CTC_MARS_INTR_REG, val);
		if (ret < 0)
			return ret;

		/* Set the WOL config */
		wol_cfg.enable = CTC_PHY_GLB_ENABLE;
		wol_cfg.type = MARS_WOL_TYPE_PULSE;
		wol_cfg.width = MARS_WOL_WIDTH_672MS;
		ret = mars_wol_en_cfg(phydev, wol_cfg);
		if (ret < 0)
			return ret;

		/* Store the device address for the magic packet */
		ret = mars_ext_write(phydev, CTC_MARS_MAGIC_PACKET_MAC_ADDR2,
				     ((phydev->attached_dev->dev_addr[0] << 8) |
				      phydev->attached_dev->dev_addr[1]));
		if (ret < 0)
			return ret;
		ret = mars_ext_write(phydev, CTC_MARS_MAGIC_PACKET_MAC_ADDR1,
				     ((phydev->attached_dev->dev_addr[2] << 8) |
				      phydev->attached_dev->dev_addr[3]));
		if (ret < 0)
			return ret;
		ret = mars_ext_write(phydev, CTC_MARS_MAGIC_PACKET_MAC_ADDR0,
				     ((phydev->attached_dev->dev_addr[4] << 8) |
				      phydev->attached_dev->dev_addr[5]));
		if (ret < 0)
			return ret;
	} else {
		wol_cfg.enable = CTC_PHY_GLB_DISABLE;
		wol_cfg.type = MARS_WOL_TYPE_MAX;
		wol_cfg.width = MARS_WOL_WIDTH_MAX;
		ret = mars_wol_en_cfg(phydev, wol_cfg);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int mars_get_port_type(struct phy_device *phydev)
{
	int val;

	val = mars_ext_read(phydev, CTC_MARS_CHIP_CFG_REG);
	if (val < 0)
		return val;
	val &= 0x7;

	if (val == 0x0 || val == 0x3) {
		g_port_type = MARS_PORT_TYPE_UTP;
		g_port_status = MARS_PORT_TYPE_UTP;
	} else if (val == 0x1 || val == 0x4 || val == 0x5) {
		g_port_type = MARS_PORT_TYPE_FIBER;
		g_port_status = MARS_PORT_TYPE_FIBER;
	} else {
		g_port_type = MARS_PORT_TYPE_COMBO;
	}

	return 0;
}

int mars_config_init(struct phy_device *phydev)
{
	int val;
	u32 features;
#ifdef CTC_MARS_WOL_ENABLE
	struct ethtool_wolinfo wol;
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	__ETHTOOL_DECLARE_LINK_MODE_MASK(features_linkmode);
#endif

	mars_set_link_timer_2_6ms(phydev);

	features = (SUPPORTED_TP | SUPPORTED_MII
		    | SUPPORTED_AUI | SUPPORTED_FIBRE |
		    SUPPORTED_BNC | SUPPORTED_Pause | SUPPORTED_Asym_Pause);

	/* Do we support autonegotiation? */
	val = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_BMSR);
	if (val < 0)
		return val;

	if (val & BMSR_ANEGCAPABLE)
		features |= SUPPORTED_Autoneg;

	if (val & BMSR_100FULL)
		features |= SUPPORTED_100baseT_Full;
	if (val & BMSR_100HALF)
		features |= SUPPORTED_100baseT_Half;
	if (val & BMSR_10FULL)
		features |= SUPPORTED_10baseT_Full;
	if (val & BMSR_10HALF)
		features |= SUPPORTED_10baseT_Half;

	if (val & BMSR_ESTATEN) {
		val = mars_page_read(phydev, CTC_PHY_REG_SPACE, MII_ESTATUS);
		if (val < 0)
			return val;

		if (val & ESTATUS_1000_TFULL)
			features |= SUPPORTED_1000baseT_Full;
		if (val & ESTATUS_1000_THALF)
			features |= SUPPORTED_1000baseT_Half;
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
	ethtool_convert_legacy_u32_to_link_mode(features_linkmode, features);
	linkmode_and(phydev->supported, phydev->supported, features_linkmode);
	linkmode_and(phydev->advertising, phydev->supported, features_linkmode);
#else
	phydev->supported &= features;
	phydev->advertising &= features;
#endif
	mars_get_port_type(phydev);

#ifdef CTC_MARS_WOL_ENABLE
	wol.wolopts = 0;
	wol.supported = WAKE_MAGIC;
	wol.wolopts |= WAKE_MAGIC;
	mars_set_wol(phydev, &wol);
#endif

	return 0;
}

int mars1p_config_init(struct phy_device *phydev)
{
	/*RGMII clock 2.5M when link down, bit12:1->0 */
	mars_page_ext_write(phydev, CTC_PHY_REG_SPACE, 0xc, 0x8051);
	/*Disable sleep mode, bit15:1->0 */
	mars_page_ext_write(phydev, CTC_PHY_REG_SPACE, 0x27, 0x2029);
	/* disable PHY to respond to MDIO access with PHYAD0 */
	/* MMD7 8001h: bit6: 0, change value: 0x7f --> 0x3f */
	mars_page_write(phydev, CTC_PHY_REG_SPACE, 0xd, 0x7);
	mars_page_write(phydev, CTC_PHY_REG_SPACE, 0xe, 0x8001);
	mars_page_write(phydev, CTC_PHY_REG_SPACE, 0xd, 0x4007);
	mars_page_write(phydev, CTC_PHY_REG_SPACE, 0xe, 0x3f);

	return mars_config_init(phydev);
}

static struct phy_driver ctc_drivers[] = {
	{
	 .phy_id = CTC_PHY_ID_MARS1S,
	 .phy_id_mask = CTC_PHY_ID_MASK,
	 .name = "CTC MARS1S",
	 .config_init = mars_config_init,
	 .features = PHY_GBIT_FEATURES,
	 .config_aneg = mars1s_config_aneg,
	 .ack_interrupt = &mars_ack_interrupt,
	 .config_intr = &mars_config_intr,
	 .read_status = &mars_read_status,
	 .suspend = genphy_suspend,
	 .resume = genphy_resume,
	 .get_wol = &mars_get_wol,
	 .set_wol = &mars_set_wol,
	 },
	{
	 .phy_id = CTC_PHY_ID_MARS1S_V1,
	 .phy_id_mask = CTC_PHY_ID_MASK,
	 .name = "CTC MARS1S_V1",
	 .config_init = mars_config_init,
	 .features = PHY_GBIT_FEATURES,
	 .config_aneg = mars1s_config_aneg,
	 .ack_interrupt = &mars_ack_interrupt,
	 .config_intr = &mars_config_intr,
	 .read_status = &mars_read_status,
	 .suspend = genphy_suspend,
	 .resume = genphy_resume,
	 .get_wol = &mars_get_wol,
	 .set_wol = &mars_set_wol,
	 },
	{
	 .phy_id = CTC_PHY_ID_MARS1P,
	 .phy_id_mask = CTC_PHY_ID_MASK,
	 .name = "CTC MARS1P",
	 .config_init = mars1p_config_init,
	 .features = PHY_GBIT_FEATURES,
	 .config_aneg = mars1s_config_aneg,
	 .ack_interrupt = &mars_ack_interrupt,
	 .config_intr = &mars_config_intr,
	 .read_status = genphy_read_status,
	 .suspend = genphy_suspend,
	 .resume = genphy_resume,
	 },
	{
	 .phy_id = CTC_PHY_ID_MARS1P_V1,
	 .phy_id_mask = CTC_PHY_ID_MASK,
	 .name = "CTC MARS1P_V1",
	 .config_init = mars1p_config_init,
	 .features = PHY_GBIT_FEATURES,
	 .config_aneg = mars1s_config_aneg,
	 .ack_interrupt = &mars_ack_interrupt,
	 .config_intr = &mars_config_intr,
	 .read_status = genphy_read_status,
	 .suspend = genphy_suspend,
	 .resume = genphy_resume,
	 },
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0))
module_phy_driver(ctc_drivers);
#else
static int __init mars_init(void)
{
	return phy_drivers_register(ctc_drivers, ARRAY_SIZE(ctc_drivers));
}

static void __exit mars_exit(void)
{
	phy_drivers_unregister(ctc_drivers, ARRAY_SIZE(ctc_drivers));
}

module_init(mars_init);
module_exit(mars_exit);
#endif

static struct mdio_device_id __maybe_unused mars_tbl[] = {
	{CTC_PHY_ID_MARS1S, CTC_PHY_ID_MASK},
	{CTC_PHY_ID_MARS1S_V1, CTC_PHY_ID_MASK},
	{CTC_PHY_ID_MARS1P, CTC_PHY_ID_MASK},
	{CTC_PHY_ID_MARS1P_V1, CTC_PHY_ID_MASK},
	{}
};

MODULE_DEVICE_TABLE(mdio, mars_tbl);

MODULE_DESCRIPTION("Driver for Centec PHYs");
MODULE_AUTHOR("liuht <liuht@centecnetworks.com>");
MODULE_LICENSE("GPL v2");
