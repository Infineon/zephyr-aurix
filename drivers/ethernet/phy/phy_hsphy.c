/*
 * Copyright (c) 2024 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT infineon_tc4x_hsphy

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "IfxPms_reg.h"
#include "IfxHsphy_reg.h"

static int phy_hsphy_init(const struct device *dev)
{
	/* Enable HSPHY voltages*/
	// Enable VDDHSIF
	MODULE_PMS.VMONP.VDDHSIFCON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDHSIFSTAT.B.RESULT <= MODULE_PMS.VMONP.VDDHSIFSTAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddhsifrst;
	vddhsifrst.U = MODULE_PMS.VMONP.VDDHSIFRST.U;
	vddhsifrst.B.RESETOFF = 0;
	vddhsifrst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDHSIFRST.U = vddhsifrst.U;

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs0), okay)
	// Enable VDDPHY0
	MODULE_PMS.VMONP.VDDPHY0CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHY0STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHY0STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphy0rst;
	vddphy0rst.U = MODULE_PMS.VMONP.VDDPHY0RST.U;
	vddphy0rst.B.RESETOFF = 0;
	vddphy0rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHY0RST.U = vddphy0rst.U;
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs1), okay)
	// Enable VDDPHY1
	MODULE_PMS.VMONP.VDDPHY1CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHY1STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHY1STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphy1rst;
	vddphy1rst.U = MODULE_PMS.VMONP.VDDPHY1RST.U;
	vddphy1rst.B.RESETOFF = 0;
	vddphy1rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHY1RST.U = vddphy1rst.U;
#endif

#if 0
	// Enable VDDPHY2
	MODULE_PMS.VMONP.VDDPHY2CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHY2STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHY2STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphy2rst;
	vddphy2rst.U = MODULE_PMS.VMONP.VDDPHY2RST.U;
	vddphy2rst.B.RESETOFF = 0;
	vddphy2rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHY2RST.U = vddphy2rst.U;
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs0), okay)
	// Enable VDDPHPHY0
	MODULE_PMS.VMONP.VDDPHPHY0CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHPHY0STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHPHY0STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphphy0rst;
	vddphphy0rst.U = MODULE_PMS.VMONP.VDDPHPHY0RST.U;
	vddphphy0rst.B.RESETOFF = 0;
	vddphphy0rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHPHY0RST.U = vddphphy0rst.U;
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs1), okay)
	// Enable VDDPHPHY1
	MODULE_PMS.VMONP.VDDPHPHY1CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHPHY1STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHPHY1STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphphy1rst;
	vddphphy1rst.U = MODULE_PMS.VMONP.VDDPHPHY1RST.U;
	vddphphy1rst.B.RESETOFF = 0;
	vddphphy1rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHPHY1RST.U = vddphphy1rst.U;
#endif

#if 0
	// Enable VDDPHPHY2
	MODULE_PMS.VMONP.VDDPHPHY2CON.B.OVENABLE = 1;
	while (MODULE_PMS.VMONP.VDDPHPHY2STAT.B.RESULT <= MODULE_PMS.VMONP.VDDPHPHY2STAT.B.RESETVAL)
		;
	Ifx_PMS_VMONPRST vddphphy2rst;
	vddphphy2rst.U = MODULE_PMS.VMONP.VDDPHPHY2RST.U;
	vddphphy2rst.B.RESETOFF = 0;
	vddphphy2rst.B.RESETOFF_P = 1;
	MODULE_PMS.VMONP.VDDPHPHY2RST.U = vddphphy2rst.U;
#endif

	/* Enable module */
	MODULE_HSPHY.CLC.B.DISR = 0;
	if (!WAIT_FOR(MODULE_HSPHY.CLC.B.DISS == 0, 100, k_busy_wait(100))) {
		return -EIO;
	}

	/* Reset HSPHY */
	MODULE_HSPHY.RST.CTRLB.B.STATCLR = 1;
	MODULE_HSPHY.RST.CTRLA.B.KRST = 1;
	MODULE_HSPHY.RST.CTRLB.B.KRST = 1;
	if (!WAIT_FOR(MODULE_HSPHY.RST.STAT.B.KRST == 1, 100, k_busy_wait(100))) {
		return -EIO;
	}

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs0), okay)
	MODULE_HSPHY.PHY[0].CTRL1.B.PWRDWN = 0;
	k_sleep(K_USEC(25));
	MODULE_HSPHY.PHY[0].CTRL1.B.RST = 0;
	/* Wait for pcs mem to initialize */
	if (!WAIT_FOR(MODULE_HSPHY.XPCS[0].PMA.VR_XS_MP_12G_16G_25G_SRAM.B.INIT_DN == 1, 1000,
		      k_sleep(K_USEC(100)))) {
		return -EIO;
	}
	/* Exit firmware update mode */
	MODULE_HSPHY.XPCS[0].PMA.VR_XS_MP_12G_16G_25G_SRAM.B.EXT_LD_DN = 1;
	/* Wait until PCS reset is complete*/
	if (!WAIT_FOR(MODULE_HSPHY.XPCS[0].PCS.SR_XS_CTRL1.B.RST == 0, 20000,
		      k_sleep(K_USEC(100)))) {
		return -EIO;
	}
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(xpcs1), okay)
	MODULE_HSPHY.PHY[1].CTRL1.B.PWRDWN = 0;
	k_sleep(K_USEC(25));
	MODULE_HSPHY.PHY[1].CTRL1.B.RST = 0;
	/* Wait for pcs mem to initialize */
	if (!WAIT_FOR(MODULE_HSPHY.XPCS[1].PMA.VR_XS_MP_12G_16G_25G_SRAM.B.INIT_DN == 1, 1000,
		      k_sleep(K_USEC(100)))) {
		return -EIO;
	}
	/* Exit firmware update mode */
	MODULE_HSPHY.XPCS[1].PMA.VR_XS_MP_12G_16G_25G_SRAM.B.EXT_LD_DN = 1;
	/* Wait until PCS reset is complete*/
	if (!WAIT_FOR(MODULE_HSPHY.XPCS[1].PCS.SR_XS_CTRL1.B.RST == 0, 20000,
		      k_sleep(K_USEC(100)))) {
		return -EIO;
	}
#endif

	return 0;
}
#define HSPHY_INIT_PRIORITY                                                                        \
	COND_CODE_1(IS_ENABLED(CONFIG_MDIO), (CONFIG_MDIO_INIT_PRIORITY), (CONFIG_PHY_INIT_PRIORITY))

#define PHY_HSPHY_INIT(n)                                                                          \
	DEVICE_DT_INST_DEFINE(n, phy_hsphy_init, NULL, NULL, NULL, POST_KERNEL,                    \
			      HSPHY_INIT_PRIORITY, NULL)

DT_INST_FOREACH_STATUS_OKAY(PHY_HSPHY_INIT)
