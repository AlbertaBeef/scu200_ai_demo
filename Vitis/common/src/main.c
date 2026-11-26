/*
 * Copyright 2025 mariobergeron.com
 *
 * This example standalone application for the RPi Camera FMC will configure all of the connected
 * cameras that it finds connected, and then displays the video outputs on the DisplayPort monitor.
 * To display all four video streams on the single display, it uses the Video Processor Subsystem IP
 * to downsize the 1080p videos coming from the cameras to 720x480 resolution and then combines them
 * with the Video Mixer to produce a 1080p resolution video containing all four video streams.
 * The background is created by a Test Pattern Generator that is connected to the master layer of
 * the Video Mixer. The camera streams are organized on the screen as shown below:
 *
 * +------------------------------------------+
 * |              1920x1080px                 |
 * |  +----------------+  +----------------+  |
 * |  |                |  |                |  |
 * |  |     CAM0       |  |     CAM1       |  |
 * |  |   720x480px    |  |   720x480px    |  |
 * |  +----------------+  +----------------+  |
 * |                                          |
 * |  +----------------+  +----------------+  |
 * |  |                |  |                |  |
 * |  |     CAM2       |  |     CAM3       |  |
 * |  |   720x480px    |  |   720x480px    |  |
 * |  +----------------+  +----------------+  |
 * |                                          |
 * +------------------------------------------+
 *
 * Camera-to-DDR video pipe (x4):
 * ------------------------------
 *  RPi cam -> MIPI CSI SS -> Demosaic -> Gamma LUT -> VProc SS -> Frame Buffer Write -> DDR
 *
 *  The video pipe runs at 1 pixel per clock.
 *
 * DDR-to-HDMI video pipe:
 * ---------------------
 *                                    +-------------+
 *    Video Test Pattern Generator -> |             |
 * DDR -> Frame Buffer Read (CAM0) -> |             |
 * DDR -> Frame Buffer Read (CAM1) -> | Video Mixer |-> HDMI TX
 * DDR -> Frame Buffer Read (CAM2) -> |   (2ppc)    |   (2ppc)
 * DDR -> Frame Buffer Read (CAM3) -> |             |
 *                                    +-------------+
 *
 * The Frame Buffer Reads operate at 2 pixels per clock to match the Video Mixer and HDMI TX.
 */

/***************************** Include Files *********************************/
#include <stdint.h>
#include "xinterrupt_wrap.h"
#include "xv_mix_l2.h"
#include "mbdpu01.h"   /* custom-DPU (mbdpu01) driver - 28x28 INT8 MNIST classifier */
#include "mnist_pre.h" /* camera 640x480 RGB8 -> 28x28 INT8 preprocessing */
#include "xil_cache.h" /* Xil_DCacheInvalidateRange for the mbpre HW-preprocess path */
#include "xil_io.h"
#include "xil_types.h"
#include "xil_exception.h"
#include "string.h"
#include "idt_8t49n24x.h"
#include "xvidc.h"
#include "xv_hdmic.h"
#include "xv_hdmic_vsif.h"
#include "dp159.h"
#include "sleep.h"
#include "xhdmi_edid.h"
#include "hdmi_compat.h"   /* selects legacy XVphy/XV_HdmiTxSs or HDMI 2.1 XHdmiphy1/XV_HdmiTxSs1 */
#include "frmbuf.h"   /* Option A: background image reader (bg_frmbuf_rd) replaces v_tpg */
#include "xparameters.h"
#if defined(XPAR_V_TPG_BASEADDR) && !defined(XPAR_BG_FRMBUF_RD_BASEADDR)
#include "xv_tpg.h"   /* variants without a bg image reader drive the v_mix master with a TPG */
#endif
#include "xgpio.h"
#include "xintc.h"
#include "reset_gpio.h"
#include "config.h"
#include "rpi_cam.h"
#include "pipe.h"
#include "motor.h"   /* motor variant only: DRV8874 speed/dir + encoder (guarded by MOTOR_PRESENT) */
#include "disc_strobe.h" /* motor variant only: encoder decode + angle strobe (guarded by DISC_STROBE_PRESENT) */
#include "ov9281.h"   /* for the FSIN trigger-mode toggle wired to the disc_strobe 'F' key */
#include "fcapz_mark.h" /* fcapz timeline markers (no-ops unless the fcapz_mark GPIO exists) */
/* fsin_frame_tag (PL, in front of v_frmbuf_wr): with CTRL.drop_en set it discards the OV9281
 * wake frame (SOF < drop window after FSIN), so frmbuf_wr only writes the good frame per
 * strobe. AXI4-Lite registers (src/hdl/fsin_frame_tag.v); the BD pins the block at
 * 0x44A80000 in case the module reference is missing from xparameters.h. */
#if defined(XPAR_MIPI_0_FSIN_TAG_BASEADDR)
#define FSIN_TAG_BASE XPAR_MIPI_0_FSIN_TAG_BASEADDR
#elif defined(XPAR_MBDPU03_BASEADDR) || defined(XPAR_MBDPU03_S_AXI_CONTROL_BASEADDR) /* every demo03 build has it */
#define FSIN_TAG_BASE 0x44A80000u
#endif
#ifdef FSIN_TAG_BASE
#define FSIN_TAG_PRESENT 1
#define FTAG_CTRL        0x00u   /* [0] drop_en, [1] clear counters (W1, self-clearing) */
#define FTAG_WINDOW      0x04u   /* drop window, 300 MHz cycles (0 = 13 ms default) */
#define FTAG_FSIN_COUNT  0x08u
#define FTAG_DROPPED     0x0Cu
#define FTAG_SOF_DELAY   0x10u   /* cycles FSIN -> latest SOF */
#define FTAG_LAST_FRAME  0x14u   /* [7:0] index in strobe, [8] early */
#define FTAG_ID          0x1Cu   /* 0x46544101 */
#define FSIN_TAG_WR(o, v) Xil_Out32(FSIN_TAG_BASE + (o), (u32)(v))
#define FSIN_TAG_RD(o)    Xil_In32(FSIN_TAG_BASE + (o))
#else
#define FSIN_TAG_WR(o, v) ((void)(v))
#define FSIN_TAG_RD(o)    0u
#endif

#if defined(DISC_STROBE_PRESENT)
/* Bridge the disc_strobe 'F' console key to the OV9281 FSIN trigger mode. */
static uint8_t g_ov9281_iic     = 0;
static int     g_ov9281_present = 0;
static uint32_t g_exposure      = 32;     /* OV9281 manual exposure (lines) — ineffective in trigger mode */
static uint32_t g_gain          = 128;    /* OV9281 analog gain in 1/16x (128 = 8.0x); live via ]/[ keys */
static Frmbuf      *g_cam_frmbuf = 0;   /* camera 0 frame buffer (set after camera init) */
static volatile int g_trig_active = 0;  /* FSIN-trigger mode active -> worker gates dim frames */
static RpiCamera   *g_ov9281_cam = 0;  /* for the watchdog re-init (gpio reset + mode) */
/* Last strobe-target change (auto loop), for correlating with camera lock-ups. */
static uint32_t g_tchg_rev = 0, g_tchg_from = 0, g_tchg_to = 0;
static int      g_tchg_wrap = 0, g_tchg_valid = 0;
static uint32_t g_cam_wd_count = 0;     /* watchdog re-inits since boot */

/* Trigger-mode frame length (q key) and PL wake-frame drop (Q key). The wake frame's SOF
 * comes ~1.06 ms after FSIN and the good frame's one VTS later (~18.2 us/line), so the drop
 * window is put half a frame period after the wake SOF. Free-run always uses the default VTS. */
static const uint16_t k_trig_vts[] = { OV9281_VTS_DEFAULT, 1024, 800, 640, 544 };
#define TRIG_VTS_N (sizeof(k_trig_vts) / sizeof(k_trig_vts[0]))
static unsigned g_trig_vts_i = 0;
static int      g_pl_drop    = 1;
static uint32_t trig_vts(void) { return k_trig_vts[g_trig_vts_i]; }
static uint32_t fsin_drop_window_us(void) { return 1060u + (trig_vts() * 182u) / 20u; }
static void fsin_tag_apply(void)
{
    FSIN_TAG_WR(FTAG_WINDOW, fsin_drop_window_us() * 300u);   /* 300 MHz video clock */
    FSIN_TAG_WR(FTAG_CTRL, (g_trig_active && g_pl_drop) ? 1u : 0u);
}

static void ov9281_fsin_trigger_cb(int enable)
{
    if (!g_ov9281_present) return;
    /* KNOWN, ACCEPTED TRANSIENT: toggling FSIN trigger stops/starts the OV9281 stream (reg
     * 0x0100), which disturbs the HDMI TX path enough that the monitor briefly drops sync and
     * re-runs its EDID/HPD handshake (~1 s black), then recovers; trigger/freeze work normally
     * after. Not a software-fixable event (masking the VPHY clock-detector removed the internal
     * TX-restart but the sink still re-syncs) -- it's a fabric-level coupling between the camera
     * pipe and the v_mix master feeding HDMI. A true fix needs a block-design change. Left as-is. */
    ov9281_set_vts(g_ov9281_iic, enable ? trig_vts() : OV9281_VTS_DEFAULT);
    ov9281_set_trigger_mode(g_ov9281_iic, enable);
    /* In trigger mode the worker drives the camera display so it can drop the dim wake frame;
     * in free-run the DONE ISR drives it (smooth). */
    g_trig_active = enable ? 1 : 0;
    fsin_tag_apply();            /* trigger mode: drop the wake frame in the PL */
    if (g_cam_frmbuf) {
        g_cam_frmbuf->AppDrivesRd = (u8)(enable ? 1 : 0);
        if (!enable) g_cam_frmbuf->DisplayHeldAddr = 0;   /* release the hold -> ISR uses all slots */
    }
}

/* Camera watchdog. In FSIN-trigger mode with the strobe armed and the disc turning, every
 * revolution must deliver N frames. If CAM_WD_REVS revolutions pass with no completed frame,
 * the OV9281 has locked up (MIPI lanes idle in stop state; not even free-run recovers it), so
 * hardware-reset and re-configure it, restore exposure/gain, and re-enter trigger mode. */
#define CAM_WD_REVS 3u
static void cam_watchdog(void)
{
    static uint32_t last_seq = 0, last_rev = 0;
    if (!g_ov9281_present || !g_ov9281_cam || !g_cam_frmbuf) return;
    uint32_t seq = g_cam_frmbuf->FrameSeq, rev = disc_strobe_revs();
    if (seq != last_seq || !g_trig_active || !disc_strobe_enabled()) {
        last_seq = seq; last_rev = rev; return;           /* frames flowing, or not expected */
    }
    if ((uint32_t)(rev - last_rev) < CAM_WD_REVS) return;   /* disc stopped -> never trips */

    g_cam_wd_count++;
    xil_printf("cam: WATCHDOG #%lu - no frame for %lu revs (seq=%lu rev=%lu pos=%lu target=%lu)\r\n",
               (unsigned long)g_cam_wd_count, (unsigned long)(rev - last_rev), (unsigned long)seq,
               (unsigned long)rev, (unsigned long)disc_strobe_position(),
               (unsigned long)disc_strobe_target());
    if (g_tchg_valid)
        xil_printf("cam:   last target change %lu->%lu%s at rev %lu (%lu revs before trip)\r\n",
                   (unsigned long)g_tchg_from, (unsigned long)g_tchg_to, g_tchg_wrap ? " WRAP" : "",
                   (unsigned long)g_tchg_rev, (unsigned long)(rev - g_tchg_rev));
    else
        xil_printf("cam:   no auto target change since boot\r\n");

    RpiCamera *c = g_ov9281_cam;
    ov9281_config(c->iic_id, c->gpio, c->gpio_mask);   /* gpio reset + soft reset + mode + stream */
    ov9281_set_exposure(c->iic_id, g_exposure);
    ov9281_set_gain(c->iic_id, g_gain);
    ov9281_set_vts(c->iic_id, trig_vts());
    ov9281_set_trigger_mode(c->iic_id, 1);
    xil_printf("cam: watchdog re-init done\r\n");
    last_seq = g_cam_frmbuf->FrameSeq; last_rev = disc_strobe_revs();
}
#endif


/************************** Constant Definitions *****************************/
#ifdef HDMI_USE_HDMIPHY1
/* SCU200: the 8T49N241 (U104) address pins S_A[1:0] are strapped to GND, so the
 * 7-bit I2C address is 0x6C (confirmed by UG1780 Table 14 "HDMI Control I2C Bus"
 * and the board schematic page 11). The auboard straps it to 0x7C. */
#define I2C_CLK_ADDR    0x6C  /**< I2C Clk Address IDT_8T49N241 (SCU200) */

/* SCU200 HDMI TX redriver is a TI TMDS1204 (U40 @ 0x5E on the HDMI_CTL bus),
 * NOT a DP159. It shares the axi_iic_hdmi_clk controller with the 8T49N241. */
#include "ti_tmds1204.h"
#define HDMI_TX_REDRIVER_ADDR  0x5E
static XIic HdmiCtlIic;       /* only .BaseAddress is used (base-address-mode XIic_Send) */
static u8   TxRedriverRev = 1;
#else
#define I2C_CLK_ADDR    0x7C  /**< I2C Clk Address IDT_8T49N241 */
#endif

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/************************** Constant Definitions *****************************/
/* Assign Mode ID Enumeration. First entry Must be > XVIDC_VM_CUSTOM */
typedef enum {
	XVIDC_VM_1152x864_60_P = (XVIDC_VM_CUSTOM + 1),
	XVIDC_CM_NUM_SUPPORTED
}
XVIDC_CUSTOM_MODES;

/* Enabling this will register a custom resolution to the video timing table
 */
#define CUSTOM_RESOLUTION_ENABLE 1

/***************** Macros (Inline Functions) Definitions *********************/
/* These macro values need to changed whenever there is a change in version */
#define APP_MAJ_VERSION 5
#define APP_MIN_VERSION 4

#define INTRNAME_HDMITX 0
#define INTRNAME_HDCP1XTX  1
#define INTRNAME_HDCP1XTX_TIMER 2
#define INTRNAME_HDCP2XTX_TIMER   3

// Number of cameras/video pipes in the design
#define NUM_CAMS XPAR_XMIPICSISS_NUM_INSTANCES

// Frame buffer Rd/Wr buffer addresses (using physical memory)
#define CAM0_FRMBUF_BUFR_ADDR 0xA0000000
#define CAM1_FRMBUF_BUFR_ADDR 0xA1000000
#define CAM2_FRMBUF_BUFR_ADDR 0xA2000000
#define CAM3_FRMBUF_BUFR_ADDR 0xA3000000

/**************************** Type Definitions *******************************/

/************************** Function Prototypes ******************************/
int I2cClk(u32 InFreq, u32 OutFreq);

void EnableHdmiTx(XVphy *VphyPtr, XV_HdmiTxSs *HdmiTxSsPtr,
					XVidC_VideoMode VideoMode,
					XVidC_ColorFormat ColorFormat,
					XVidC_ColorDepth Bpc);

void TxConnectCallback(void *CallbackRef);
void TxToggleCallback(void *CallbackRef);
void TxBrdgUnlockedCallback(void *CallbackRef);
void TxStreamUpCallback(void *CallbackRef);
void TxStreamDownCallback(void *CallbackRef);
void VphyHdmiTxInitCallback(void *CallbackRef);
void VphyHdmiTxReadyCallback(void *CallbackRef);

void VphyErrorCallback(void *CallbackRef);
void VphyProcessError(void);

/************************* Variable Definitions *****************************/
// VPHY structure
XVphy              Vphy;
u8                 VphyErrorFlag;
u8                 VphyPllLayoutErrorFlag;

// HDMI TX SS structure
XV_HdmiTxSs        HdmiTxSs;
XV_HdmiTxSs_Config *XV_HdmiTxSs_ConfigPtr;

EdidHdmi20 EdidHdmi20_t;

// Test Pattern Generator Structure
/* Option A background image reader: streams a static 1920x1080 image from DDR into
 * the v_mix master layer (replaces the v_tpg cross-hatch). The image is loaded to
 * BG_IMG_BUF over JTAG at program time (see program script), so it isn't in the ELF. */
XV_FrmbufRd_l2     BgFrmbufRd;
#define BG_IMG_ORIG  0xB4000000u   /* pristine background image (JTAG-loaded; SW never draws here) */
#define BG_IMG_BUF   0xB5000000u   /* working front buffer (copied from ORIG at boot) */
#define BG_IMG_BUF_B 0xB6000000u   /* working back buffer for double-buffered overlay */
/* Overlay double-buffering: the per-capture worker draws the ROI+bars into the BACK buffer,
 * flushes it, then pageflips bg_frmbuf_rd to it (takes effect at the next frame boundary), so
 * the scanout never sees a half-drawn panel. Both buffers hold the same image + static labels
 * (seeded by a boot-time copy); the dynamic panel is fully repainted each frame, so ping-pong
 * is safe. Displayed = front. */
static uintptr_t g_bg_front = BG_IMG_BUF;
static uintptr_t g_bg_back  = BG_IMG_BUF_B;

/* ---- RPM / captures-per-second HUD (disc/motor build only) -------------------------- */
#if defined(DISC_STROBE_PRESENT) && defined(XPAR_BG_FRMBUF_RD_BASEADDR)
#define STATS_ENABLE 1
/* Read the BSP sleep timer (a free-running AXI timer @ 100 MHz) as a wall clock for the rate
 * math. usleep() reprograms this timer transiently, but at run time only the I2C gain/exposure
 * path calls usleep — so stats_service() re-arms the timer whenever it finds the mode disturbed
 * (down-count / stopped) and simply drops that one measurement window. */
#define RT_BASE    XPAR_AXI_TIMER_BASEADDR
#define RT_TCSR    0x00u
#define RT_TLR     0x04u
#define RT_TCR     0x08u
#define RT_ENABLE  0x80u      /* XTC_CSR_ENABLE_TMR_MASK  */
#define RT_ARLD    0x10u      /* XTC_CSR_AUTO_RELOAD_MASK */
#define RT_LOAD    0x20u      /* XTC_CSR_LOAD_MASK        */
#define RT_DOWN    0x02u      /* XTC_CSR_DOWN_COUNT_MASK  */
#define RT_MODE    (RT_ENABLE | RT_ARLD)   /* up-count, auto-reload, no interrupt */
#ifdef XPAR_AXI_TIMER_CLOCK_FREQUENCY
#define RT_FREQ_HZ XPAR_AXI_TIMER_CLOCK_FREQUENCY
#else
#define RT_FREQ_HZ 100000000u
#endif
static uint32_t rt_tick(void) { return Xil_In32(RT_BASE + RT_TCR); }   /* overlay profiling */
#define STAT_PANEL_X 815      /* in the free gap between the two 640-wide camera windows */
#define STAT_PANEL_Y 515
#define STAT_PANEL_W 230      /* must match mnist_stats_panel() W/H */
#define STAT_PANEL_H 320
#define TGT_GAUGE_X  1300     /* compact target/AUTO gauge, between digit-9's bar (ends y~864) and */
#define TGT_GAUGE_Y  906      /* the background's "Classification" title (y 990 over these columns). */
#define TGT_GAUGE_W  192      /* Mid tick at x0+(w-1)/2 ~ 1395 = DX centre tick (ROI_X 1340 + 56). */
static int g_tgt_redraw = 1;  /* force the TGT/AUTO gauge repaint (set when the HUD comes on) */
static int g_hud_on = 0;      /* 'h' toggles the stats panel + ROI box together; default OFF */
static int g_verbose = 1;     /* 'v' toggles a 1/sec one-line console dump of the panel; default ON */
static void hud_clear_panel(void);  /* fwd decl: used by the 'h' key handler above its definition */
static uint32_t g_rt_last_cy, g_rt_last_cnt, g_rt_last_seq, g_verbose_last_cy;
static int      g_stat_rpm = -1, g_stat_caps_x10 = -1;
/* Per-capture stage latencies in ms x10 (updated by the worker; -1 = no data yet). */
static volatile int g_stat_pre_x10 = -1, g_stat_dpu_x10 = -1, g_stat_ovl_x10 = -1;

/* ---- ROI training-data capture (record mode -> DDR gallery -> JTAG dump) ------------ */
#define ROI_GAL_BASE     0xBA000000u   /* DDR gallery (in the free 0xBA region; DDR ends 0xBFFFFFFF) */
#define ROI_GAL_CAP      1024u         /* samples per session */
#define ROI_GAL_HDR      256u          /* reserved gallery-header bytes before sample 0 */
#define ROI_SMP_HDR      128u          /* per-sample header bytes */
#define ROI_SMP_IMG      784u          /* 28x28 int8 (model input) */
#define ROI_SMP_CROP     (112u*112u)   /* 112x112 u8 hi-res crop = 12544 */
#define ROI_SMP_BYTES    (ROI_SMP_HDR + ROI_SMP_IMG + ROI_SMP_CROP)   /* 13456 */
static int      g_recording = 0;
static uint32_t g_roi_count = 0;
static void roi_gallery_init(void);   /* fwd; defined after stats_service */
static void roi_record(uintptr_t cambuf, uint32_t stride, uintptr_t img_src,
                       int pred, const int32_t *logits, uint32_t mean);  /* fwd; defined after stats */
#endif
#if defined(XPAR_V_TPG_BASEADDR) && !defined(XPAR_BG_FRMBUF_RD_BASEADDR)
XV_tpg             Tpg;   /* v_mix master driver for variants without a bg image reader */
#endif

// Video Mixer
XV_Mix_l2  VMix;

// Flag indicates whether the TX Cable is connected or not
u8                 TxCableConnect = (FALSE);

// TX busy flag. This flag is set while the TX is initialized
u8                 TxBusy = (TRUE);
// TX restart HDMI TX. This flag is set when the TX cable has been reconnected.
u8                 TxRestartHdmiTx = (FALSE);
// TX Stream Up Status Flag, Avoiding Race condition
u8                 IsStreamUp = (FALSE);
/* Persistent "HDMI TX link is up" latch (IsStreamUp is a one-shot edge consumed in the
 * loop). Set in TxStreamUpCallback, cleared in TxStreamDownCallback. The per-capture
 * inference worker gates on this so HDMI bring-up gets the full loop before the ~60 ms
 * worker starts consuming CPU (otherwise the free-running camera starves bring-up). */
volatile u8        HdmiStreamUp = (FALSE);

/* Diagnostic: per-capture frame-brightness counters. The worker samples the centre row of
 * each completed frame; frames darker than a threshold are counted as "dark". Confirms the
 * suspected OV9281 wake-dummy-frame in FSIN-trigger mode. Printed + reset by the 'B' key. */
static uint32_t g_last_good_bright = 0;           /* good-frame gate reference (see top of file) */
static uint32_t g_frm_min = 255, g_frm_max = 0;   /* TEST probe: brightness range over a window */

/* --- Programmable mbpre crop (power-of-2 block; see Vivado/ip/mbpre/mbpre.cpp) ----------
 * blk = 1<<shift, crop = 28*blk, top-left (x0,y0) in the 640x480 frame, output 28x28.
 * Written to the mbpre AXI-Lite regs each capture. Live pan/zoom over UART (see keys below).
 * Guarded by the XPAR macro (MBPRE_BASE itself is #defined inside main()). Default = centred
 * 112 crop (shift 2), matching the previous MNIST_CROP. */
/* --- mbdpu02 digit-position head (scu200_ai_demo02) -----------------------------------
 * mbdpu02 = mbdpu01 + a dx/dy head. Per capture: dx/dy (1/100 model px) drive the gauges
 * around the ROI preview; with auto-target ('A') the mean dx over one revolution nudges the
 * strobe target. Sign: on the rig the digits sweep right->left in the image (take02: later
 * strobes -> smaller ink-x), so a digit right of centre (dx>0) means the strobe fired early
 * -> raise target (confirmed on the rig: dx > 0 -> increase target). Target wraps within
 * 0..target_max. Flip DX_TARGET_SIGN if the loop runs away (e.g. motor reversed with 'r'). */
/* mbdpu03 (scu200_ai_demo03): 112x112 mono input, camera crop/zoom fused into the core (no mbpre),
 * same position head but dx/dy in 112-px units (4x demo02's), so the loop constants scale by 4. */
#if defined(XPAR_MBDPU03_S_AXI_CONTROL_BASEADDR) || defined(XPAR_MBDPU03_BASEADDR)
#define MBDPU03_CORE 1
#endif
#if defined(XPAR_MBDPU02_S_AXI_CONTROL_BASEADDR) || defined(XPAR_MBDPU02_BASEADDR) || defined(MBDPU03_CORE)
#define MBDPU_HAS_POS 1
#define DX_TARGET_SIGN      (+1)
#ifdef MBDPU03_CORE
#define POS_UNIT_X100       200   /* dx per target count step: 2 px (112-px units) */
#else
#define POS_UNIT_X100       50    /* dx per target count step: 0.5 px (28-px units) */
#endif
#define AUTO_DEADBAND_X100  POS_UNIT_X100   /* |mean dx| <= 1 unit: leave target alone */
#define AUTO_STEP_MAX       5     /* counts per revolution (~1.3 px at ~0.26 px/count, 28-px units) */
static int32_t  g_pos_x100[2] = {0, 0};   /* latest dx, dy */
static int      g_auto_tgt = 0;           /* 'A': dx -> target loop on/off (default off) */
static int32_t  g_auto_sum = 0;           /* sum of dx over the current revolution */
static uint32_t g_auto_n   = 0;           /* captures summed */
#endif

#if defined(XPAR_MBPRE_S_AXI_CONTROL_BASEADDR) || defined(XPAR_MBPRE_BASEADDR) || defined(MBDPU03_CORE)
#define MBPRE_CROP_CTRL 1
#ifdef MBDPU03_CORE
#define CROP_DIM       112u                                /* mbdpu03 output side: crop = 112<<shift */
static uint32_t g_crop_shift = 0;                          /* 112<<0 = 112, 1:1 */
#define CROP_SHIFT_MAX 2                                   /* blk<=4, crop<=448 */
#else
#define CROP_DIM       ((uint32_t)MNIST_DIM)               /* mbpre output side: crop = 28<<shift */
static uint32_t g_crop_shift = 2;                          /* 28<<2 = 112 */
#define CROP_SHIFT_MAX 4                                   /* blk<=16, crop<=448 (fits 480 lines) */
#endif
static uint32_t g_crop_x0    = (MNIST_FRAME_W - 112) / 2;  /* 264 */
static uint32_t g_crop_y0    = (MNIST_FRAME_H - 112) / 2;  /* 184 */

static void mnist_crop_recenter(void)
{
    uint32_t crop = CROP_DIM << g_crop_shift;
    g_crop_x0 = (MNIST_FRAME_W - crop) / 2u;
    g_crop_y0 = (MNIST_FRAME_H - crop) / 2u;
}

/* Console keys (routed via motor_poll's aux hook). Lowercase, clear of the motor/strobe keys:
 *   z zoom in (smaller crop)   x zoom out (bigger)   i/k pan up/down   j/l pan left/right
 *   o re-centre */
static int mnist_crop_key(int ch)
{
    const uint32_t step = 16;
    uint32_t crop = CROP_DIM << g_crop_shift;
    switch (ch) {
    case 'z': if (g_crop_shift > 0)              { g_crop_shift--; mnist_crop_recenter(); } break;
    case 'x': if (g_crop_shift < CROP_SHIFT_MAX) { g_crop_shift++; mnist_crop_recenter(); } break;
    case 'j': g_crop_x0 = (g_crop_x0 > step) ? g_crop_x0 - step : 0; break;
    case 'l': { uint32_t mx = MNIST_FRAME_W - crop; g_crop_x0 = (g_crop_x0 + step < mx) ? g_crop_x0 + step : mx; } break;
    case 'i': g_crop_y0 = (g_crop_y0 > step) ? g_crop_y0 - step : 0; break;
    case 'k': { uint32_t my = MNIST_FRAME_H - crop; g_crop_y0 = (g_crop_y0 + step < my) ? g_crop_y0 + step : my; } break;
    case 'o': mnist_crop_recenter(); break;
#ifdef MBDPU_HAS_POS
    case 'A': if (!g_auto_tgt && !(disc_strobe_enabled() && g_trig_active)) {
                  xil_printf("auto-target: not enabled - needs strobe ARMED (S) and FSIN trigger ON (F)\r\n");
                  return 1;
              }
              g_auto_tgt = !g_auto_tgt; g_auto_sum = 0; g_auto_n = 0;
              xil_printf("auto-target: %s (dx -> target, once per rev)\r\n",
                         g_auto_tgt ? "ON" : "off"); return 1;
#endif
#ifdef STATS_ENABLE
    case 'u': mnist_ovl_set_delta(!mnist_ovl_get_delta());   /* overlay: full / delta redraw */
              xil_printf("overlay: %s redraw\r\n", mnist_ovl_get_delta() ? "delta" : "full"); return 1;
    case 'h': g_hud_on = !g_hud_on;                 /* toggle stats panel + ROI box together */
              if (!g_hud_on) hud_clear_panel();     /* black out the panel (ROI box clears on next frame) */
              mnist_set_show_values(g_hud_on);      /* DX/DY + TGT numbers follow the HUD */
              g_tgt_redraw = 1;                     /* (gauges + labels always shown) */
              xil_printf("hud: %s\r\n", g_hud_on ? "ON" : "off"); return 1;
    case 'v': g_verbose = !g_verbose;               /* toggle the 1/sec console stats line */
              xil_printf("verbose: %s\r\n", g_verbose ? "ON" : "off"); return 1;
    case 'w': g_recording = !g_recording;           /* toggle ROI training-data recording */
              if (g_recording) { roi_gallery_init();
                  xil_printf("REC: ON (arm F+S and spin; 0/%lu)\r\n", (unsigned long)ROI_GAL_CAP); }
              else xil_printf("REC: off (%lu captured)\r\n", (unsigned long)g_roi_count);
              return 1;
#endif
    case '?':                                       /* list every UART key */
        xil_printf("\r\n=== SCU200 AI Demo - UART keys ===\r\n");
        xil_printf("Motor Driver:  1-9 speed 10-90%% | +/- speed +-5%% | 0 stop | r reverse | e/d enable/disable | f fault | p status\r\n");
        xil_printf("Motor Encoder: c enc-check\r\n");
        xil_printf("Strobe:  S arm/disarm | F FSIN trigger | N captures/rev (1/2/4/8/16) | T/G target +-5 | C/V cpr +-0.1 (drift-null) | Y status\r\n");
        xil_printf("Crop:    z/x zoom in/out | j/l pan L/R | i/k pan U/D | o recenter\r\n");
        xil_printf("Camera:  ]/[ gain +-0.5x | ./, exposure +-256 | q trigger VTS 1373/1024/800/640/544 | Q PL wake-frame drop\r\n");
        xil_printf("Overlay: h HUD (stats, ROI box, DX/DY + TGT values) on/off | u full/delta redraw | v verbose console on/off | ? this help\r\n");
        xil_printf("Capture: w record ROI training data on/off (fills DDR gallery for JTAG dump)\r\n");
#ifdef MBDPU_HAS_POS
        xil_printf("Position: A auto-target on/off (CNN dx steers the strobe target; DX/DY gauges on HDMI)\r\n");
#endif
        return 1;
    case ']': g_gain += 8; if (g_gain > 0x3F0) g_gain = 0x3F0;   /* gain up (~0.5x/step, cap ~63x) */
              if (g_ov9281_present) ov9281_set_gain(g_ov9281_iic, g_gain);
              xil_printf("gain=%lu.%02lux (reg %lu)\r\n", (unsigned long)(g_gain/16),
                         (unsigned long)((g_gain%16)*100/16), (unsigned long)g_gain); return 1;
    case '[': g_gain = (g_gain > 24) ? g_gain - 8 : 16;          /* gain down (floor 1.0x) */
              if (g_ov9281_present) ov9281_set_gain(g_ov9281_iic, g_gain);
              xil_printf("gain=%lu.%02lux (reg %lu)\r\n", (unsigned long)(g_gain/16),
                         (unsigned long)((g_gain%16)*100/16), (unsigned long)g_gain); return 1;
    case '.': g_exposure += 256; if (g_exposure > 4000) g_exposure = 4000;   /* exposure up (test) */
              if (g_ov9281_present) ov9281_set_exposure(g_ov9281_iic, g_exposure);
              xil_printf("expo=%lu lines\r\n", (unsigned long)g_exposure); return 1;
    case ',': g_exposure = (g_exposure > 256) ? g_exposure - 256 : 32;       /* exposure down */
              if (g_ov9281_present) ov9281_set_exposure(g_ov9281_iic, g_exposure);
              xil_printf("expo=%lu lines\r\n", (unsigned long)g_exposure); return 1;
    case 'q': g_trig_vts_i = (g_trig_vts_i + 1u) % TRIG_VTS_N;   /* trigger-mode frame length */
              if (g_ov9281_present && g_trig_active) ov9281_set_vts(g_ov9281_iic, trig_vts());
              fsin_tag_apply();
              xil_printf("trigger VTS=%lu lines (~%lu us/frame), PL drop window %lu us%s\r\n",
                         (unsigned long)trig_vts(), (unsigned long)(trig_vts() * 182u / 10u),
                         (unsigned long)fsin_drop_window_us(),
                         g_trig_active ? "" : " (applies when F is on)"); return 1;
    case 'Q': g_pl_drop = !g_pl_drop; fsin_tag_apply();             /* PL wake-frame drop */
              xil_printf("PL wake-frame drop: %s\r\n", g_pl_drop ? "ON" : "off (SW dim gate only)");
              return 1;
    case 'B': { /* TEST probe: frame-dones (ISR) vs strobe revs -> extra-frame ratio; bright range */
                uint32_t seq = g_cam_frmbuf ? g_cam_frmbuf->FrameSeq : 0;
                xil_printf("probe: frame-dones=%lu revs=%lu bright[min=%lu max=%lu]\r\n",
                           (unsigned long)seq, (unsigned long)disc_strobe_revs(),
                           (unsigned long)g_frm_min, (unsigned long)g_frm_max);
#ifdef FSIN_TAG_PRESENT
                { u32 lf = FSIN_TAG_RD(FTAG_LAST_FRAME);
                  xil_printf("fsin_tag: fsin=%lu dropped=%lu window=%lu us drop=%s | last SOF %lu us after FSIN, idx %lu%s\r\n",
                             (unsigned long)FSIN_TAG_RD(FTAG_FSIN_COUNT), (unsigned long)FSIN_TAG_RD(FTAG_DROPPED),
                             (unsigned long)(FSIN_TAG_RD(FTAG_WINDOW) / 300u),
                             (FSIN_TAG_RD(FTAG_CTRL) & 1u) ? "on" : "off",
                             (unsigned long)(FSIN_TAG_RD(FTAG_SOF_DELAY) / 300u),
                             (unsigned long)(lf & 0xFFu), (lf & 0x100u) ? " (early)" : "");
                  FSIN_TAG_WR(FTAG_CTRL, FSIN_TAG_RD(FTAG_CTRL) | 2u); }  /* clear counters */
#endif
#ifdef STATS_ENABLE
                { uint32_t pt[MNIST_OVL_PROF_N]; uint32_t n = mnist_ovl_prof(pt);
                  uint32_t tpu = RT_FREQ_HZ / 1000000u, sum = 0;
                  for (int i = 0; i < MNIST_OVL_PROF_N; i++) sum += pt[i];
                  if (n == 0) n = 1;
                  xil_printf("overlay (%s, n=%lu) avg us: preview=%lu bars=%lu live_flush=%lu pos=%lu pos_flush=%lu total=%lu\r\n",
                             mnist_ovl_get_delta() ? "delta" : "full", (unsigned long)n,
                             (unsigned long)(pt[0]/tpu/n), (unsigned long)(pt[1]/tpu/n), (unsigned long)(pt[2]/tpu/n),
                             (unsigned long)(pt[3]/tpu/n), (unsigned long)(pt[4]/tpu/n), (unsigned long)(sum/tpu/n)); }
#endif
                g_frm_min = 255; g_frm_max = 0; } return 1;
    default:  return 0;
    }
    crop = CROP_DIM << g_crop_shift;
    xil_printf("crop: %lux%lu at (%lu,%lu) blk=%lu\r\n",
               (unsigned long)crop, (unsigned long)crop, (unsigned long)g_crop_x0,
               (unsigned long)g_crop_y0, (unsigned long)(1u << g_crop_shift));
    return 1;
}
#endif
u64                TxLineRate = 0;
// Sink Ready: Become true when the EDID parsing is completed upon cable connect
u8                 SinkReady = (FALSE);

// Interrupt Controller
static XIntc       Intc;

// GPIO for resets of the video pipes
XGpio GpioVideoResets;
XGpio_Config *GpioVideoResets_ConfigPtr;

#ifdef XPAR_MIPI_0_AXI_IIC_0_BASEADDR
// Video pipe 0
VideoPipe Cam0;

VideoPipeBaseAddr CamBaseAddr0 = {
		XPAR_MIPI_0_AXI_IIC_0_BASEADDR,
		XPAR_MIPI_0_AXI_GPIO_0_BASEADDR,
		XPAR_MIPI_0_V_FRMBUF_WR_BASEADDR,
		XPAR_MIPI_0_V_FRMBUF_RD_BASEADDR,
		CAM0_FRMBUF_BUFR_ADDR,
#ifdef PIPE_HAS_ISP
		XPAR_MIPI_0_DEMOSAIC_0_BASEADDR,
		XPAR_MIPI_0_V_GAMMA_LUT_BASEADDR,
		XPAR_MIPI_0_V_PROC_BASEADDR
#endif
};
#endif

#ifdef XPAR_MIPI_1_AXI_IIC_0_BASEADDR
// Video pipe 1
VideoPipe Cam1;

VideoPipeBaseAddr CamBaseAddr1 = {
		XPAR_MIPI_1_AXI_IIC_0_BASEADDR,
		XPAR_MIPI_1_AXI_GPIO_0_BASEADDR,
		XPAR_MIPI_1_V_FRMBUF_WR_BASEADDR,
		XPAR_MIPI_1_V_FRMBUF_RD_BASEADDR,
		CAM1_FRMBUF_BUFR_ADDR,
		XPAR_MIPI_1_DEMOSAIC_0_BASEADDR,
		XPAR_MIPI_1_V_GAMMA_LUT_BASEADDR,
		XPAR_MIPI_1_V_PROC_BASEADDR
};
#endif

#ifdef XPAR_MIPI_2_AXI_IIC_0_BASEADDR
// Video pipe 2
VideoPipe Cam2;

VideoPipeBaseAddr CamBaseAddr2 = {
		XPAR_MIPI_2_AXI_IIC_0_BASEADDR,
		XPAR_MIPI_2_AXI_GPIO_0_BASEADDR,
		XPAR_MIPI_2_V_FRMBUF_WR_BASEADDR,
		XPAR_MIPI_2_V_FRMBUF_RD_BASEADDR,
		CAM2_FRMBUF_BUFR_ADDR,
		XPAR_MIPI_2_DEMOSAIC_0_BASEADDR,
		XPAR_MIPI_2_V_GAMMA_LUT_BASEADDR,
		XPAR_MIPI_2_V_PROC_BASEADDR
};
#endif

#ifdef XPAR_MIPI_3_AXI_IIC_0_BASEADDR
// Video pipe 3
VideoPipe Cam3;

VideoPipeBaseAddr CamBaseAddr3 = {
		XPAR_MIPI_3_AXI_IIC_0_BASEADDR,
		XPAR_MIPI_3_AXI_GPIO_0_BASEADDR,
		XPAR_MIPI_3_V_FRMBUF_WR_BASEADDR,
		XPAR_MIPI_3_V_FRMBUF_RD_BASEADDR,
		CAM3_FRMBUF_BUFR_ADDR,
		XPAR_MIPI_3_DEMOSAIC_0_BASEADDR,
		XPAR_MIPI_3_V_GAMMA_LUT_BASEADDR,
		XPAR_MIPI_3_V_PROC_BASEADDR
};
#endif

// Indices of the video pipes in this design
static const int g_mipi_present[] = {
#ifdef XPAR_MIPI_0_AXI_IIC_0_BASEADDR
    0,
#endif
#ifdef XPAR_MIPI_1_AXI_IIC_0_BASEADDR
    1,
#endif
#ifdef XPAR_MIPI_2_AXI_IIC_0_BASEADDR
    2,
#endif
#ifdef XPAR_MIPI_3_AXI_IIC_0_BASEADDR
    3,
#endif
};

/* Return the 1-based Video Mixer channel for a given MIPI index,
   or 0 if that MIPI isn’t present. */
static int mixer_channel_for_mipi(int mipi_idx)
{
    for (int i = 0; i < NUM_CAMS; ++i) {
        if (g_mipi_present[i] == mipi_idx) {
            return i + 1;  // 1-based channels: [1..NUM_ACTIVE_MIPI]
        }
    }
    return 0; // not present
}

/*
 * The table below determines how the four camera video streams are organized
 * on a single 1080p video stream
 */
static const XVidC_VideoWindow MixLayerConfig[4] =
{// X    Y    W    H   -- 640x480 windows at the original layer locations
  {160,  500, VPROC_WIDTH_OUT, VPROC_HEIGHT_OUT}, //Layer 1 (CAM0) camera input
  {1120, 500, VPROC_WIDTH_OUT, VPROC_HEIGHT_OUT}, //Layer 2 HLS pass-through
  {160,  560, VPROC_WIDTH_OUT, VPROC_HEIGHT_OUT}, //Layer 3 bottom-left
  {1040, 560, VPROC_WIDTH_OUT, VPROC_HEIGHT_OUT}  //Layer 4 bottom-right
};

// VideoPipe struct
typedef struct {
	uint8_t Index;
    VideoPipe *VideoPipePtr;    
} Cam;

/*
 * ActiveCams - the cameras that are found to be actually connected 
 * (as confirmed by I2C comms) are placed in an array so that they 
 * can be iterated
 */
Cam ActiveCams[NUM_CAMS];
u8 NumActiveCams = 0;

/************************** Function Definitions *****************************/

#ifdef STATS_ENABLE
/* Hide the panel: fill the panel rect with black in BOTH working buffers. */
static void hud_clear_panel(void)
{
	u32 stride = (u32)(VMODE_WIDTH*3u);
	for (int r = 0; r < STAT_PANEL_H; r++) {
		uintptr_t off = (uintptr_t)(STAT_PANEL_Y+r)*stride + (uintptr_t)STAT_PANEL_X*3u;
		memset((void *)(uintptr_t)(BG_IMG_BUF   + off), 0, (size_t)STAT_PANEL_W*3u);
		memset((void *)(uintptr_t)(BG_IMG_BUF_B + off), 0, (size_t)STAT_PANEL_W*3u);
	}
	Xil_DCacheFlushRange(BG_IMG_BUF   + (uintptr_t)STAT_PANEL_Y*stride, (uintptr_t)STAT_PANEL_H*stride);
	Xil_DCacheFlushRange(BG_IMG_BUF_B + (uintptr_t)STAT_PANEL_Y*stride, (uintptr_t)STAT_PANEL_H*stride);
}

/* Arm the AXI timer as a free-running 32-bit up-counter (100 MHz, wraps ~every 43 s). */
static inline void rt_arm(void)
{
	Xil_Out32(RT_BASE + RT_TLR,  0u);
	Xil_Out32(RT_BASE + RT_TCSR, RT_LOAD);   /* load 0 into the counter */
	Xil_Out32(RT_BASE + RT_TCSR, RT_MODE);   /* run: up-count, auto-reload */
}

/* Once per ~0.5 s, turn the disc rev-counter and camera frame-sequence deltas into RPM and
 * captures/second, then repaint the on-screen HUD in BOTH background buffers (so the readout
 * survives the ROI/bar double-buffer page-flip). Called every main-loop pass; a couple of
 * register reads until a window actually elapses. */
static void stats_service(void)
{
	/* target gauge under the ROI preview: always shown (its value text only with the HUD),
	 * redrawn when target, its max (cpr/N), the auto flag or the HUD changes. */
	{ static uint32_t last_t = 0xFFFFFFFFu, last_tm = 0xFFFFFFFFu; static int last_a = -1;
#ifdef MBDPU_HAS_POS
	  int a = g_auto_tgt;
#else
	  int a = 0;
#endif
	  uint32_t t = disc_strobe_target(), tm = disc_strobe_target_max();
	  if (g_tgt_redraw || t != last_t || tm != last_tm || a != last_a) {
		g_tgt_redraw = 0; last_t = t; last_tm = tm; last_a = a;
		mnist_target_gauge(BG_IMG_BUF,   (u32)(VMODE_WIDTH*3u), TGT_GAUGE_X, TGT_GAUGE_Y, TGT_GAUGE_W, t, tm, a);
		mnist_target_gauge(BG_IMG_BUF_B, (u32)(VMODE_WIDTH*3u), TGT_GAUGE_X, TGT_GAUGE_Y, TGT_GAUGE_W, t, tm, a);
	  } }

	uint32_t cpr  = disc_strobe_cpr(); if (cpr == 0u) cpr = 1u;
	uint32_t fine = disc_strobe_revs() * cpr + disc_strobe_position();  /* absolute encoder count */

	uint32_t tcsr = Xil_In32(RT_BASE + RT_TCSR);
	if ((tcsr & (RT_MODE | RT_DOWN)) != RT_MODE) {   /* a usleep left it down-count/stopped */
		rt_arm();
		g_rt_last_cy   = 0u;
		g_rt_last_cnt  = fine;
		g_rt_last_seq  = (NumActiveCams > 0) ? ActiveCams[0].VideoPipePtr->Frmbuf.FrameSeq : 0u;
		return;
	}

	uint32_t now = Xil_In32(RT_BASE + RT_TCR);
	uint32_t dcy = now - g_rt_last_cy;               /* wrap-safe unsigned delta */
	if (dcy < (RT_FREQ_HZ / 2u)) return;             /* ~2 updates/second */

	uint32_t seq  = (NumActiveCams > 0) ? ActiveCams[0].VideoPipePtr->Frmbuf.FrameSeq : g_rt_last_seq;
	uint32_t dcnt = fine - g_rt_last_cnt;            /* fine encoder counts this window (245/rev) */
	uint32_t dseq = seq  - g_rt_last_seq;

	/* RPM from fine counts: (dcnt/cpr) rev / (dcy/FREQ) s * 60. Fine resolution -> smooth even
	 * at low speed, where whole-rev counting quantized badly. One-pole smoothing damps jitter. */
	int rpm = (int)(((unsigned long long)dcnt * 60ull * RT_FREQ_HZ) / ((unsigned long long)cpr * dcy));
	g_stat_rpm      = (g_stat_rpm < 0) ? rpm : (g_stat_rpm + rpm + 1) / 2;
	g_stat_caps_x10 = (int)(((unsigned long long)dseq * 10ull * RT_FREQ_HZ) / dcy);

	g_rt_last_cy = now; g_rt_last_cnt = fine; g_rt_last_seq = seq;

	/* Verbose console: one line of the same panel info, at most once per second (independent
	 * of the on-screen HUD toggle). Latencies show 0.0 until the first capture populates them. */
	if (g_verbose && (uint32_t)(now - g_verbose_last_cy) >= RT_FREQ_HZ) {
		g_verbose_last_cy = now;
		int cx = g_stat_caps_x10 < 0 ? 0 : g_stat_caps_x10;
		int px = g_stat_pre_x10  < 0 ? 0 : g_stat_pre_x10;
		int dx = g_stat_dpu_x10  < 0 ? 0 : g_stat_dpu_x10;
		int ox = g_stat_ovl_x10  < 0 ? 0 : g_stat_ovl_x10;
		int gx = (int)(g_gain * 10u / 16u);
		int cpx = (int)disc_strobe_cpr_x10();
		xil_printf("RPM=%d CAP=%d.%d PRE=%d.%d DPU=%d.%d OVL=%d.%d TRIG=%s STRB=%s EXPO=%d GAIN=%d.%d TGT=%d CPR=%d.%d REC=%s %lu/%lu",
		           g_stat_rpm < 0 ? 0 : g_stat_rpm,
		           cx/10, cx%10, px/10, px%10, dx/10, dx%10, ox/10, ox%10,
		           g_trig_active ? "ON" : "OFF", disc_strobe_enabled() ? "ARM" : "OFF",
		           (int)g_exposure, gx/10, gx%10, (int)disc_strobe_target(), cpx/10, cpx%10,
		           g_recording ? "ON" : "off", (unsigned long)g_roi_count, (unsigned long)ROI_GAL_CAP);
#ifdef MBDPU_HAS_POS
		{ int a0 = g_pos_x100[0] < 0 ? -g_pos_x100[0] : g_pos_x100[0];
		  int a1 = g_pos_x100[1] < 0 ? -g_pos_x100[1] : g_pos_x100[1];
		  xil_printf(" DX=%c%d.%02d DY=%c%d.%02d AUTO=%s", g_pos_x100[0] < 0 ? '-' : '+', a0/100, a0%100,
		             g_pos_x100[1] < 0 ? '-' : '+', a1/100, a1%100, g_auto_tgt ? "ON" : "off"); }
#endif
		xil_printf("\r\n");
	}

	if (!g_hud_on) return;   /* measurement keeps running; drawing is toggled off */

	mnist_stats_t st = {
		.rpm          = g_stat_rpm,       .caps_x10 = g_stat_caps_x10,
		.pre_x10      = g_stat_pre_x10,   .dpu_x10  = g_stat_dpu_x10,  .ovl_x10 = g_stat_ovl_x10,
		.trig_on      = g_trig_active,    .strobe_armed = disc_strobe_enabled(),
		.expo         = (int)g_exposure,  .gain_x10 = (int)(g_gain * 10u / 16u),
		.target       = (int)disc_strobe_target(), .cpr_x10 = (int)disc_strobe_cpr_x10(),
	};
	mnist_stats_panel(BG_IMG_BUF,   (u32)(VMODE_WIDTH*3u), STAT_PANEL_X, STAT_PANEL_Y, &st);
	mnist_stats_panel(BG_IMG_BUF_B, (u32)(VMODE_WIDTH*3u), STAT_PANEL_X, STAT_PANEL_Y, &st);
}

/* Write the gallery header (magic/count/layout) and reset the sample count for a new session. */
static void roi_gallery_init(void)
{
	volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)ROI_GAL_BASE;
	h[0] = 0x47494F52u;          /* 'ROIG' */
	h[1] = 0u;                   /* count */
	h[2] = ROI_GAL_CAP;
	h[3] = ROI_SMP_BYTES;        /* per-sample stride */
	h[4] = ROI_GAL_HDR;          /* offset to sample 0 */
	h[5] = ROI_SMP_HDR;          /* per-sample header bytes */
	h[6] = 28u;  h[7] = ROI_SMP_IMG;    /* img dim / bytes */
	h[8] = 112u; h[9] = ROI_SMP_CROP;   /* crop dim / bytes */
	Xil_DCacheFlushRange(ROI_GAL_BASE, ROI_GAL_HDR);
	g_roi_count = 0;
}

/* Append one captured sample (28x28 model input + 112x112 hi-res crop + metadata) to the DDR
 * gallery. Metadata carries the strobe `position` for offline angle-clustering / auto-labeling. */
static void roi_record(uintptr_t cambuf, uint32_t stride, uintptr_t img_src,
                       int pred, const int32_t *logits, uint32_t mean)
{
	if (g_roi_count >= ROI_GAL_CAP) { g_recording = 0; return; }
	uintptr_t s = ROI_GAL_BASE + ROI_GAL_HDR + (uintptr_t)g_roi_count * ROI_SMP_BYTES;
	volatile uint32_t *hd = (volatile uint32_t *)s;
	hd[0] = 0x32494F52u;                 /* 'ROI2' */
	hd[1] = g_roi_count;                 /* seq */
	hd[2] = disc_strobe_position();      /* angle proxy (offline clustering key) */
	hd[3] = disc_strobe_revs();
	hd[4] = disc_strobe_captures();      /* N */
	hd[5] = (uint32_t)pred;              /* predicted digit (QC) */
	hd[6] = mean;                        /* frame brightness (QC) */
#ifdef MBDPU_HAS_POS
	hd[7] = ((uint32_t)(uint16_t)(int16_t)g_pos_x100[0]) |      /* dx x100 (int16, lo) */
	        ((uint32_t)(uint16_t)(int16_t)g_pos_x100[1] << 16);  /* dy x100 (int16, hi) */
#else
	hd[7] = 0u;
#endif
	for (int i = 0; i < 10; i++) ((volatile int32_t *)s)[8 + i] = logits[i];   /* logits at +32 */

#ifdef MBDPU03_CORE
	/* mbdpu03's input is 112x112 (already invalidated after the run): keep the 28x28 field as
	 * its 4x4 box average so the gallery format stays the same as demo01/02. */
	{ const int8_t *in = (const int8_t *)img_src; int8_t *o = (int8_t *)(uintptr_t)(s + ROI_SMP_HDR);
	  for (int oy = 0; oy < 28; oy++) for (int ox = 0; ox < 28; ox++) {
	      int sum = 0;
	      for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) sum += in[(oy*4 + dy)*112 + ox*4 + dx];
	      o[oy*28 + ox] = (int8_t)(sum >> 4); } }
#else
	Xil_DCacheFlushRange((UINTPTR)img_src, ROI_SMP_IMG);   /* ensure model input is coherent */
	memcpy((void *)(uintptr_t)(s + ROI_SMP_HDR), (const void *)img_src, ROI_SMP_IMG);
#endif
	mnist_capture_crop112(cambuf, stride, (int)g_crop_x0, (int)g_crop_y0, (int)(CROP_DIM << g_crop_shift),
	                      (uint8_t *)(uintptr_t)(s + ROI_SMP_HDR + ROI_SMP_IMG));
	Xil_DCacheFlushRange(s, ROI_SMP_BYTES);

	g_roi_count++;
	{ volatile uint32_t *gh = (volatile uint32_t *)(uintptr_t)ROI_GAL_BASE; gh[1] = g_roi_count;
	  Xil_DCacheFlushRange(ROI_GAL_BASE, 8); }
	if ((g_roi_count % 64u) == 0u) xil_printf("REC: %lu/%lu\r\n",
	    (unsigned long)g_roi_count, (unsigned long)ROI_GAL_CAP);
	if (g_roi_count >= ROI_GAL_CAP) { g_recording = 0; xil_printf("REC: full (%lu), stopped\r\n",
	    (unsigned long)g_roi_count); }
}
#endif /* STATS_ENABLE */

/*****************************************************************************/
/**
*
* This function checks the TX Busy flag, and returns TRUE with prompt. and
* FALSE
* @return TRUE/FALSE.
*
* @note   None.
*
******************************************************************************/
u8 CheckTxBusy (void)
{
	if (TxBusy) {
		xil_printf("Either TX still on transition to a new video"
		            " format\r\nor the TX cable is not connected\r\n");
	}
	return (TxBusy);
}

/*****************************************************************************/
/**
*
* This function sets up the IDT 8T49N24x clock generator either in free or 
* locked mode.
*
* @param  Index specifies an index for selecting mode frequency.
* @param  Mode specifies either free or locked mode.
*
* @return
*   - Zero if error in programming external clock.
*   - One if programmed external clock.
*
* @note   None.
*
******************************************************************************/
int I2cClk(u32 InFreq, u32 OutFreq)
{
	int Status;

	/* Reset I2C controller before issuing new transaction.
	 * This is required to recover the IIC controller in case a previous
	 * transaction is pending.
	 */
	XIic_WriteReg(XPAR_AXI_IIC_HDMI_CLK_BASEADDR, XIIC_RESETR_OFFSET,
				  XIIC_RESET_MASK);

	/* Free running mode */
	if (InFreq == 0) {
		Status = IDT_8T49N24x_SetClock((XPAR_AXI_IIC_HDMI_CLK_BASEADDR),
					       (I2C_CLK_ADDR),
					       (IDT_8T49N24X_XTAL_FREQ),
					       OutFreq,
					       TRUE);

		if (Status != (XST_SUCCESS)) {
			xil_printf("Error programming IDT_8T49N241\r\n");
			return XST_FAILURE;
		}
	}

	/* Locked mode */
	else {
		Status = IDT_8T49N24x_SetClock((XPAR_AXI_IIC_HDMI_CLK_BASEADDR),
					       (I2C_CLK_ADDR),
					       InFreq,
					       OutFreq,
					       FALSE);

		if (Status != (XST_SUCCESS)) {
			xil_printf("Error programming IDT_8T49N241\r\n");
			return XST_FAILURE;
		}
	}

	return XST_SUCCESS;
}


/*****************************************************************************/
/**
*
* This function is called when a TX connect event has occurred.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxConnectCallback(void *CallbackRef) {
    xil_printf("TX connect callback\n\r");
	XV_HdmiTxSs *HdmiTxSsPtr = (XV_HdmiTxSs *)CallbackRef;
	if(HdmiTxSsPtr->IsStreamConnected == (FALSE)) {
		/* TX Cable is disconnected */
		TxCableConnect = (FALSE);

		/* Cable is disconnected, don't restart HDMI TX */
		TxRestartHdmiTx = (FALSE);
		/* Cable is disconnected, don't allow any TX operation */
		TxBusy = (TRUE);

		XVphy_IBufDsEnable(&Vphy, 0, XVPHY_DIR_TX, (FALSE));

	} else {
		/* Set TX Cable Connect Flag to (TRUE) as the cable is
		 * connected
		 */
		TxCableConnect = (TRUE);

		TxRestartHdmiTx = (TRUE);
		TxBusy = (FALSE);

		XVphy_IBufDsEnable(&Vphy, 0, XVPHY_DIR_TX, (TRUE));

		/* Initialize EDID App during cable connect */
		EDIDConnectInit(&EdidHdmi20_t);
		/* Read the EDID and the SCDC */
		EdidScdcCheck(HdmiTxSsPtr, &EdidHdmi20_t);

	}
}

/*****************************************************************************/
/**
*
* This function is called when a TX toggle event has occurred.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxToggleCallback(void *CallbackRef) {
	XV_HdmiTxSs_StreamStart(&HdmiTxSs);

}

/*****************************************************************************/
/**
*
* This function is called when the GT TX reference input clock has changed.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void VphyHdmiTxInitCallback(void *CallbackRef) {
	XV_HdmiTxSs_RefClockChangeInit(&HdmiTxSs);
}

/*****************************************************************************/
/**
*
* This function is called when the GT TX has been initialized
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void VphyHdmiTxReadyCallback(void *CallbackRef) {
}


/*****************************************************************************/
/**
*
* This function is called whenever an error condition in VPHY occurs.
* This will fill the FIFO of VPHY error events which will be processed outside
* the ISR.
*
* @param  CallbackRef is the VPHY instance pointer
* @param  ErrIrqType is the VPHY error type
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void VphyErrorCallback(void *CallbackRef) {
	VphyErrorFlag = TRUE;
}

/*****************************************************************************/
/**
*
* This function is called in the application to process the pending
* VPHY errors
*
* @param  None.
*
* @return None.
*
* @note   This function can be expanded to perform necessary actions depending
*		on the error type. For example, XVPHY_ERR_PLL_LAYOUT can be
*		used to automatically switch in and out of bonded mode for
*               GTXE2 devices
*
******************************************************************************/
void VphyProcessError(void) {
	if (VphyErrorFlag == TRUE) {
		xil_printf(ANSI_COLOR_RED "VPHY Error: See log for details"
				   ANSI_COLOR_RESET "\r\n");
	}
	/* Clear Flag */
	VphyErrorFlag = FALSE;

}

/*****************************************************************************/
/**
*
* This function is called when a bridge unlocked has occurred.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxBrdgUnlockedCallback(void *CallbackRef) {
}

/*****************************************************************************/
/**
*
* This function is called when the TX stream is up.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxStreamUpCallback(void *CallbackRef) {

	XHdmiC_AVI_InfoFrame  *AVIInfoFramePtr;
	IsStreamUp = TRUE;
	HdmiStreamUp = TRUE;   /* persistent latch for the per-capture inference gate */

	XV_HdmiTxSs *HdmiTxSsPtr = (XV_HdmiTxSs *)CallbackRef;
	XVphy_PllType TxPllType;
	XVidC_VideoStream *HdmiTxSsVidStreamPtr;

	// Obtain the stream information
	HdmiTxSsVidStreamPtr = XV_HdmiTxSs_GetVideoStream(HdmiTxSsPtr);

	/* Check whether the sink is DVI/HDMI Supported */
	if (EdidHdmi20_t.EdidCtrlParam.IsHdmi == XVIDC_ISDVI) {
		if (HdmiTxSsVidStreamPtr->ColorDepth != XVIDC_BPC_8 ||
			HdmiTxSsVidStreamPtr->ColorFormatId != XVIDC_CSF_RGB) {
			xil_printf(ANSI_COLOR_YELLOW "Un-able to set TX "
						"stream, sink is DVI\r\n"
						ANSI_COLOR_RESET "\r\n");
			/* Clear TX busy flag */
			TxBusy = (FALSE);
			/* Don't set TX, if the Sink is DVI, but the source
			 * properties are:
			 *      - Color Depth more than 8 BPC
			 *      - Color Space not RGB
			 */
			return;
		} else {
			xil_printf(ANSI_COLOR_YELLOW "Set TX stream to DVI,"
				" sink is DVI\r\n" ANSI_COLOR_RESET "\r\n");
			XV_HdmiTxSs_AudioMute(HdmiTxSsPtr, TRUE);
			XV_HdmiTxSS_SetDviMode(HdmiTxSsPtr);
		}
	} else {
#ifdef HDMI_USE_HDMIPHY1
		/* HDMI 2.1 splits HDMI mode into TMDS vs FRL. The camera pipeline
		 * runs at TMDS clock rates (<=340MHz, e.g. 1080p), so use TMDS. */
		XV_HdmiTxSS1_SetHdmiTmdsMode(HdmiTxSsPtr);
#else
		XV_HdmiTxSS_SetHdmiMode(HdmiTxSsPtr);
#endif
		XV_HdmiTxSs_AudioMute(HdmiTxSsPtr, FALSE);
	}

	xil_printf("TX stream is up\r\n");

	/* Check for the 480i/576i during color bar mode
	 * When it's (TRUE), set the Info Frame Pixel Repetition to x2
	 */
	AVIInfoFramePtr = XV_HdmiTxSs_GetAviInfoframe(HdmiTxSsPtr);

	if ( (HdmiTxSsVidStreamPtr->VmId == XVIDC_VM_1440x480_60_I) ||
		     (HdmiTxSsVidStreamPtr->VmId == XVIDC_VM_1440x576_50_I) ) {
			AVIInfoFramePtr->PixelRepetition =
					XHDMIC_PIXEL_REPETITION_FACTOR_2;
	} else {
			AVIInfoFramePtr->PixelRepetition =
					XHDMIC_PIXEL_REPETITION_FACTOR_1;
	}
	TxPllType = XVphy_GetPllType(&Vphy,
	                             0,
				     XVPHY_DIR_TX,
				     XVPHY_CHANNEL_ID_CH1);

	if ((TxPllType == XVPHY_PLL_TYPE_CPLL)) {
		TxLineRate = XVphy_GetLineRateHz(&Vphy,
		                                 0,
						 XVPHY_CHANNEL_ID_CH1);

	} else if((TxPllType == XVPHY_PLL_TYPE_QPLL) ||
			  (TxPllType == XVPHY_PLL_TYPE_QPLL0) ||
			  (TxPllType == XVPHY_PLL_TYPE_PLL0)) {
		TxLineRate =
			XVphy_GetLineRateHz(&Vphy, 0, XVPHY_CHANNEL_ID_CMN0);
	} else {
		TxLineRate =
			XVphy_GetLineRateHz(&Vphy, 0, XVPHY_CHANNEL_ID_CMN1);
	}

	/* Copy Sampling Rate */
	XV_HdmiTxSs_SetSamplingRate(HdmiTxSsPtr, Vphy.HdmiTxSampleRate);

	/* Clear TX busy flag */
	TxBusy = (FALSE);

}

/*****************************************************************************/
/**
*
* This function is called when the TX stream is down.
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxStreamDownCallback(void *CallbackRef) {
	HdmiStreamUp = FALSE;   /* pause per-capture inference until the link is back up */
	xil_printf("TX stream is down\r\n");
}

/*****************************************************************************/
/**
*
* This function is called when a TX Bridge Overflow event has occurred.
* TX Video Bridge Debug Utility
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxBrdgOverflowCallback(void *CallbackRef) {

	/* xil_printf(ANSI_COLOR_YELLOW "TX Video Bridge Overflow"
			ANSI_COLOR_RESET "\r\n"); */
}

/*****************************************************************************/
/**
*
* This function is called when a TX Bridge Underflow event has occurred.
* TX Video Bridge Debug Utility
*
* @param  None.
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void TxBrdgUnderflowCallback(void *CallbackRef) {

	/* xil_printf(ANSI_COLOR_YELLOW "TX Video Bridge Underflow"
			ANSI_COLOR_RESET "\r\n"); */
}


/*****************************************************************************/
/**
*
* This function enables the HDMI TX
*
* @param VphyPtr is a pointer to the VPHY core instance.
* @param HdmiTxSsPtr is a pointer to the XV_HdmiTxSs instance.
* @param Requested Video mode
* @param Requested ColorFormat
* @param Requested ColorDepth
* @param Requested Pixels per clock
*
* @return None.
*
* @note   None.
*
******************************************************************************/
void EnableHdmiTx(XVphy                *VphyPtr,
		    XV_HdmiTxSs          *HdmiTxSsPtr,
		    XVidC_VideoMode      VideoMode,
		    XVidC_ColorFormat    ColorFormat,
		    XVidC_ColorDepth     Bpc) {

	u32 TmdsClock = 0;
	u32 Result;
	XVidC_VideoStream *HdmiTxSsVidStreamPtr;
	XHdmiC_AVI_InfoFrame *AviInfoFramePtr;
	XHdmiC_VSIF *VSIFPtr;

	HdmiTxSsVidStreamPtr = XV_HdmiTxSs_GetVideoStream(HdmiTxSsPtr);
	AviInfoFramePtr = XV_HdmiTxSs_GetAviInfoframe(HdmiTxSsPtr);
	VSIFPtr = XV_HdmiTxSs_GetVSIF(HdmiTxSsPtr);

	/* Reset Avi InfoFrame */
	(void)memset((void *)AviInfoFramePtr, 0, sizeof(XHdmiC_AVI_InfoFrame));
	/* Reset Vendor Specific InfoFrame */
	(void)memset((void *)VSIFPtr, 0, sizeof(XHdmiC_VSIF));

	/* Check if the TX isn't busy already */
	if (!CheckTxBusy()) {

		TxBusy = (TRUE);         /* Set TX busy flag */
#if(CUSTOM_RESOLUTION_ENABLE == 1)
		if (VideoMode < XVIDC_VM_NUM_SUPPORTED ||
		       (VideoMode > XVIDC_VM_CUSTOM &&
		        VideoMode < (XVidC_VideoMode)XVIDC_CM_NUM_SUPPORTED)) {
#else
		if (VideoMode < XVIDC_VM_NUM_SUPPORTED) {
#endif

			/* Disable TX TDMS clock */
			XVphy_Clkout1OBufTdsEnable(VphyPtr,
						XVPHY_DIR_TX,
						(FALSE));

		} else {
			TxBusy = (FALSE);
			xil_printf("Video mode not supported, please change"
					" video mode\r\n");
			return;
		}

#ifdef HDMI_USE_HDMIPHY1
		/* HDMI 2.1 SetStream takes VideoTiming + FrameRate (not a VideoMode
		 * enum) and returns the TMDS clock through a u64* out-parameter. */
		{
			const XVidC_VideoTimingMode *VmPtr =
					XVidC_GetVideoModeData(VideoMode);
			u64 TmdsClk64 = 0;
			XV_HdmiTxSs1_SetStream(HdmiTxSsPtr,
						VmPtr->Timing,
						VmPtr->FrameRate,
						ColorFormat,
						Bpc,
						FALSE,   /* IsDSCompressed */
						NULL,    /* Info3D */
						&TmdsClk64);
			TmdsClock = (u32)TmdsClk64;
			xil_printf("DBG VideoMode=%d FrameRate=%d HActive=%u VActive=%u "
				"TmdsClk64=%u TmdsClock=%u\r\n",
				(int)VideoMode, (int)VmPtr->FrameRate,
				(unsigned)VmPtr->Timing.HActive,
				(unsigned)VmPtr->Timing.VActive,
				(unsigned)TmdsClk64, (unsigned)TmdsClock);
		}
#else
		TmdsClock = XV_HdmiTxSs_SetStream(HdmiTxSsPtr,
						VideoMode,
						ColorFormat,
						Bpc,
						NULL);
#endif

		/* Calling this API can enable or disable scrambler even
		 * if the TMDS clock is >340MHz.
		 * E.g:
		 * 	XV_HdmiTxSs_SetScrambler(HdmiTxSsPtr, TRUE);
		 */

		/* Update AVI InfoFrame */
		AviInfoFramePtr->Version = 2;
		AviInfoFramePtr->ColorSpace =
				XV_HdmiC_XVidC_To_IfColorformat(ColorFormat);
#ifdef HDMI_USE_HDMIPHY1
		AviInfoFramePtr->VIC =
				HdmiTxSsPtr->HdmiTx1Ptr->Stream.Vic;
#else
		AviInfoFramePtr->VIC =
				HdmiTxSsPtr->HdmiTxPtr->Stream.Vic;
#endif

		/* Set TX reference clock */
		VphyPtr->HdmiTxRefClkHz = TmdsClock;

		/* Set GT TX parameters */
		Result = XVphy_SetHdmiTxParam(VphyPtr,
					0,
					XVPHY_CHANNEL_ID_CHA,
					HdmiTxSsVidStreamPtr->PixPerClk,
					HdmiTxSsVidStreamPtr->ColorDepth,
					HdmiTxSsVidStreamPtr->ColorFormatId);

		if (Result == (XST_FAILURE)) {
			TxBusy = (FALSE);
			xil_printf
			  ("Unable to set requested TX video resolution.\r\n");
			xil_printf
			  ("Returning to previously TX video resolution.\r\n");
			return;
		}

		/* Disable RX clock forwarding */
		XVphy_Clkout1OBufTdsEnable(VphyPtr, XVPHY_DIR_RX, (FALSE));

		/* Program external clock generator in free running mode */
		I2cClk(0, VphyPtr->HdmiTxRefClkHz);
	}
}

void Xil_AssertCallbackRoutine(u8 *File, s32 Line) {
	xil_printf("Assertion in File %s, on line %0d\r\n", File, Line);
}

/*
 * Initializes a video pipe by index
 * First releases the resets, then initializes the video pipe
 */
static void try_init_pipe(int idx, VideoPipe* cam, const VideoPipeBaseAddr* addrs)
{
    int st;
    xil_printf("Probing CAM%d (iic=0x%08x gpio=0x%08x)...\r\n",
               idx, (unsigned)addrs->Iic, (unsigned)addrs->Gpio);
    st = pipe_init(cam, addrs);
    if (st == XST_SUCCESS && cam->IsConnected) {
        xil_printf("  CAM%d DETECTED\r\n", idx);
        ActiveCams[NumActiveCams].Index = (uint8_t)idx;
        ActiveCams[NumActiveCams].VideoPipePtr = cam;
        ++NumActiveCams;
    } else {
        xil_printf("  CAM%d not detected (pipe_init=%d IsConnected=%d)\r\n",
                   idx, st, (int)cam->IsConnected);
    }
}

// Returns the name of a specific camera model
static const char* cam_type_name(uint8_t model)
{
    switch (model) {
        case RPI_CAM_TYPE_OV5640: return "Digilent P-Cam";
        case RPI_CAM_TYPE_IMX219: return "Raspberry Pi Camera v2";
        case RPI_CAM_TYPE_IMX477: return "Raspberry Pi HQ / IMX477";
        case RPI_CAM_TYPE_AR0234: return "Arducam AR0234 (B0429)";
        case RPI_CAM_TYPE_OV9281: return "Arducam OV9281 (B0165)";
        default:                  return "Unknown";
    }
}

/*
 * Main function
 */
int main() {
	u32 Status = XST_FAILURE;
	XVphy_Config *XVphyCfgPtr;
	XVidC_VideoStream *HdmiTxSsVidStreamPtr;
    XVidC_VideoTiming const *TimingPtr;

	xil_printf("\r\n\r\n");
	xil_printf("################################################\r\n");
	xil_printf("#                SCU200 AI Demo                #\r\n");
	xil_printf("#              mariobergeron.com               #\r\n");
	xil_printf("################################################\r\n");

	TxBusy            = (TRUE);
	TxRestartHdmiTx = (FALSE);
	VphyErrorFlag = FALSE;
	VphyPllLayoutErrorFlag = FALSE;

	/* Initialize platform */
	init_platform();

	/* Initialize IIC */
	IDT_8T49N24x_Init(XPAR_AXI_IIC_HDMI_CLK_BASEADDR, I2C_CLK_ADDR);

#ifdef HDMI_USE_HDMIPHY1
	/* Initialize the TI TMDS1204 HDMI TX redriver (U40) on the same bus. */
	HdmiCtlIic.BaseAddress = XPAR_AXI_IIC_HDMI_CLK_BASEADDR;
	if (TI_TMDS1204_CheckDeviceID(&HdmiCtlIic, HDMI_TX_REDRIVER_ADDR)
			== XST_SUCCESS) {
		u8 Ver = TI_TMDS1204_CheckDeviceVersion(&HdmiCtlIic,
				HDMI_TX_REDRIVER_ADDR);
		/* The driver implements a single (R1) register set. The reference
		 * design maps BOTH 0x01 and 0x03 silicon to "Revision 1"
		 * (VFMC_MEZZ_HDMI_TI_R1 == _R3 == 0x70000201), i.e. it always
		 * passes Revision=1. Confirmed on SCU200 HW: the redriver reads
		 * device version 0x03 but must be programmed as Revision 1 —
		 * passing 3 would hit the driver's empty default case and program
		 * nothing. So always use Revision 1 here. */
		xil_printf("TI TMDS1204 HDMI TX redriver: device version 0x%02x\r\n",
				Ver);
		TxRedriverRev = 1;
		TI_TMDS1204_Init(&HdmiCtlIic, HDMI_TX_REDRIVER_ADDR,
				TxRedriverRev, 1 /*IsTx*/);
	} else {
		xil_printf(ANSI_COLOR_YELLOW
			"WARNING: TI TMDS1204 HDMI TX redriver not found at 0x%02x\r\n"
			ANSI_COLOR_RESET, HDMI_TX_REDRIVER_ADDR);
	}
#endif

	/* Initialize IRQ */
	Xil_ExceptionInit();

#ifndef HDMI_USE_HDMIPHY1
	/*
	 * Register the interrupt controller handler with the exception table.
	 * (Legacy path only.) For the HDMI 2.1 build, XSetupInterruptSystem() below
	 * registers XIntc_InterruptHandler against its own managed, initialized intc
	 * instance. Doing it here with the app's uninitialized `Intc` conflicts with
	 * that and causes a flaky hang: an early/pending interrupt is serviced by
	 * XIntc_InterruptHandler(&Intc) which asserts because Intc.IsReady is unset.
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
				(Xil_ExceptionHandler)XIntc_InterruptHandler,
				(XIntc *)&Intc);
#endif

	/* Initialize HDMI TX Subsystem */

	XV_HdmiTxSs_ConfigPtr =
		XV_HdmiTxSs_LookupConfig(XPAR_XV_HDMITX_0_BASEADDR);

	if(XV_HdmiTxSs_ConfigPtr == NULL) {
		HdmiTxSs.IsReady = 0;
	}

	/* Initialize top level and all included sub-cores */
	Status = XV_HdmiTxSs_CfgInitialize(&HdmiTxSs, XV_HdmiTxSs_ConfigPtr,
					XV_HdmiTxSs_ConfigPtr->BaseAddress);
	if(Status != XST_SUCCESS) {
		xil_printf
		       ("ERR:: HDMI TX Subsystem Initialization failed %d\r\n",
		        Status);
	}

#ifdef HDMI_USE_HDMIPHY1
	/* The HDMI 2.1 TXSS does NOT enable the TX PIO block in CfgInitialize the
	 * way the legacy v_hdmi_tx_ss did (which called PioEnable during init).
	 * Without the PIO running, HPD sensing is dead (PIO_IN reads 0, no connect
	 * interrupt), so the monitor is never detected. Start the subsystem here to
	 * enable the PIO; this only turns on input sensing, it does not start a
	 * video stream. */
	XV_HdmiTxSs1_Start(&HdmiTxSs);
#endif

	/* Set the Application version in TXSs driver structure */
	XV_HdmiTxSS_SetAppVersion(&HdmiTxSs, APP_MAJ_VERSION, APP_MIN_VERSION);

	/* Register HDMI TX SS Interrupt Handler with Interrupt Controller */
	Status = XSetupInterruptSystem(&HdmiTxSs,
				       XV_HdmiTxSS_HdmiTxIntrHandler,
				       XV_HdmiTxSs_ConfigPtr->IntrId[INTRNAME_HDMITX],
				       XV_HdmiTxSs_ConfigPtr->IntrParent,
				       XINTERRUPT_DEFAULT_PRIORITY);
	if (Status == XST_SUCCESS) {
	} else {
		xil_printf
			("ERR:: Unable to register HDMI TX interrupt handler");
		xil_printf("HDMI TX SS initialization error\r\n");
		return XST_FAILURE;
	}

	/* HDMI TX SS callback setup */
	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_CONNECT,
				(void *)TxConnectCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_TOGGLE,
				(void *)TxToggleCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_BRDGUNLOCK,
				(void *)TxBrdgUnlockedCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_BRDGOVERFLOW,
				(void *)TxBrdgOverflowCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_BRDGUNDERFLOW,
				(void *)TxBrdgUnderflowCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_STREAM_UP,
				(void *)TxStreamUpCallback,
				(void *)&HdmiTxSs);

	XV_HdmiTxSs_SetCallback(&HdmiTxSs,
				XV_HDMITXSS_HANDLER_STREAM_DOWN,
				(void *)TxStreamDownCallback,
				(void *)&HdmiTxSs);

	/*
	 *  Initialize Video PHY
	 *  The GT needs to be initialized after the HDMI RX and TX.
	 *  The reason for this is the GtRxInitStartCallback
	 *  calls the RX stream down callback.
	 *
         */
	XVphyCfgPtr = XVphy_LookupConfig(XPAR_XVPHY_0_BASEADDR);
	if (XVphyCfgPtr == NULL) {
		xil_printf("Video PHY device not found\r\n\r\n");
		return XST_FAILURE;
	}

#ifdef HDMI_USE_HDMIPHY1
	/* HDMI 2.1 PHY (v_hdmi_phy1): initialize the instance BEFORE enabling its
	 * interrupt. This PHY can already be asserting an interrupt at this point
	 * (e.g. PLL/GT status), and XHdmiphy1_InterruptHandler hangs forever in an
	 * assert if it runs before the instance is marked ready. The legacy Video
	 * PHY did not have a pending interrupt here, so the original order (enable
	 * then init) was safe on other boards. */
	Status = XVphy_Hdmi_CfgInitialize(&Vphy, 0, XVphyCfgPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("HDMI VPHY initialization error\r\n");
		return XST_FAILURE;
	}
	Status = XSetupInterruptSystem(&Vphy,
				(XInterruptHandler)XVphy_InterruptHandler,
				XVphyCfgPtr->IntrId,
				XVphyCfgPtr->IntrParent,
				XINTERRUPT_DEFAULT_PRIORITY);
	if (Status != XST_SUCCESS) {
		xil_printf("HDMI VPHY Interrupt Vec ID not found!\r\n");
		return XST_FAILURE;
	}
#else
	/* Register VPHY Interrupt Handler */
	Status = XSetupInterruptSystem(&Vphy,
				(XInterruptHandler)XVphy_InterruptHandler,
				XVphyCfgPtr->IntrId,
				XVphyCfgPtr->IntrParent,
				XINTERRUPT_DEFAULT_PRIORITY);
	if (Status != XST_SUCCESS) {
		xil_printf("HDMI VPHY Interrupt Vec ID not found!\r\n");
		return XST_FAILURE;
	}

	/* Initialize HDMI VPHY */
	Status = XVphy_Hdmi_CfgInitialize(&Vphy, 0, XVphyCfgPtr);

	if (Status != XST_SUCCESS) {
		xil_printf("HDMI VPHY initialization error\r\n");
		return XST_FAILURE;
	}
#endif

	/* Enable VPHY Interrupt */
	/* VPHY callback setup */
	XVphy_SetHdmiCallback(&Vphy,
				XVPHY_HDMI_HANDLER_TXINIT,
				(void *)VphyHdmiTxInitCallback,
				(void *)&Vphy);
	XVphy_SetHdmiCallback(&Vphy,
				XVPHY_HDMI_HANDLER_TXREADY,
				(void *)VphyHdmiTxReadyCallback,
				(void *)&Vphy);

	XVphy_SetErrorCallback(&Vphy,
				(void *)VphyErrorCallback,
				(void *)&Vphy);

    /*
     * Initialize the main AXI GPIO for resets
     */
    GpioVideoResets_ConfigPtr = XGpio_LookupConfig(XPAR_MAIN_RESETN_GPIO_BASEADDR);
	if(GpioVideoResets_ConfigPtr == NULL) {
        xil_printf("GPIO for main resets not found\r\n");
		GpioVideoResets.IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	Status = XGpio_CfgInitialize(&GpioVideoResets,
				GpioVideoResets_ConfigPtr,
				GpioVideoResets_ConfigPtr->BaseAddress);
	if(Status != XST_SUCCESS) {
		xil_printf("Failed to initialize GPIO for Video pipe resets\r\n");
		return(XST_FAILURE);
	}

	// Set GPIO directions (1=input, 0=output) - ALL OUTPUTS
	XGpio_SetDataDirection(&GpioVideoResets, 1, 0x00);

	// Assert reset for Video Mixer and Test Pattern Generator
	reset_assert(RESET_MASK_VMIX | RESET_MASK_VTPG);
    usleep(10000);

	/*
	 * Initialize and configure the Video Mixer
	 */
    reset_deassert(RESET_MASK_VMIX);
	XVidC_VideoStream Stream;
	XVidC_ColorFormat Cfmt;
	Status  = XVMix_Initialize(&VMix, XPAR_V_MIX_BASEADDR);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Video Mixer device not initialized\r\n");
		return(XST_FAILURE);
	}

	// Video stream properties of the mixer's master input and output
	Stream.VmId = XVidC_GetVideoModeId(VMODE_WIDTH,VMODE_HEIGHT,VMODE_FRAMERATE,FALSE);
	XVMix_GetLayerColorFormat(&VMix, XVMIX_LAYER_MASTER, &Cfmt);
	Stream.PixPerClk = VMix.Mix.Config.PixPerClk;
	Stream.ColorFormatId = Cfmt;
	Stream.ColorDepth = VMix.Mix.Config.MaxDataWidth;
	TimingPtr = XVidC_GetTimingInfo(Stream.VmId);
	Stream.Timing = *TimingPtr;
	Stream.FrameRate = XVidC_GetFrameRate(Stream.VmId);

	XVMix_LayerDisable(&VMix, XVMIX_LAYER_MASTER);
	XVMix_LayerDisable(&VMix, XVMIX_LAYER_ALL);
	XVMix_SetVidStream(&VMix, &Stream);

	// Background color used when a streaming layer is disabled
	XVMix_SetBackgndColor(&VMix, XVMIX_BKGND_GREEN, Stream.ColorDepth);

	XVidC_VideoWindow Win;
	u32 Stride;
	for(u8 layerIndex = 0; layerIndex < NUM_CAMS; layerIndex++) {
		Win = MixLayerConfig[layerIndex];
		XVMix_GetLayerColorFormat(&VMix, layerIndex+1, &Cfmt);
		Stride = ((Cfmt == XVIDC_CSF_YCRCB_422) ? 2: 4); //BytesPerPixel
		Stride *= Win.Width;
		Status = XVMix_SetLayerWindow(&VMix, layerIndex+1, &Win, Stride);
		if(Status != XST_SUCCESS) {
			xil_printf("ERROR: Failed to set window for Video Mixer layer %d\r\n",layerIndex+1);
		}
	}

	XVMix_LayerEnable(&VMix, XVMIX_LAYER_MASTER);
	XVMix_InterruptDisable(&VMix);
	XVMix_Start(&VMix);

	/*
	 * Background layer (Option A): a static 1920x1080 image is streamed from DDR
	 * (BG_IMG_BUF) by bg_frmbuf_rd into the v_mix master layer, replacing the TPG
	 * cross-hatch. The image is JTAG-loaded to BG_IMG_BUF at program time; here we
	 * just configure and start the reader (it free-runs via auto-restart).
	 * RESET_MASK_VTPG now releases bg_frmbuf_rd (it reuses the old TPG reset slice).
	 */
	reset_deassert(RESET_MASK_VTPG);
#if defined(XPAR_BG_FRMBUF_RD_BASEADDR)
	/* Make two working copies from the pristine JTAG-loaded image (ORIG stays untouched so any
	 * overlay can be erased with a clean blit from it — robust across hot-reloads, where the
	 * working buffers would otherwise still hold the previous run's overlays). Draw the fixed
	 * 0-9 confidence-bar labels into the front copy and mirror it to the back. Done BEFORE the
	 * display DMA starts below so it never scans an unfilled working buffer. */
	{
		size_t bgsz = (size_t)VMODE_WIDTH * VMODE_HEIGHT * 3u;
		Xil_DCacheInvalidateRange(BG_IMG_ORIG, bgsz);                      /* JTAG-loaded, not cached */
		memcpy((void *)(uintptr_t)BG_IMG_BUF, (const void *)(uintptr_t)BG_IMG_ORIG, bgsz);
#ifdef MBDPU03_CORE
		mnist_set_bg_labels(1);       /* demo03 background has DX/DY, 0-9 and "Target Offset" */
#else
		mnist_draw_static(BG_IMG_BUF, (u32)(VMODE_WIDTH*3u));             /* fixed 0-9 labels (once) */
#endif
		memcpy((void *)(uintptr_t)BG_IMG_BUF_B, (const void *)(uintptr_t)BG_IMG_BUF, bgsz);
		Xil_DCacheFlushRange(BG_IMG_BUF,   bgsz);
		Xil_DCacheFlushRange(BG_IMG_BUF_B, bgsz);
#ifdef STATS_ENABLE
		mnist_set_tick(rt_tick);
		mnist_set_show_values(g_hud_on);
#endif
	}
	{
		u32 bgStride = 0;
		Status = BgFrmbufRdInit(&BgFrmbufRd, XPAR_BG_FRMBUF_RD_BASEADDR, BG_IMG_BUF,
		                        VMODE_WIDTH, VMODE_HEIGHT, &bgStride);
		if (Status != XST_SUCCESS) {
			xil_printf("ERR:: background frmbuf_rd init failed %d\r\n", Status);
			return (XST_FAILURE);
		}
		XVFrmbufRd_Start(&BgFrmbufRd);
		xil_printf("background: %ux%u image @ 0x%08X stride %u -> v_mix master\r\n",
		           (unsigned)VMODE_WIDTH, (unsigned)VMODE_HEIGHT,
		           (unsigned)BG_IMG_BUF, (unsigned)bgStride);
	}
#elif defined(XPAR_V_TPG_BASEADDR)
	/* No bg image reader in this variant (a monochrome build): drive
	 * the v_mix master with the v_tpg cross-hatch so the mixer has a timing master. */
	{
		XV_tpg_Config *Tpg_ConfigPtr = XV_tpg_LookupConfig(XPAR_V_TPG_BASEADDR);
		if (Tpg_ConfigPtr == NULL) {
			xil_printf("ERR:: v_tpg LookupConfig failed\r\n");
			return (XST_FAILURE);
		}
		Status = XV_tpg_CfgInitialize(&Tpg, Tpg_ConfigPtr, Tpg_ConfigPtr->BaseAddress);
		if (Status != XST_SUCCESS) {
			xil_printf("ERR:: v_tpg init failed %d\r\n", Status);
			return (XST_FAILURE);
		}
		XV_tpg_DisableAutoRestart(&Tpg);
		XV_tpg_Set_height(&Tpg, VMODE_HEIGHT);
		XV_tpg_Set_width(&Tpg, VMODE_WIDTH);
		XV_tpg_Set_colorFormat(&Tpg, Cfmt);
		XV_tpg_Set_bckgndId(&Tpg, XTPG_BKGND_CROSS_HATCH);
		XV_tpg_Set_ovrlayId(&Tpg, 0);
		XV_tpg_EnableAutoRestart(&Tpg);
		XV_tpg_Start(&Tpg);
		xil_printf("background: v_tpg cross-hatch -> v_mix master\r\n");
	}
#endif
	
	/* Start with 1080p stream */
#ifdef HDMI_USE_HDMIPHY1
	{
		const XVidC_VideoTimingMode *VmPtr =
				XVidC_GetVideoModeData(XVIDC_VM_1920x1080_60_P);
		u64 TmdsClk64 = 0;
		XV_HdmiTxSs1_SetStream(&HdmiTxSs,
			VmPtr->Timing,
			VmPtr->FrameRate,
			XVIDC_CSF_RGB,
			XVIDC_BPC_8,
			FALSE,   /* IsDSCompressed */
			NULL,    /* Info3D */
			&TmdsClk64);
	}
#else
	XV_HdmiTxSs_SetStream(
		&HdmiTxSs,
		XVIDC_VM_1920x1080_60_P,
		XVIDC_CSF_RGB,
		XVIDC_BPC_8,
		NULL);
#endif

	/*
	 * Initialize Video pipe for each camera
	 */
#ifdef XPAR_MIPI_0_AXI_IIC_0_BASEADDR
    try_init_pipe(0, &Cam0, &CamBaseAddr0);
#endif
#ifdef XPAR_MIPI_1_AXI_IIC_0_BASEADDR
    try_init_pipe(1, &Cam1, &CamBaseAddr1);
#endif
#ifdef XPAR_MIPI_2_AXI_IIC_0_BASEADDR
    try_init_pipe(2, &Cam2, &CamBaseAddr2);
#endif
#ifdef XPAR_MIPI_3_AXI_IIC_0_BASEADDR
    try_init_pipe(3, &Cam3, &CamBaseAddr3);
#endif
    
	if(NumActiveCams == 0) {
		/* No cameras detected. Rather than quit, continue and bring up HDMI
		 * anyway: the bg_frmbuf_rd -> v_mix master layer (already started above)
		 * drives the static background image, so the HDMI output path can be
		 * verified without any camera attached. Camera overlays stay disabled. */
		xil_printf("WARNING: No cameras detected — showing background image "
				"over HDMI.\n\r");
	}

	/*
	 * List the connected cameras
	 */
	xil_printf("\n\rDetected %d connected cameras:\n\r",NumActiveCams);
    for (int i = 0; i < NumActiveCams; ++i) {
        int layer = mixer_channel_for_mipi(ActiveCams[i].Index);
        RpiCamera *rcam = &(ActiveCams[i].VideoPipePtr->Camera);

        const char *type  = cam_type_name(rcam->sensor.model);

        xil_printf("  - CAM%-1d: %-24s to Mixer layer %-2d\r\n",
                ActiveCams[i].Index, type, layer);
    }

	/*
	 * Start the connected cameras and enable corresponding mixer layers
	 */
    xil_printf("\n\rConfiguring cameras:\n\r");
	for(int i = 0; i < NumActiveCams; i++) {
        xil_printf("  - CAM%-1d: ",ActiveCams[i].Index);
		Status = pipe_start_camera(ActiveCams[i].VideoPipePtr);
        if(Status == XST_SUCCESS) {
            xil_printf("SUCCESS\n\r");
        } else {
            xil_printf("FAILED\n\r");
        }
		/* Windows are tiled (non-overlapping), so enable every camera layer. */
		XVMix_LayerEnable(&VMix, mixer_channel_for_mipi(ActiveCams[i].Index));
	}
    xil_printf("\n\r");

#if defined(DISC_STROBE_PRESENT)
    /* If an OV9281 is on the bus, wire the disc_strobe 'F' key to its FSIN trigger mode.
     * Stored iic_id lets the console toggle the sensor between free-run and one-frame-per
     * -FSIN-pulse without threading state through the video loop. */
    for (int i = 0; i < NumActiveCams; i++) {
        RpiCamera *rcam = &(ActiveCams[i].VideoPipePtr->Camera);
        if (rcam->sensor.model == RPI_CAM_TYPE_OV9281) {
            g_ov9281_iic = rcam->iic_id;
            g_ov9281_present = 1;
            g_cam_frmbuf = &(ActiveCams[i].VideoPipePtr->Frmbuf);   /* for the 'B' FrameSeq probe */
            g_ov9281_cam = rcam;                                     /* for cam_watchdog() */
            ov9281_set_exposure(rcam->iic_id, g_exposure);          /* apply default exposure */
            ov9281_set_gain(rcam->iic_id, g_gain);                  /* apply default gain (brightness) */
            disc_strobe_set_cam_cb(ov9281_fsin_trigger_cb);
            xil_printf("strobe: 'F' key wired to OV9281 FSIN trigger (iic %u)\r\n",
                       (unsigned)g_ov9281_iic);
            break;
        }
    }
#endif

/* MBDPU01_BASE = "the DPU": mbdpu02 (demo02, same register map + a position head) when
 * present, else mbdpu01 (demo01). */
#if defined(XPAR_MBDPU03_S_AXI_CONTROL_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU03_S_AXI_CONTROL_BASEADDR
#elif defined(XPAR_MBDPU03_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU03_BASEADDR
#elif defined(XPAR_MBDPU02_S_AXI_CONTROL_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU02_S_AXI_CONTROL_BASEADDR
#elif defined(XPAR_MBDPU02_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU02_BASEADDR
#elif defined(XPAR_MBDPU01_S_AXI_CONTROL_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU01_S_AXI_CONTROL_BASEADDR
#elif defined(XPAR_MBDPU01_BASEADDR)
#  define MBDPU01_BASE   XPAR_MBDPU01_BASEADDR   /* 2025.2 SDT flow names it this */
#endif
#ifdef MBDPU01_BASE
	/* mbdpu01 = 28x28x1 INT8 MNIST classifier sharing DDR with the display.
	 * Weights are JTAG-loaded to DDR once at program time (weight-agnostic core),
	 * so retraining needs no rebuild. img is (re)written each classification. */
#  define MBDPU01_WBLOB  0xB0000000u   /* weights int8[9064] (mbdpu02: [10632]) - JTAG-loaded */
#  define MBDPU01_PBLOB  0xB0010000u   /* params  int32[38]  (mbdpu02: [42])    - JTAG-loaded */
#  define MBDPU01_IMG    0xB0020000u   /* 28x28 image - written each frame by preprocess (mbdpu03: 112x112, written back by the core) */
#  define MBDPU01_OUT    0xB0030000u   /* result int32[11] (mbdpu02: [13] + dx, dy) */
#if defined(XPAR_MBPRE_S_AXI_CONTROL_BASEADDR)
#  define MBPRE_BASE XPAR_MBPRE_S_AXI_CONTROL_BASEADDR
#elif defined(XPAR_MBPRE_BASEADDR)
#  define MBPRE_BASE XPAR_MBPRE_BASEADDR   /* preprocessing offloaded to fabric */
#endif
#if defined(MBDPU03_CORE)
	mnist_set_input_dim(MBDPU03_DIM);
	xil_printf("mbdpu03 112x112 MNIST classifier + dx/dy position head ready (camera crop in core): "
	           "ctrl 0x%08x, weights @0x%08x/0x%08x\n\r",
#elif defined(MBDPU_HAS_POS)
	xil_printf("mbdpu02 MNIST classifier + dx/dy position head ready: ctrl 0x%08x, "
	           "weights @0x%08x/0x%08x (inverted-model, invert=0)\n\r",
#else
	xil_printf("mbdpu01 MNIST classifier ready: ctrl 0x%08x, weights @0x%08x/0x%08x "
	           "(inverted-model, invert=0)\n\r",
#endif
	           (unsigned)MBDPU01_BASE, (unsigned)MBDPU01_WBLOB, (unsigned)MBDPU01_PBLOB);
#endif

#ifdef STATS_ENABLE
	/* Start the free-running rate timer, baseline the counters, and paint an initial
	 * "RPM -- / CAP/S --" HUD into both background buffers (right of the camera window). */
	rt_arm();
	g_rt_last_cy   = Xil_In32(RT_BASE + RT_TCR);
	{ uint32_t c = disc_strobe_cpr(); if (c == 0u) c = 1u;
	  g_rt_last_cnt = disc_strobe_revs() * c + disc_strobe_position(); }
	g_rt_last_seq  = (NumActiveCams > 0) ? ActiveCams[0].VideoPipePtr->Frmbuf.FrameSeq : 0u;
	if (g_hud_on) { mnist_stats_t st0 = {
		.rpm = -1, .caps_x10 = -1, .pre_x10 = -1, .dpu_x10 = -1, .ovl_x10 = -1,
		.trig_on = g_trig_active, .strobe_armed = disc_strobe_enabled(),
		.expo = (int)g_exposure, .gain_x10 = (int)(g_gain * 10u / 16u),
		.target = (int)disc_strobe_target(), .cpr_x10 = (int)disc_strobe_cpr_x10(),
	  };
	  mnist_stats_panel(BG_IMG_BUF,   (u32)(VMODE_WIDTH*3u), STAT_PANEL_X, STAT_PANEL_Y, &st0);
	  mnist_stats_panel(BG_IMG_BUF_B, (u32)(VMODE_WIDTH*3u), STAT_PANEL_X, STAT_PANEL_Y, &st0); }
#endif

	XV_HdmiTxSS_MaskDisable(&HdmiTxSs);

    /*
     * Bring up the HDMI monitor
     */
    TxCableConnect = (TRUE);
	TxRestartHdmiTx = (TRUE);
	TxBusy = (FALSE);
	XVphy_IBufDsEnable(&Vphy, 0, XVPHY_DIR_TX, (TRUE));
	// Initialize EDID App during cable connect
	EDIDConnectInit(&EdidHdmi20_t);
	// Read the EDID and the SCDC
	EdidScdcCheck(&HdmiTxSs, &EdidHdmi20_t);

#ifdef MOTOR_PRESENT
	/* Motor runs CONCURRENTLY with the video loop: init the rails, then auto-start
	 * at 20% clockwise. motor_poll() inside the loop handles live UART commands, so
	 * HDMI + camera come up from boot with no blocking console. */
	motor_init();
	motor_start_default();
#ifdef DISC_STROBE_PRESENT
	/* Angle-triggered strobe core: program cpr/target/pulse, leave OFF until armed
	 * over UART ('S'). Reads the same encoder in fabric; drives MOTOR_XVS -> OV9281 FSIN. */
	disc_strobe_init();
#endif
#ifdef MBPRE_CROP_CTRL
	/* Live mbpre crop pan/zoom keys (z x i j k l o) routed through the motor console. */
	motor_set_aux_key_cb(mnist_crop_key);
#endif
#endif

	/* Main loop */
	do {
#ifdef MOTOR_PRESENT
		motor_poll();   /* non-blocking motor command handling; video stays live */
#if defined(DISC_STROBE_PRESENT)
		cam_watchdog();  /* re-init the OV9281 if FSIN frames stop while the disc turns */
#endif
#ifdef MBDPU_HAS_POS
		if (g_auto_tgt && !(disc_strobe_enabled() && g_trig_active)) {   /* S disarmed or F off */
			g_auto_tgt = 0; g_auto_sum = 0; g_auto_n = 0;
			xil_printf("auto-target: off (strobe disarmed or FSIN trigger off)\r\n");
		}
#endif
#endif
#ifdef STATS_ENABLE
		stats_service();   /* ~2 Hz: refresh the on-screen RPM / captures-per-second HUD */
#endif

#ifdef MBDPU01_BASE
		/* Per-capture inference (interrupt-driven bottom half). The v_frmbuf_wr DONE ISR
		 * (FrmbufWrCallback) raises a per-frame flag; we service it here in thread context
		 * so the ~60 ms preprocess+infer+overlay never runs in ISR context (that would
		 * starve the interrupt-driven HDMI/VPHY). Runs exactly once per completed capture —
		 * in FSIN-trigger mode that is one inference per disc-angle strobe; when the disc is
		 * stopped no frames land and this stays idle. Consumes the just-COMPLETED buffer
		 * (FrameDoneAddr), not the in-flight write slot, so no tearing. The on-screen
		 * confidence-bar overlay is the result readout (no UART spam). */
		UINTPTR cambuf;
		if (HdmiStreamUp && NumActiveCams > 0 &&
		    FrmbufTakeFrame(&ActiveCams[0].VideoPipePtr->Frmbuf, &cambuf)) {
			int32_t logits[10];
			/* fcapz timeline markers (no-ops unless the fcapz_mark GPIO exists) */
			u32 fma = FMA_TAKE | FMA_SLOT(fmark_slot(ActiveCams[0].VideoPipePtr->Frmbuf.FrmbufBufrBaseAddr,
			                                         ActiveCams[0].VideoPipePtr->Frmbuf.FrameSizeBytes, cambuf));
			FMARK_A(fma);
#ifdef FCAPZ_MARK_PRESENT
			{ static int once = 0;   /* must match fcapz_evtcap SLOT_BASE / SLOT_SIZE */
			  if (!once) { once = 1;
			    xil_printf("fcapz: frame slots base 0x%08lx size 0x%lx\r\n",
			               (unsigned long)ActiveCams[0].VideoPipePtr->Frmbuf.FrmbufBufrBaseAddr,
			               (unsigned long)ActiveCams[0].VideoPipePtr->Frmbuf.FrameSizeBytes); } }
#endif
			/* Mean brightness of the frame's centre row (green byte = luma), for the good-frame
			 * gate below. The frame is DMA-written, so invalidate the row before the CPU reads. */
			u32 mean;
			{
				u32 st = ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes;
				UINTPTR rowa = cambuf + (uintptr_t)(MNIST_FRAME_H/2) * st;
				Xil_DCacheInvalidateRange(rowa, MNIST_FRAME_W * 3u);
				const uint8_t *row = (const uint8_t *)rowa;
				u32 sum = 0; for (int i = 0; i < MNIST_FRAME_W; i += 10) sum += row[i*3 + 1];
				mean = sum / (MNIST_FRAME_W/10);
				if (mean < g_frm_min) g_frm_min = mean;   /* TEST probe */
				if (mean > g_frm_max) g_frm_max = mean;
			}
			/* Good-frame gate: in FSIN-trigger mode the OV9281 emits a dim wake-settling frame
			 * per pulse. (Where fsin_frame_tag exists the PL already drops it; this stays as a
			 * fallback.) Drop frames dimmer than half the last good one — skip display + ROI +
			 * inference so neither the camera layer nor the ROI flashes; the display holds the
			 * last good frame. In free-run (g_trig_active=0) nothing is gated. */
			int frame_dim = (g_trig_active && g_last_good_bright != 0u &&
			                 mean < (g_last_good_bright >> 1)) ? 1 : 0;
			if (frame_dim) FMARK_A(fma | FMA_DIM);
			if (!frame_dim) {
			g_last_good_bright = mean;
			if (g_trig_active && g_cam_frmbuf) {   /* worker drives display: show this good frame */
				g_cam_frmbuf->DisplayHeldAddr = cambuf;   /* tell the ISR to not overwrite this slot */
				g_cam_frmbuf->FrmbufRdBufrAddr = cambuf;
				XVFrmbufRd_SetBufferAddr(&g_cam_frmbuf->FrmbufRd, cambuf);
				fma |= FMA_HELD; FMARK_A(fma);
			}
#ifdef STATS_ENABLE
			uint32_t _lt0 = Xil_In32(RT_BASE + RT_TCR);   /* stage-latency stamp: start */
#endif
#if defined(MBDPU03_CORE)
			/* No separate preprocess: mbdpu03 crops/zooms the camera frame itself (PRE = 0). */
#elif defined(MBPRE_BASE)
			/* Preprocessing OFFLOADED to the fabric (mbpre): camera 640x480 -> 28x28 in
			 * ~2 ms vs ~17 ms on the soft CPU. Programmable centre crop from the live
			 * g_crop_* state (x0,y0,blk_shift). cam/img are DDR-resident; mbpre streams
			 * them over AXI (no CPU cache for cam). Register map = xmbpre_hw.h. */
			Xil_Out32(MBPRE_BASE + 0x10u, (u32)cambuf);                                         /* cam lo */
			Xil_Out32(MBPRE_BASE + 0x14u, 0u);                                                  /* cam hi */
			Xil_Out32(MBPRE_BASE + 0x1cu, ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes); /* stride */
			Xil_Out32(MBPRE_BASE + 0x24u, 0u);                    /* invert (model is inverted-trained) */
			Xil_Out32(MBPRE_BASE + 0x2cu, g_crop_x0);             /* x0 */
			Xil_Out32(MBPRE_BASE + 0x34u, g_crop_y0);             /* y0 */
			Xil_Out32(MBPRE_BASE + 0x3cu, g_crop_shift);          /* blk_shift */
			Xil_Out32(MBPRE_BASE + 0x44u, (u32)MBDPU01_IMG);      /* img lo */
			Xil_Out32(MBPRE_BASE + 0x48u, 0u);                    /* img hi */
			Xil_Out32(MBPRE_BASE + 0x00u, 1u);                    /* ap_start */
			while (!(Xil_In32(MBPRE_BASE + 0x00u) & 0x2u)) { }    /* poll ap_done */
			/* mbpre wrote img to DDR over AXI: invalidate so the CPU (overlay) and
			 * mbdpu01's input-flush see mbpre's fresh data, not stale cache. */
			Xil_DCacheInvalidateRange((UINTPTR)MBDPU01_IMG, MNIST_N);
#else
			mnist_preprocess_rgb8(cambuf,
			                      ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes,
			                      0 /*no invert (model trained on inverted dataset)*/, MBDPU01_IMG);
#endif
#ifdef STATS_ENABLE
			uint32_t _lt1 = Xil_In32(RT_BASE + RT_TCR);   /* end mbpre preprocess */
#endif
#if defined(MBDPU03_CORE)
			FMARK_A(fma | FMA_INFER);
			int pred = mbdpu03_infer(MBDPU01_BASE, MBDPU01_WBLOB, MBDPU01_PBLOB,
			                         MBDPU01_IMG, MBDPU01_OUT, (uint64_t)cambuf,
			                         ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes,
			                         g_crop_x0, g_crop_y0, g_crop_shift, logits, g_pos_x100);
			FRESULT(pred, g_pos_x100[0], g_pos_x100[1]);   /* fcapz timeline: digit, dx, dy */
			FMARK_A(fma);
#elif defined(MBDPU_HAS_POS)
			int pred = mbdpu02_infer(MBDPU01_BASE, MBDPU01_WBLOB, MBDPU01_PBLOB,
			                         MBDPU01_IMG, MBDPU01_OUT, logits, g_pos_x100);
#endif
#ifdef MBDPU_HAS_POS
			/* Auto-target: average dx over one revolution (N captures) so each printed digit's
			 * own static offset averages out, then step target toward centring (1 count per
			 * 0.5 px of mean dx, capped). Only while the strobe drives the camera. */
			if (g_auto_tgt && g_trig_active && disc_strobe_enabled()) {
				g_auto_sum += g_pos_x100[0];
				if (++g_auto_n >= disc_strobe_captures()) {
					int32_t mean_dx = g_auto_sum / (int32_t)g_auto_n;
					g_auto_sum = 0; g_auto_n = 0;
					if (mean_dx > AUTO_DEADBAND_X100 || mean_dx < -AUTO_DEADBAND_X100) {
						int step = (int)(mean_dx / POS_UNIT_X100) * DX_TARGET_SIGN;
						if (step >  AUTO_STEP_MAX) step =  AUTO_STEP_MAX;
						if (step < -AUTO_STEP_MAX) step = -AUTO_STEP_MAX;
						/* Wrap within one strobe period (0..target_max): the fire points are
						 * target + k*cpr/N, so stepping past either end continues on the
						 * neighbouring fire point instead of sticking at the limit. */
						int span = (int)disc_strobe_target_max() + 1;
						int t0 = (int)disc_strobe_target();
						int t = (t0 + step) % span;
						if (t < 0) t += span;
						disc_strobe_set_target((uint32_t)t);
						g_tchg_rev = disc_strobe_revs(); g_tchg_from = (uint32_t)t0;
						g_tchg_to = (uint32_t)t; g_tchg_valid = 1;
						g_tchg_wrap = (t0 + step < 0 || t0 + step >= span);
						xil_printf("auto: mean dx=%c%d.%02d -> target %d->%d%s (rev %lu pos %lu)\r\n",
						           mean_dx < 0 ? '-' : '+', (int)(mean_dx < 0 ? -mean_dx : mean_dx) / 100,
						           (int)(mean_dx < 0 ? -mean_dx : mean_dx) % 100, t0, t,
						           g_tchg_wrap ? " WRAP" : "", (unsigned long)g_tchg_rev,
						           (unsigned long)disc_strobe_position());
					}
				}
			}
#else
			int pred = mbdpu01_infer(MBDPU01_BASE, MBDPU01_WBLOB, MBDPU01_PBLOB,
			                         MBDPU01_IMG, MBDPU01_OUT, logits);
#endif
			(void)pred;
#ifdef STATS_ENABLE
			uint32_t _lt2 = Xil_In32(RT_BASE + RT_TCR);   /* end mbdpu01 inference */
#endif
#if defined(XPAR_BG_FRMBUF_RD_BASEADDR)
			/* On-screen feedback: ROI preview + live per-digit confidence bars. Draw into
			 * the BACK buffer, then pageflip bg_frmbuf_rd to it so the scanout only ever
			 * sees a complete panel (no mid-draw tearing). Flip lands at the next frame
			 * boundary (auto-restart), then we swap front/back for the next capture. */
			FMARK_A(fma | FMA_OVERLAY);
			mnist_draw_live(g_bg_back, (u32)(VMODE_WIDTH*3u),
			                (const int8_t *)(uintptr_t)MBDPU01_IMG, logits);
#ifdef MBDPU_HAS_POS
			mnist_draw_pos(g_bg_back, (u32)(VMODE_WIDTH*3u), (int)g_pos_x100[0], (int)g_pos_x100[1]);
#endif
			XVFrmbufRd_SetBufferAddr(&BgFrmbufRd, g_bg_back);
			{ uintptr_t t = g_bg_front; g_bg_front = g_bg_back; g_bg_back = t; }
			FMARK_A(fma);
#endif
#ifdef STATS_ENABLE
			/* Stage latencies -> ms x10 (10000 ticks @100 MHz = 0.1 ms). The stats panel
			 * (drawn ~2 Hz by stats_service) reads these globals. */
			{ uint32_t _lt3 = Xil_In32(RT_BASE + RT_TCR);   /* end overlay */
			  uint32_t per = RT_FREQ_HZ / 10000u;
			  g_stat_pre_x10 = (int)((_lt1 - _lt0) / per);
			  g_stat_dpu_x10 = (int)((_lt2 - _lt1) / per);
			  g_stat_ovl_x10 = (int)((_lt3 - _lt2) / per); }
#endif
#ifdef STATS_ENABLE
			/* Record training data BEFORE the ROI box is drawn into cambuf, so the teal
			 * outline never contaminates the captured crop. */
			if (g_recording)
				roi_record(cambuf, ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes,
				           (uintptr_t)MBDPU01_IMG, pred, logits, mean);
#endif
#if defined(MBPRE_BASE) || defined(MBDPU03_CORE)
			/* ROI box on the live feed: outline the exact crop mbpre used (tracks pan/zoom).
			 * Drawn into the just-displayed camera buffer AFTER mbpre consumed it, so the
			 * classified pixels are untouched; the frame is fresh each capture -> no erase. */
#ifdef STATS_ENABLE
			if (g_hud_on)
#endif
			{
			FMARK_A(fma | FMA_ROIBOX);
			mnist_draw_roi_box(cambuf, ActiveCams[0].VideoPipePtr->Frmbuf.FrameStrideBytes,
			                   (int)g_crop_x0, (int)g_crop_y0, (int)(CROP_DIM << g_crop_shift),
			                   0x22E6E6u /*teal, matches the HUD/overlay*/, 4);
			FMARK_A(fma);
			}
#endif
#ifdef MNIST_VERBOSE
			xil_printf("MNIST: %d (logit=%d)\r\n", pred, (int)logits[pred]);
#endif
			}   /* end good-frame gate */
			FMARK_A(FMA_SLOT(3));   /* capture done: take=0, slot=none */
		}
#endif

		SinkReady = SinkReadyCheck(&HdmiTxSs, &EdidHdmi20_t);

        if (TxRestartHdmiTx && SinkReady) {
			/* Clear TxRestartHdmiTx Flag */
			TxRestartHdmiTx = (FALSE);
			HdmiTxSsVidStreamPtr =
					XV_HdmiTxSs_GetVideoStream(&HdmiTxSs);
#ifdef HDMI_USE_HDMIPHY1
			/* The HDMI 2.1 SetStream takes a VideoTiming (not a VideoMode
			 * enum) and leaves the stream's VmId = 0, so re-deriving the
			 * mode from HdmiTxSsVidStreamPtr->VmId yields a garbage 720x240
			 * timing -> wrong TMDS/refclk -> monitor "No Input". This design
			 * outputs a fixed 1080p60 pattern, so pass the mode explicitly. */
			EnableHdmiTx(&Vphy,
					&HdmiTxSs,
					XVIDC_VM_1920x1080_60_P,
					XVIDC_CSF_RGB,
					XVIDC_BPC_8);
#else
			EnableHdmiTx(&Vphy,
					&HdmiTxSs,
					HdmiTxSsVidStreamPtr->VmId,
					HdmiTxSsVidStreamPtr->ColorFormatId,
					HdmiTxSsVidStreamPtr->ColorDepth);
#endif
		}

		if (IsStreamUp && SinkReady) {
			IsStreamUp = FALSE;

#ifdef HDMI_USE_HDMIPHY1
			/* Reconfigure the TMDS1204 redriver for the new line
			 * rate. TMDS mode (IsFRL=0); Lanes is ignored for TMDS. */
			TI_TMDS1204_LineRateReconfig(&HdmiCtlIic,
				HDMI_TX_REDRIVER_ADDR, TxRedriverRev,
				0 /*IsFRL*/, TxLineRate, 3 /*Lanes*/, 1 /*IsTx*/);
#else
			i2c_dp159(&Vphy, 0, TxLineRate);
#endif
			XVphy_Clkout1OBufTdsEnable
				(&Vphy, XVPHY_DIR_TX, (TRUE));
		}

		/* VPHY error */
		VphyProcessError();

	}
	while (1);

	return 0;
}
