/*
 * Copyright 2025 mariobergeron.com
 *
 * IMX477 (Raspberry Pi HQ Camera / Arducam B0242) driver, SCU200 port.
 *
 * Fixed 2-lane, 1920x1080 RAW configuration:
 *   - imx477_common   : from RPi kernel imx477.c (housekeeping, 24 MHz INCK,
 *                       2-lane CSI at {0x0114,0x01})
 *   - imx477_mode_1080p : "1080p cropped" 2x2-binned mode (2028x1080)
 *   - imx477_crop_1920  : narrow the analog window 2028 -> 1920 wide
 *   - imx477_tune       : frame length (~30fps), exposure, gains
 * See imx477.h for provenance.
 *
 * The camera runs ~30fps; the frame buffer decouples this from the HDMI 60Hz
 * display, so an exact 60fps sensor rate is not required.
 */
#include "sleep.h"
#include "imx477.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "i2c.h"

/* First-light aid: enable the sensor's internal colour-bar test pattern. It
 * validates the whole digital path (CSI-2 RX -> demosaic -> frame buffer ->
 * HDMI) independently of lens focus, exposure and lighting. Set to 0 for a
 * live image once the pipe is proven. */
#ifndef IMX477_USE_TEST_PATTERN
#define IMX477_USE_TEST_PATTERN 0
#endif

/* --- Common housekeeping registers (all modes) --------------------------- */
static const imx477_config_word_t imx477_common[] =
{
    {0x0136, 0x18},
    {0x0137, 0x00},
    {0x0138, 0x01},
    {0xe000, 0x00},
    {0xe07a, 0x01},
    {0x0808, 0x02},
    {0x4ae9, 0x18},
    {0x4aea, 0x08},
    {0xf61c, 0x04},
    {0xf61e, 0x04},
    {0x4ae9, 0x21},
    {0x4aea, 0x80},
    {0x38a8, 0x1f},
    {0x38a9, 0xff},
    {0x38aa, 0x1f},
    {0x38ab, 0xff},
    {0x55d4, 0x00},
    {0x55d5, 0x00},
    {0x55d6, 0x07},
    {0x55d7, 0xff},
    {0x55e8, 0x07},
    {0x55e9, 0xff},
    {0x55ea, 0x00},
    {0x55eb, 0x00},
    {0x574c, 0x07},
    {0x574d, 0xff},
    {0x574e, 0x00},
    {0x574f, 0x00},
    {0x5754, 0x00},
    {0x5755, 0x00},
    {0x5756, 0x07},
    {0x5757, 0xff},
    {0x5973, 0x04},
    {0x5974, 0x01},
    {0x5d13, 0xc3},
    {0x5d14, 0x58},
    {0x5d15, 0xa3},
    {0x5d16, 0x1d},
    {0x5d17, 0x65},
    {0x5d18, 0x8c},
    {0x5d1a, 0x06},
    {0x5d1b, 0xa9},
    {0x5d1c, 0x45},
    {0x5d1d, 0x3a},
    {0x5d1e, 0xab},
    {0x5d1f, 0x15},
    {0x5d21, 0x0e},
    {0x5d22, 0x52},
    {0x5d23, 0xaa},
    {0x5d24, 0x7d},
    {0x5d25, 0x57},
    {0x5d26, 0xa8},
    {0x5d37, 0x5a},
    {0x5d38, 0x5a},
    {0x5d77, 0x7f},
    {0x7b75, 0x0e},
    {0x7b76, 0x0b},
    {0x7b77, 0x08},
    {0x7b78, 0x0a},
    {0x7b79, 0x47},
    {0x7b7c, 0x00},
    {0x7b7d, 0x00},
    {0x8d1f, 0x00},
    {0x8d27, 0x00},
    {0x9004, 0x03},
    {0x9200, 0x50},
    {0x9201, 0x6c},
    {0x9202, 0x71},
    {0x9203, 0x00},
    {0x9204, 0x71},
    {0x9205, 0x01},
    {0x9371, 0x6a},
    {0x9373, 0x6a},
    {0x9375, 0x64},
    {0x991a, 0x00},
    {0x996b, 0x8c},
    {0x996c, 0x64},
    {0x996d, 0x50},
    {0x9a4c, 0x0d},
    {0x9a4d, 0x0d},
    {0xa001, 0x0a},
    {0xa003, 0x0a},
    {0xa005, 0x0a},
    {0xa006, 0x01},
    {0xa007, 0xc0},
    {0xa009, 0xc0},
    {0x3d8a, 0x01},
    {0x4421, 0x04},
    {0x7b3b, 0x01},
    {0x7b4c, 0x00},
    {0x9905, 0x00},
    {0x9907, 0x00},
    {0x9909, 0x00},
    {0x990b, 0x00},
    {0x9944, 0x3c},
    {0x9947, 0x3c},
    {0x994a, 0x8c},
    {0x994b, 0x50},
    {0x994c, 0x1b},
    {0x994d, 0x8c},
    {0x994e, 0x50},
    {0x994f, 0x1b},
    {0x9950, 0x8c},
    {0x9951, 0x1b},
    {0x9952, 0x0a},
    {0x9953, 0x8c},
    {0x9954, 0x1b},
    {0x9955, 0x0a},
    {0x9a13, 0x04},
    {0x9a14, 0x04},
    {0x9a19, 0x00},
    {0x9a1c, 0x04},
    {0x9a1d, 0x04},
    {0x9a26, 0x05},
    {0x9a27, 0x05},
    {0x9a2c, 0x01},
    {0x9a2d, 0x03},
    {0x9a2f, 0x05},
    {0x9a30, 0x05},
    {0x9a41, 0x00},
    {0x9a46, 0x00},
    {0x9a47, 0x00},
    {0x9c17, 0x35},
    {0x9c1d, 0x31},
    {0x9c29, 0x50},
    {0x9c3b, 0x2f},
    {0x9c41, 0x6b},
    {0x9c47, 0x2d},
    {0x9c4d, 0x40},
    {0x9c6b, 0x00},
    {0x9c71, 0xc8},
    {0x9c73, 0x32},
    {0x9c75, 0x04},
    {0x9c7d, 0x2d},
    {0x9c83, 0x40},
    {0x9c94, 0x3f},
    {0x9c95, 0x3f},
    {0x9c96, 0x3f},
    {0x9c97, 0x00},
    {0x9c98, 0x00},
    {0x9c99, 0x00},
    {0x9c9a, 0x3f},
    {0x9c9b, 0x3f},
    {0x9c9c, 0x3f},
    {0x9ca0, 0x0f},
    {0x9ca1, 0x0f},
    {0x9ca2, 0x0f},
    {0x9ca3, 0x00},
    {0x9ca4, 0x00},
    {0x9ca5, 0x00},
    {0x9ca6, 0x1e},
    {0x9ca7, 0x1e},
    {0x9ca8, 0x1e},
    {0x9ca9, 0x00},
    {0x9caa, 0x00},
    {0x9cab, 0x00},
    {0x9cac, 0x09},
    {0x9cad, 0x09},
    {0x9cae, 0x09},
    {0x9cbd, 0x50},
    {0x9cbf, 0x50},
    {0x9cc1, 0x50},
    {0x9cc3, 0x40},
    {0x9cc5, 0x40},
    {0x9cc7, 0x40},
    {0x9cc9, 0x0a},
    {0x9ccb, 0x0a},
    {0x9ccd, 0x0a},
    {0x9d17, 0x35},
    {0x9d1d, 0x31},
    {0x9d29, 0x50},
    {0x9d3b, 0x2f},
    {0x9d41, 0x6b},
    {0x9d47, 0x42},
    {0x9d4d, 0x5a},
    {0x9d6b, 0x00},
    {0x9d71, 0xc8},
    {0x9d73, 0x32},
    {0x9d75, 0x04},
    {0x9d7d, 0x42},
    {0x9d83, 0x5a},
    {0x9d94, 0x3f},
    {0x9d95, 0x3f},
    {0x9d96, 0x3f},
    {0x9d97, 0x00},
    {0x9d98, 0x00},
    {0x9d99, 0x00},
    {0x9d9a, 0x3f},
    {0x9d9b, 0x3f},
    {0x9d9c, 0x3f},
    {0x9d9d, 0x1f},
    {0x9d9e, 0x1f},
    {0x9d9f, 0x1f},
    {0x9da0, 0x0f},
    {0x9da1, 0x0f},
    {0x9da2, 0x0f},
    {0x9da3, 0x00},
    {0x9da4, 0x00},
    {0x9da5, 0x00},
    {0x9da6, 0x1e},
    {0x9da7, 0x1e},
    {0x9da8, 0x1e},
    {0x9da9, 0x00},
    {0x9daa, 0x00},
    {0x9dab, 0x00},
    {0x9dac, 0x09},
    {0x9dad, 0x09},
    {0x9dae, 0x09},
    {0x9dc9, 0x0a},
    {0x9dcb, 0x0a},
    {0x9dcd, 0x0a},
    {0x9e17, 0x35},
    {0x9e1d, 0x31},
    {0x9e29, 0x50},
    {0x9e3b, 0x2f},
    {0x9e41, 0x6b},
    {0x9e47, 0x2d},
    {0x9e4d, 0x40},
    {0x9e6b, 0x00},
    {0x9e71, 0xc8},
    {0x9e73, 0x32},
    {0x9e75, 0x04},
    {0x9e94, 0x0f},
    {0x9e95, 0x0f},
    {0x9e96, 0x0f},
    {0x9e97, 0x00},
    {0x9e98, 0x00},
    {0x9e99, 0x00},
    {0x9ea0, 0x0f},
    {0x9ea1, 0x0f},
    {0x9ea2, 0x0f},
    {0x9ea3, 0x00},
    {0x9ea4, 0x00},
    {0x9ea5, 0x00},
    {0x9ea6, 0x3f},
    {0x9ea7, 0x3f},
    {0x9ea8, 0x3f},
    {0x9ea9, 0x00},
    {0x9eaa, 0x00},
    {0x9eab, 0x00},
    {0x9eac, 0x09},
    {0x9ead, 0x09},
    {0x9eae, 0x09},
    {0x9ec9, 0x0a},
    {0x9ecb, 0x0a},
    {0x9ecd, 0x0a},
    {0x9f17, 0x35},
    {0x9f1d, 0x31},
    {0x9f29, 0x50},
    {0x9f3b, 0x2f},
    {0x9f41, 0x6b},
    {0x9f47, 0x42},
    {0x9f4d, 0x5a},
    {0x9f6b, 0x00},
    {0x9f71, 0xc8},
    {0x9f73, 0x32},
    {0x9f75, 0x04},
    {0x9f94, 0x0f},
    {0x9f95, 0x0f},
    {0x9f96, 0x0f},
    {0x9f97, 0x00},
    {0x9f98, 0x00},
    {0x9f99, 0x00},
    {0x9f9a, 0x2f},
    {0x9f9b, 0x2f},
    {0x9f9c, 0x2f},
    {0x9f9d, 0x00},
    {0x9f9e, 0x00},
    {0x9f9f, 0x00},
    {0x9fa0, 0x0f},
    {0x9fa1, 0x0f},
    {0x9fa2, 0x0f},
    {0x9fa3, 0x00},
    {0x9fa4, 0x00},
    {0x9fa5, 0x00},
    {0x9fa6, 0x1e},
    {0x9fa7, 0x1e},
    {0x9fa8, 0x1e},
    {0x9fa9, 0x00},
    {0x9faa, 0x00},
    {0x9fab, 0x00},
    {0x9fac, 0x09},
    {0x9fad, 0x09},
    {0x9fae, 0x09},
    {0x9fc9, 0x0a},
    {0x9fcb, 0x0a},
    {0x9fcd, 0x0a},
    {0xa14b, 0xff},
    {0xa151, 0x0c},
    {0xa153, 0x50},
    {0xa155, 0x02},
    {0xa157, 0x00},
    {0xa1ad, 0xff},
    {0xa1b3, 0x0c},
    {0xa1b5, 0x50},
    {0xa1b9, 0x00},
    {0xa24b, 0xff},
    {0xa257, 0x00},
    {0xa2ad, 0xff},
    {0xa2b9, 0x00},
    {0xb21f, 0x04},
    {0xb35c, 0x00},
    {0xb35e, 0x08},
    {0x0112, 0x0c},
    {0x0113, 0x0c},
    {0x0114, 0x01},
    {0x0350, 0x00},
    {0xbcf1, 0x02},
    {0x3ff9, 0x01},
};

/* --- 2028x1080 2x2-binned "1080p cropped" mode --------------------------- */
static const imx477_config_word_t imx477_mode_1080p[] =
{
    {0x0342, 0x31},
    {0x0343, 0xc4},
    {0x0344, 0x00},
    {0x0345, 0x00},
    {0x0346, 0x01},
    {0x0347, 0xb8},
    {0x0348, 0x0f},
    {0x0349, 0xd7},
    {0x034a, 0x0a},
    {0x034b, 0x27},
    {0x0220, 0x00},
    {0x0221, 0x11},
    {0x0381, 0x01},
    {0x0383, 0x01},
    {0x0385, 0x01},
    {0x0387, 0x01},
    {0x0900, 0x01},
    {0x0901, 0x22},
    {0x0902, 0x02},
    {0x3140, 0x02},
    {0x3c00, 0x00},
    {0x3c01, 0x03},
    {0x3c02, 0xa2},
    {0x3f0d, 0x01},
    {0x5748, 0x07},
    {0x5749, 0xff},
    {0x574a, 0x00},
    {0x574b, 0x00},
    {0x7b53, 0x01},
    {0x9369, 0x73},
    {0x936b, 0x64},
    {0x936d, 0x5f},
    {0x9304, 0x00},
    {0x9305, 0x00},
    {0x9e9a, 0x2f},
    {0x9e9b, 0x2f},
    {0x9e9c, 0x2f},
    {0x9e9d, 0x00},
    {0x9e9e, 0x00},
    {0x9e9f, 0x00},
    {0xa2a9, 0x60},
    {0xa2b7, 0x00},
    {0x0401, 0x00},
    {0x0404, 0x00},
    {0x0405, 0x20},
    {0x0408, 0x00},
    {0x0409, 0x00},
    {0x040a, 0x00},
    {0x040b, 0x00},
    {0x040c, 0x0f},
    {0x040d, 0xd8},
    {0x040e, 0x04},
    {0x040f, 0x38},
    {0x034c, 0x07},
    {0x034d, 0xec},
    {0x034e, 0x04},
    {0x034f, 0x38},
    {0x0301, 0x05},
    {0x0303, 0x02},
    {0x0305, 0x04},
    {0x0306, 0x01},
    {0x0307, 0x5e},
    {0x0309, 0x0c},
    {0x030b, 0x02},
    {0x030d, 0x02},
    {0x0310, 0x01},
    {0x0820, 0x07},
    {0x0821, 0x08},
    {0x0822, 0x00},
    {0x0823, 0x00},
    {0x080a, 0x00},
    {0x080b, 0x7f},
    {0x080c, 0x00},
    {0x080d, 0x4f},
    {0x080e, 0x00},
    {0x080f, 0x77},
    {0x0810, 0x00},
    {0x0811, 0x5f},
    {0x0812, 0x00},
    {0x0813, 0x57},
    {0x0814, 0x00},
    {0x0815, 0x4f},
    {0x0816, 0x01},
    {0x0817, 0x27},
    {0x0818, 0x00},
    {0x0819, 0x3f},
    {0xe04c, 0x00},
    {0xe04d, 0x7f},
    {0xe04e, 0x00},
    {0xe04f, 0x1f},
    {0x3e20, 0x01},
    {0x3e37, 0x00},
    {0x3f50, 0x00},
    {0x3f56, 0x01},
    {0x3f57, 0x6c},
};

/* --- Window the horizontal output from 2028 down to 1920 ----------------- *
 * Analog crop X: start 108 (0x006C), end 3947 (0x0F6B) -> 3840 wide.
 * 3840 / 2 (binning) = 1920. 108 and 3840 are multiples of 4, so the 2x2 bin
 * grid and the RGGB Bayer phase are preserved. Digital crop width and output
 * size are narrowed to match. Y is left at 1080. */
/* Output a STANDARD 1920x1080 (so the vproc uses a real XVidC timing, no
 * CUSTOM-timing seam). Analog crop X: 108..3947 = 3840 wide -> 1920 after 2x2
 * bin. To keep each 1920-px RAW10 line inside the 2-lane 450 Mbps bandwidth
 * (drain ~21.3 us) the line period is stretched by widening LINE_LENGTH
 * (0x0342/0x0343) from the native 0x31C4=12740 to 0x5000=20480 (line period
 * ~24 us > drain -> no shear). This lowers the sensor frame rate to ~18 fps;
 * the frame buffer + HDMI still output 60 Hz. All offsets are multiples of 4
 * to preserve the RGGB Bayer phase. Height stays 1080. */
static const imx477_config_word_t imx477_crop_1920[] =
{
    /* 640x480 (4:3) crop, all < the ~704 capture wrap limit. Analog window
     * 1280x960 -> 640x480 after 2x2 bin. The physical region is 4:3, matching
     * the 4:3 output, so the vproc runs 1:1 in BOTH axes -> correct aspect
     * ratio (no horizontal stretch) and no scaling. All offsets multiples of
     * 4 to preserve the RGGB Bayer phase. Native LINE_LENGTH. */
    {0x0344, 0x05}, {0x0345, 0x6C},   /* X_ADD_STA       = 1388  */
    {0x0348, 0x0A}, {0x0349, 0x6B},   /* X_ADD_END       = 2667 (-> 640 wide) */
    {0x0346, 0x04}, {0x0347, 0x10},   /* Y_ADD_STA       = 1040  */
    {0x034A, 0x07}, {0x034B, 0xCF},   /* Y_ADD_END       = 1999 (-> 480 tall) */
    {0x040C, 0x05}, {0x040D, 0x00},   /* DIG_CROP_WIDTH  = 1280 (analog, ->640) */
    {0x040E, 0x01}, {0x040F, 0xE0},   /* DIG_CROP_HEIGHT = 480   */
    {0x034C, 0x02}, {0x034D, 0x80},   /* X_OUTPUT_SIZE   = 640   */
    {0x034E, 0x01}, {0x034F, 0xE0},   /* Y_OUTPUT_SIZE   = 480   */
};

/* --- Force RAW10 output ---------------------------------------------------*
 * The 2028x1080 mode is a RAW12 mode ({0x0112,0x0113}={0x0C,0x0C}), but the
 * SCU200 MIPI CSI-2 RX subsystem is built for RAW10 (to match the IMX219 the
 * reference design targets) and the demosaic is 8-bit. A RAW12 sensor into a
 * RAW10 receiver mis-frames every line, which shows as horizontal banding
 * instead of the expected vertical colour bars. Switch the sensor to RAW10:
 *   0x0112/0x0113 = CSI_DATA_FORMAT  -> 0x0A0A (10-bit)
 *   0x0309        = OPPXCK_DIV       -> 0x0A   (output pixel-clock divider
 *                                               must track the bit depth) */
static const imx477_config_word_t imx477_fmt_raw10[] =
{
    {0x0112, 0x0A}, {0x0113, 0x0A},   /* CSI_DATA_FORMAT = RAW10 */
    {0x0309, 0x0A},                   /* OPPXCK_DIV      = 10     */
};

/* --- 4-lane MIPI output --------------------------------------------------- *
 * The IMX477 (and the Arducam module) is a 4-lane sensor, and the SCU200 MIPI0
 * RX is now built for 4 lanes. Running 4-lane spreads the pixel data over twice
 * the lanes, so halving the output byte clock (OPSYCK_DIV 2 -> 4) keeps the
 * aggregate throughput the same while dropping the per-lane rate from ~900 to
 * ~450 Mbps/lane - close to the proven 420-Mbps design point and much kinder to
 * the D-PHY / board signal integrity.
 *   0x0114 = CSI_LANE_MODE : 0x03 = 4-lane (was 0x01 = 2-lane in common regs)
 *   0x030B = OPSYCK_DIV    : 0x04 (was 0x02) -> half byte clock */
static const imx477_config_word_t imx477_lanes4[] =
{
    {0x0114, 0x03},   /* 4-lane            */
    {0x030B, 0x04},   /* OPSYCK_DIV = 4    */
};

/* --- 2-lane RAW10 @ ~420 Mbps (the ONLY viable config on SCU200) ----------- *
 * 4-lane is dead board-wide: D3 lands on an unusable _N8 D-PHY bitslice on all
 * three connectors, so lane 3 never receives. Run 2 lanes (D0/D1, both good)
 * at the reference 420-Mbps rate. RAW10 to match the MIPI0 subset converter
 * (tdata[9:2]) and to fit 1080p in 2-lane bandwidth (2x420=840 > 622 Mbps).
 *   0x0114 = 2-lane
 *   0x0112/0x0113 = RAW10, 0x0309 OPPXCK_DIV = 10 (bit depth)
 *   PLL_VT_MPY 350->175 halves the single master PLL -> pixel + byte + frame
 *   rate all scale down together to ~375 Mbps/lane (~25 fps), inside the D-PHY
 *   window; fine-tune HS_SETTLE live via the reg interface if needed. */
static const imx477_config_word_t imx477_2lane_raw10[] =
{
    /* DIAGNOSTIC: minimal 2-lane, native RAW12, proven 450 Mbps byte clock,
     * halved pixel readout. Checking whether the sensor drives BOTH lanes
     * cleanly at 2-lane (DL0+DL1 active) before worrying about format. */
    {0x0114, 0x01},                   /* 2-lane                     */
    {0x0112, 0x0A}, {0x0113, 0x0A},   /* CSI_DATA_FORMAT = RAW10    */
    {0x0309, 0x0A},                   /* OPPXCK_DIV = 10 (RAW10)    */
    {0x030B, 0x04},                   /* OPSYCK_DIV = 4 -> ~450 Mbps/lane byte
                                       * clock (this rate gave CLEAN colour bars,
                                       * proving D-PHY/RX/pipe all work here).  */
    /* NOTE: VT (pixel-array) clock left NATIVE. Instead of slowing the readout
     * (which broke the array timing -> shear), the crop below narrows the line
     * to ~960 px so its MIPI drain time fits inside the native line period at
     * 2-lane 450 Mbps. Clean readout, at reduced horizontal resolution. */
};

/* --- Slow the MIPI link to match the RX D-PHY ---------------------------- *
 * The SCU200 CSI-2 RX D-PHY is built for the IMX219's line rate: 420 Mbps/lane
 * with HS_SETTLE=158 ns. That HS_SETTLE stays valid for ~82..769 Mbps/lane, but
 * the IMX477's native mode runs at ~900 Mbps/lane (LINK_FREQ 450 MHz x2), above
 * the window, so the D-PHY mis-samples and every packet fails ECC/CRC (RX Image
 * Info reads ByteCount=0, ISR shows Unsupported-Data-Type + CRC + ECC errors).
 * Halve the master PLL multiplier (PLL_VT_MPY 350 -> 175). In this mode
 * PLL_MULT_DRIV (0x0310)=1 drives the output clock from the single VT PLL, so
 * pixel clock, MIPI link rate and frame rate all scale down together (~450
 * Mbps/lane, ~15 fps) - the link/pixel ratio is preserved and the rate lands
 * inside the D-PHY window. (No bitstream rebuild needed; software only.) */
static const imx477_config_word_t imx477_slowpll[] =
{
    {0x0306, 0x00}, {0x0307, 0xAF},   /* PLL_VT_MPY = 175 (was 350) */
};

/* --- Frame timing, exposure and gain ------------------------------------- *
 * FRAME_LENGTH 2198 lines with line_length 0x31C4 and 840 MHz pixel rate is
 * ~30 fps. Exposure 0x0300 (768 lines) and analog gain 0x0200 (~2x) are a
 * moderate indoor starting point - all easily tuned. */
static const imx477_config_word_t imx477_tune[] =
{
    /* FRAME_LENGTH 1099 -> ~60.0 fps (840 MHz / (12740 * 1099)) to match the
     * 60 Hz HDMI output as closely as possible. Camera and display are NOT
     * genlocked, so a slow residual drift remains (occasional repeat/skip),
     * but matching the nominal rate removes the periodic ~2 fps beat that
     * showed as the "circling back in time" discontinuity. */
    {0x0340, 0x04}, {0x0341, 0x4B},   /* FRAME_LENGTH  = 1099   */
    /* Short exposure to cut motion blur (the moving-hand "ghost"): 600 lines
     * (~9 ms) instead of 1600 (~24 ms). Compensate the lost light with more
     * gain: analog ~8x (0x0380) + digital 2x (0x0200). */
    {0x0202, 0x02}, {0x0203, 0xEE},   /* EXPOSURE      = 750 (lowered for LED headroom) */
    {0x0204, 0x03}, {0x0205, 0x80},   /* ANALOG_GAIN   = 896 (~8x) */
    {0x020E, 0x02}, {0x020F, 0x80},   /* DIGITAL_GAIN  = 0x0280 (2.5x) */
};

static int imx477_write_table(uint8_t iic_id,
                              const imx477_config_word_t *tbl, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int Status = imx477_write(iic_id, tbl[i].addr, tbl[i].data);
        if (Status != XST_SUCCESS) {
            xil_printf("  imx477: I2C write failed at reg 0x%04x\r\n",
                       tbl[i].addr);
            return Status;
        }
    }
    return XST_SUCCESS;
}

#define TBL(t) (t), (sizeof(t)/sizeof((t)[0]))

/*
 * Look for the IMX477 on the I2C bus by reading its 16-bit chip ID.
 */
int imx477_detect(uint8_t iic_id)
{
    int Status;
    uint8_t hi = 0, lo = 0;

    Status = imx477_read(iic_id, IMX477_REG_CHIP_ID_HI, &hi);
    if (Status != XST_SUCCESS) {
        xil_printf("  imx477_detect: I2C read FAILED at addr 0x%02x "
                   "(no ACK)\r\n", IMX477_I2C_SLAVE_ADDR);
        return XST_FAILURE;
    }
    (void)imx477_read(iic_id, IMX477_REG_CHIP_ID_LO, &lo);

    uint16_t id = ((uint16_t)hi << 8) | lo;
    if (id != IMX477_CHIP_ID) {
        xil_printf("  imx477_detect: I2C OK but chip ID=0x%04x "
                   "(expected 0x%04x)\r\n", id, IMX477_CHIP_ID);
        return XST_FAILURE;
    }
    xil_printf("  imx477_detect: IMX477 FOUND (chip ID=0x0477)\r\n");
    return XST_SUCCESS;
}

int imx477_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask)
{
    int Status;

    imx477_reset(gpio, gpio_mask);

    /* Standby before reconfiguring */
    Status = imx477_write(iic_id, 0x0100, 0x00);
    if (Status != XST_SUCCESS) return Status;
    usleep(10000);

    /* Native RAW12 @ ~900 Mbps/lane. The MIPI0 RX (and only MIPI0) is built to
     * match this in the bitstream, so no software format/PLL slow-down is
     * applied here - the sensor runs its native mode. */
    if ((Status = imx477_write_table(iic_id, TBL(imx477_common)))     != XST_SUCCESS) return Status;
    if ((Status = imx477_write_table(iic_id, TBL(imx477_mode_1080p))) != XST_SUCCESS) return Status;
    if ((Status = imx477_write_table(iic_id, TBL(imx477_2lane_raw10))) != XST_SUCCESS) return Status;
    if ((Status = imx477_write_table(iic_id, TBL(imx477_crop_1920)))  != XST_SUCCESS) return Status;
    if ((Status = imx477_write_table(iic_id, TBL(imx477_tune)))       != XST_SUCCESS) return Status;

#if IMX477_USE_TEST_PATTERN
    /* 0x0600 = 2 : colour bars */
    imx477_write(iic_id, 0x0600, 0x00);
    imx477_write(iic_id, 0x0601, 0x02);
    xil_printf("  imx477: internal COLOUR-BAR test pattern ENABLED\r\n");
#else
    imx477_write(iic_id, 0x0600, 0x00);
    imx477_write(iic_id, 0x0601, 0x00);
#endif

    /* DEBUG: read back the key mode registers to confirm the writes landed. */
    {
        u8 v114=0xEE, v112=0xEE, v113=0xEE, v309=0xEE, v30b=0xEE, v301=0xEE;
        imx477_read(iic_id, 0x0114, &v114);
        imx477_read(iic_id, 0x0112, &v112);
        imx477_read(iic_id, 0x0113, &v113);
        imx477_read(iic_id, 0x0309, &v309);
        imx477_read(iic_id, 0x030B, &v30b);
        imx477_read(iic_id, 0x0301, &v301);
        xil_printf("  imx477 readback: LANE(0x0114)=0x%02x FMT(0x0112/13)=0x%02x/0x%02x "
                   "OPPXCK(0x0309)=0x%02x OPSYCK(0x030B)=0x%02x VTPXCK(0x0301)=0x%02x\r\n",
                   v114, v112, v113, v309, v30b, v301);
    }

    /* Stream on */
    usleep(10000);
    Status = imx477_write(iic_id, 0x0100, 0x01);
    if (Status != XST_SUCCESS) return Status;
    usleep(20000);

    return XST_SUCCESS;
}

/* Reset the IMX477 by toggling the power-enable GPIO (same scheme as IMX219) */
int imx477_reset(XGpio *gpio, uint32_t gpio_mask)
{
    XGpio_DiscreteClear(gpio, 1, gpio_mask);
    usleep(100000);
    XGpio_DiscreteSet(gpio, 1, gpio_mask);
    usleep(100000);
    return XST_SUCCESS;
}

/* Uses the POLLED I2C path (XIic_Send/Recv): the SCU200 camera-bus interrupt is
 * unreliable, so the interrupt-driven IicWrite/IicRead can miss a responding
 * sensor. See i2c.c IicPolledSend/Recv for the rationale. */
int imx477_write(uint8_t iic_id, uint16_t addr, uint8_t data)
{
    uint8_t buf[3];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    buf[2] = data;
    return IicPolledSend(iic_id, IMX477_I2C_SLAVE_ADDR, buf, 3, XIIC_STOP);
}

int imx477_read(uint8_t iic_id, uint16_t addr, uint8_t *data)
{
    int Status;
    uint8_t buf[2];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    /* Repeated-start: write the 16-bit register pointer, then read the byte. */
    Status = IicPolledSend(iic_id, IMX477_I2C_SLAVE_ADDR, buf, 2,
                           XIIC_REPEATED_START);
    if (Status != XST_SUCCESS) return Status;
    Status = IicPolledRecv(iic_id, IMX477_I2C_SLAVE_ADDR, data, 1, XIIC_STOP);
    return Status;
}
