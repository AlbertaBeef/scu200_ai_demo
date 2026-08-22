/******************************************************************************
* Copyright (C) 2018 - 2020 Xilinx, Inc.  All rights reserved.
* Copyright 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/


#ifndef DP159_H		 /* prevent circular inclusions */
#define DP159_H		 /* by using protection macros */

#include "xil_types.h"
#include "xparameters.h"
/*
 * HDMI PHY stack selection.
 * Designs built with the HDMI 2.1 PHY (v_hdmi_phy1) use the XHdmiphy1 driver;
 * everything else uses the legacy Video PHY (XVphy). Key off the IP actually
 * present in the XSA rather than the device family: the SCU200 (Spartan
 * UltraScale+) is not Versal but must use v_hdmi_phy1, because the legacy
 * vid_phy_controller has no transceiver wrapper for that silicon.
 */
#if defined(XPAR_XV_HDMIPHY1_NUM_INSTANCES) || defined(versal)
#include "xhdmiphy1.h"
#define DP159_USES_HDMIPHY1
#else
#include "xvphy.h"
#endif

#ifdef SDT
#define XPAR_IIC_0_BASEADDR XPAR_AXI_IIC_HDMI_CLK_BASEADDR
#endif
// Function prototypes
#ifdef DP159_USES_HDMIPHY1
u32 i2c_dp159(XHdmiphy1 *Hdmiphy1Ptr, u8 QuadId, u64 TxLineRate);
#else
u32 i2c_dp159(XVphy *VphyPtr, u8 QuadId, u64 TxLineRate);
#endif
u32 i2c_dp159_write(u8 dev, u8 addr, u8 dat);
u8 i2c_dp159_read(u8 dev, u8 addr);
void i2c_dp159_dump(void);

#endif
