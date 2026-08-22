/******************************************************************************
 * HDMI stack compatibility shim.
 *
 * The camera->HDMI app (main.c) is written against the legacy HDMI 1.4/2.0
 * stack: the Video PHY (XVphy / xvphy.h) + HDMI TX Subsystem (XV_HdmiTxSs /
 * xv_hdmitxss.h).
 *
 * On the SCU200 (Spartan UltraScale+) the legacy vid_phy_controller has no
 * transceiver wrapper for that silicon, so the design uses the HDMI 2.1 stack
 * instead: v_hdmi_phy1 (XHdmiphy1) + v_hdmi_txss1 (XV_HdmiTxSs1). Almost every
 * driver entry point maps 1:1 by name (XVphy_* -> XHdmiphy1_*,
 * XV_HdmiTxSs_* -> XV_HdmiTxSs1_*) with identical signatures, so this header
 * aliases the legacy vocabulary onto the HDMI 2.1 driver when that IP is present
 * in the XSA. The C preprocessor matches whole tokens, so aliasing the type
 * "XV_HdmiTxSs" does not touch "XV_HdmiTxSs_GetVideoStream" etc.; each name is
 * mapped independently below.
 *
 * The handful of entry points whose *signatures* actually differ are NOT aliased
 * here and are gated inline in main.c on HDMI_USE_HDMIPHY1:
 *   - XV_HdmiTxSs(1)_SetStream  (VideoMode vs VideoTiming/FrameRate, u64* out)
 *   - HdmiTxPtr -> HdmiTx1Ptr    (sub-core struct member rename)
 *   - SetHdmiMode -> SetHdmiTmdsMode (HDMI 2.1 splits TMDS vs FRL)
 *
 * Selection keys off the IP actually in the XSA (XPAR_XV_HDMIPHY1_NUM_INSTANCES),
 * not the device family, so any future board that uses the HDMI 2.1 PHY gets the
 * right stack automatically.
 ******************************************************************************/
#ifndef HDMI_COMPAT_H
#define HDMI_COMPAT_H

#include "xparameters.h"

#if defined(XPAR_XV_HDMIPHY1_NUM_INSTANCES)
#define HDMI_USE_HDMIPHY1
#endif

/* Stack-agnostic "an HDMI TX subsystem exists" gate. Legacy designs define
 * XPAR_XV_HDMITXSS_NUM_INSTANCES; HDMI 2.1 designs define ..._HDMITXSS1_...
 * Modules like xhdmi_edid keyed on the legacy macro must use this instead. */
#if defined(XPAR_XV_HDMITXSS_NUM_INSTANCES) || defined(XPAR_XV_HDMITXSS1_NUM_INSTANCES)
#define HDMI_TX_SS_PRESENT
#endif

#ifdef HDMI_USE_HDMIPHY1

#include "xhdmiphy1.h"
#include "xv_hdmitxss1.h"

/* ---- Types ------------------------------------------------------------- */
#define XVphy                            XHdmiphy1
#define XVphy_Config                     XHdmiphy1_Config
#define XVphy_PllType                    XHdmiphy1_PllType
#define XV_HdmiTxSs                      XV_HdmiTxSs1
#define XV_HdmiTxSs_Config               XV_HdmiTxSs1_Config

/* ---- Direction / channel / PLL enums ----------------------------------- */
#define XVPHY_DIR_TX                     XHDMIPHY1_DIR_TX
#define XVPHY_DIR_RX                     XHDMIPHY1_DIR_RX
#define XVPHY_CHANNEL_ID_CH1             XHDMIPHY1_CHANNEL_ID_CH1
#define XVPHY_CHANNEL_ID_CHA             XHDMIPHY1_CHANNEL_ID_CHA
#define XVPHY_CHANNEL_ID_CMN0            XHDMIPHY1_CHANNEL_ID_CMN0
#define XVPHY_CHANNEL_ID_CMN1            XHDMIPHY1_CHANNEL_ID_CMN1
#define XVPHY_PLL_TYPE_CPLL              XHDMIPHY1_PLL_TYPE_CPLL
#define XVPHY_PLL_TYPE_QPLL              XHDMIPHY1_PLL_TYPE_QPLL
#define XVPHY_PLL_TYPE_QPLL0             XHDMIPHY1_PLL_TYPE_QPLL0
/* GTHE4 has no PLL0 (that was a 7-series GTPE2 type). Map to UNKNOWN so the
 * legacy PllType comparison stays valid but never false-matches on this GT. */
#define XVPHY_PLL_TYPE_PLL0              XHDMIPHY1_PLL_TYPE_UNKNOWN
#define XVPHY_ERR_PLL_LAYOUT             XHDMIPHY1_ERR_PLL_LAYOUT
#define XVPHY_HDMI_HANDLER_TXINIT        XHDMIPHY1_HDMI_HANDLER_TXINIT
#define XVPHY_HDMI_HANDLER_TXREADY       XHDMIPHY1_HDMI_HANDLER_TXREADY

/* ---- TXSS handler enums ------------------------------------------------- */
#define XV_HDMITXSS_HANDLER_CONNECT      XV_HDMITXSS1_HANDLER_CONNECT
#define XV_HDMITXSS_HANDLER_TOGGLE       XV_HDMITXSS1_HANDLER_TOGGLE
#define XV_HDMITXSS_HANDLER_BRDGUNLOCK   XV_HDMITXSS1_HANDLER_BRDGUNLOCK
#define XV_HDMITXSS_HANDLER_BRDGOVERFLOW XV_HDMITXSS1_HANDLER_BRDGOVERFLOW
#define XV_HDMITXSS_HANDLER_BRDGUNDERFLOW XV_HDMITXSS1_HANDLER_BRDGUNDERFLOW
#define XV_HDMITXSS_HANDLER_STREAM_UP    XV_HDMITXSS1_HANDLER_STREAM_UP
#define XV_HDMITXSS_HANDLER_STREAM_DOWN  XV_HDMITXSS1_HANDLER_STREAM_DOWN

/* ---- Video PHY functions ----------------------------------------------- */
#define XVphy_IBufDsEnable               XHdmiphy1_IBufDsEnable
#define XVphy_GetLineRateHz              XHdmiphy1_GetLineRateHz
#define XVphy_Clkout1OBufTdsEnable       XHdmiphy1_Clkout1OBufTdsEnable
#define XVphy_SetHdmiCallback            XHdmiphy1_SetHdmiCallback
#define XVphy_SetHdmiTxParam             XHdmiphy1_SetHdmiTxParam
#define XVphy_SetErrorCallback           XHdmiphy1_SetErrorCallback
#define XVphy_GetPllType                 XHdmiphy1_GetPllType
#define XVphy_LookupConfig               XHdmiphy1_LookupConfig
#define XVphy_Hdmi_CfgInitialize         XHdmiphy1_Hdmi_CfgInitialize
#define XVphy_InterruptHandler           XHdmiphy1_InterruptHandler

/* ---- HDMI TX Subsystem functions (identical signatures) ---------------- */
#define XV_HdmiTxSs_SetCallback          XV_HdmiTxSs1_SetCallback
#define XV_HdmiTxSs_GetVideoStream       XV_HdmiTxSs1_GetVideoStream
#define XV_HdmiTxSs_StreamStart          XV_HdmiTxSs1_StreamStart
#define XV_HdmiTxSs_SetScrambler         XV_HdmiTxSs1_SetScrambler
#define XV_HdmiTxSs_SetSamplingRate      XV_HdmiTxSs1_SetSamplingRate
#define XV_HdmiTxSs_RefClockChangeInit   XV_HdmiTxSs1_RefClockChangeInit
#define XV_HdmiTxSs_LookupConfig         XV_HdmiTxSs1_LookupConfig
#define XV_HdmiTxSs_CfgInitialize        XV_HdmiTxSs1_CfgInitialize
#define XV_HdmiTxSs_GetAviInfoframe      XV_HdmiTxSs1_GetAviInfoframe
#define XV_HdmiTxSs_AudioMute            XV_HdmiTxSs1_AudioMute
#define XV_HdmiTxSs_GetVSIF              XV_HdmiTxSs1_GetVSIF
#define XV_HdmiTxSS_SetAppVersion        XV_HdmiTxSS1_SetAppVersion
#define XV_HdmiTxSS_MaskDisable          XV_HdmiTxSS1_MaskDisable
#define XV_HdmiTxSS_SetDviMode           XV_HdmiTxSS1_SetDviMode
#define XV_HdmiTxSS_HdmiTxIntrHandler    XV_HdmiTxSS1_HdmiTx1IntrHandler
#define XV_HdmiTxSs_DetectHdmi20         XV_HdmiTxSs1_DetectHdmi20
/* HDMI 2.1 ReadEdid gained a buffer-size arg. All callers use a 256-byte
 * buffer (one full EDID + extension block), so bind it in a function macro. */
#define XV_HdmiTxSs_ReadEdid(Inst, Buf)  XV_HdmiTxSs1_ReadEdid((Inst), (Buf), 256)

/* ---- xparameters base-address aliases ---------------------------------- */
#ifndef XPAR_XV_HDMITX_0_BASEADDR
#define XPAR_XV_HDMITX_0_BASEADDR        XPAR_XV_HDMITXSS1_0_BASEADDR
#endif
#ifndef XPAR_XVPHY_0_BASEADDR
#define XPAR_XVPHY_0_BASEADDR            XPAR_XV_HDMIPHY1_0_BASEADDR
#endif

#else  /* legacy HDMI 1.4/2.0 stack */

#include "xvphy.h"
#include "xv_hdmitxss.h"

#endif /* HDMI_USE_HDMIPHY1 */

#endif /* HDMI_COMPAT_H */
