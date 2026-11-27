/*
 * ov9281.c — OmniVision OV9281 (Arducam B0165) driver, SCU200 port. See ov9281.h.
 * DETECT-first: confirm the raw sensor + I2C before investing in the mode init.
 * Polled I2C path (SCU200 camera-bus interrupt is unreliable — see imx477.c).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sleep.h"
#include "ov9281.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "i2c.h"

/* First-light aid: OV9281 internal test pattern validates CSI-2 RX -> subset_conv ->
 * frame buffer -> HDMI independent of lens/exposure/light. Set 0 for a live image. */
#ifndef OV9281_USE_TEST_PATTERN
#define OV9281_USE_TEST_PATTERN 0
#endif

typedef struct { uint16_t addr; uint8_t data; } ov9281_reg_t;

/* 2-lane RAW10 base, from Arducam's OV9281_MIPI_2Lane_RAW10_10b_1280x800_40fps.cfg,
 * with the readout WINDOWED to a 640x480 centre crop (<= the ~704px capture-wrap limit
 * and matching the mono pipeline). Link rate ~400 Mbps/lane fits the 420 Mbps D-PHY
 * window, so the PLL (0x0302/0x030d/0x030e) is left at native. 0x3662=0x05 = 2-lane RAW10. */
static const ov9281_reg_t ov9281_mode_640x480[] = {
    {0x0302,0x32},{0x030d,0x50},{0x030e,0x02},
    {0x3001,0x00},{0x3004,0x00},{0x3005,0x00},{0x3006,0x04},
    {0x3011,0x0a},{0x3013,0x18},{0x3022,0x01},{0x3030,0x10},{0x3039,0x32},{0x303a,0x00},
    {0x3500,0x00},{0x3501,0x2a},{0x3502,0x90},{0x3503,0x08},{0x3505,0x8c},
    {0x3507,0x03},{0x3508,0x00},{0x3509,0x10},
    {0x3610,0x80},{0x3611,0xa0},{0x3620,0x6f},{0x3632,0x56},{0x3633,0x78},
    {0x3662,0x05},{0x3666,0x00},{0x366f,0x5a},{0x3680,0x84},
    {0x3712,0x80},{0x372d,0x22},{0x3731,0x80},{0x3732,0x30},
    {0x3778,0x00},{0x377d,0x22},{0x3788,0x02},{0x3789,0xa4},{0x378a,0x00},{0x378b,0x4a},{0x3799,0x20},
    /* --- windowing: 640x480 centre crop of the 1280x800 array (8px ISP offset) --- */
    {0x3800,0x01},{0x3801,0x40},   /* X_START = 320  */
    {0x3802,0x00},{0x3803,0xa0},   /* Y_START = 160  */
    {0x3804,0x03},{0x3805,0xcf},   /* X_END   = 975  (readout 656 = 640 + 2*8) */
    {0x3806,0x02},{0x3807,0x8f},   /* Y_END   = 655  (readout 496 = 480 + 2*8) */
    {0x3808,0x02},{0x3809,0x80},   /* X_OUTPUT = 640 */
    {0x380a,0x01},{0x380b,0xe0},   /* Y_OUTPUT = 480 */
    {0x380c,0x05},{0x380d,0xb0},   /* HTS = 1456     */
    {0x380e,0x05},{0x380f,0x5d},   /* VTS = 1373     */
    {0x3810,0x00},{0x3811,0x08},   /* ISP X offset 8 */
    {0x3812,0x00},{0x3813,0x08},   /* ISP Y offset 8 */
    {0x3814,0x11},{0x3815,0x11},   /* no subsample   */
    {0x3820,0x44},{0x3821,0x04},   /* 0x3821 bit2 = horizontal mirror ON (un-mirror the image) */
    {0x3881,0x42},{0x38b1,0x00},{0x3920,0xff},
    {0x4003,0x40},{0x4008,0x04},{0x4009,0x0b},{0x400c,0x00},{0x400d,0x07},{0x4010,0x40},{0x4043,0x40},
    {0x4307,0x30},{0x4317,0x00},{0x4501,0x00},{0x4507,0x00},{0x4509,0x00},{0x450a,0x08},
    {0x4601,0x04},{0x470f,0x00},{0x4f07,0x00},{0x4800,0x00},
    {0x5000,0x9f},{0x5001,0x00},{0x5e00,0x00},{0x5d00,0x07},{0x5d01,0x00},
};

/* --- External-trigger (FSIN) mode -------------------------------------------------
 * Default: V1, the datasheet's external-trigger snapshot mode (section 4.8.3): 0x4F00=1 low
 * power, 0x3030 b[2] r_fsin_wake_up_en, 0x303F=1 frame per FSIN, 0x0100=0 standby, and
 * 0x3823=0x00. A FSIN rising edge wakes the sensor, integrates and reads out exactly ONE
 * frame, SOF 1.86 ms after FSIN (fcapz, 2026-11-26), then it sleeps.
 * Arducam's ov9281_external_trigger.c (and our old default, V0) also sets 0x3823=0x30
 * (ext_vs_en + r_init_man). r_init_man (b[4]) is what produces the dim "wake" frame at
 * 1.06 ms, with the good frame one VTS later (10.96 ms at VTS 544); V2 (0x3823=0x10) showed
 * the same two frames. The earlier pure-snapshot test failed only because it used 0x303F=0. */
/* Trigger variants (K key), measured with fcapz at 77 RPM, N=8, VTS 544, PL drop off:
 *   V0 snapshot + ext_vs (0x3823=0x30): the old default (Arducam's config). Two frames per
 *      FSIN: dim at 1.06 ms (brightness ~21), good one VTS later (10.96 ms).
 *   V1 pure snapshot (0x3823=0x00), the DEFAULT: one frame per FSIN, SOF 1.86 ms after FSIN.
 *   V2 snapshot + r_init_man (0x3823=0x10): two frames, like V0 -> r_init_man causes the dim one.
 *   V3 ext_vs only, streaming, VTS 0xFFFF: free-runs, not re-aligned by FSIN (4% of strobes).
 * Each variant is applied on top of ov9281_trigger_off so the variants don't leak. */
#define OV9281_TRIG_VARIANTS 4
static const char *const k_trig_var_name[OV9281_TRIG_VARIANTS] = {
    "V0 snapshot+ext_vs", "V1 pure snapshot", "V2 snapshot+r_init_man", "V3 ext_vs only, VTS max" };
static unsigned s_trig_var = 1;   /* default: V1 pure snapshot (one frame per FSIN) */
static const ov9281_reg_t ov9281_trigger_on[] = {   /* known-working: snapshot wake + ext_vs */
    {0x3006,0x00},
    {0x4f00,0x01},{0x303f,0x01},{0x302c,0x00},{0x302f,0x7f},   /* r_frame_on_num = 1 */
    {0x3030,0x04},                                            /* wake on (snapshot, gated) */
    {0x3823,0x30},                                            /* overridden per variant: V0 0x30, V1 0x00, V2 0x10 */
    {0x0100,0x00},                                            /* standby */
};
static const ov9281_reg_t ov9281_trigger_v3[] = {   /* ext_vs frame-sync, continuous streaming */
    {0x3006,0x00},{0x4f00,0x00},{0x3030,0x10},{0x3823,0x30},{0x0100,0x01},
};
/* Undo: disable frame-sync, restore the streaming clock reg, resume continuous readout. */
static const ov9281_reg_t ov9281_trigger_off[] = {
    {0x3823,0x00},{0x3030,0x10},{0x4f00,0x00},{0x0100,0x01},
};

/* OV9281: 16-bit register address, 8-bit register data. */
int ov9281_write(uint8_t iic_id, uint16_t addr, uint8_t data)
{
    uint8_t buf[3];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    buf[2] = data;
    return IicPolledSend(iic_id, OV9281_I2C_SLAVE_ADDR, buf, 3, XIIC_STOP);
}

int ov9281_read(uint8_t iic_id, uint16_t addr, uint8_t *data)
{
    int Status;
    uint8_t buf[2];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    Status = IicPolledSend(iic_id, OV9281_I2C_SLAVE_ADDR, buf, 2, XIIC_REPEATED_START);
    if (Status != XST_SUCCESS) return Status;
    return IicPolledRecv(iic_id, OV9281_I2C_SLAVE_ADDR, data, 1, XIIC_STOP);
}

/* Read the 16-bit chip ID (two 8-bit registers). 0x9281 = raw OV9281 present. */
int ov9281_detect(uint8_t iic_id)
{
    int Status;
    uint8_t hi = 0, lo = 0;

    Status = ov9281_read(iic_id, OV9281_REG_CHIP_ID_HI, &hi);
    if (Status != XST_SUCCESS) {
        xil_printf("  ov9281_detect: I2C read FAILED at addr 0x%02x (no ACK)\r\n",
                   OV9281_I2C_SLAVE_ADDR);
        return XST_FAILURE;
    }
    (void)ov9281_read(iic_id, OV9281_REG_CHIP_ID_LO, &lo);

    uint16_t id = ((uint16_t)hi << 8) | lo;
    if (id != OV9281_CHIP_ID) {
        xil_printf("  ov9281_detect: I2C OK but chip ID=0x%04x (expected 0x%04x)\r\n",
                   id, OV9281_CHIP_ID);
        return XST_FAILURE;
    }
    xil_printf("  ov9281_detect: OV9281 FOUND (raw sensor, chip ID=0x9281)\r\n");
    return XST_SUCCESS;
}

/* Configure the OV9281 for 640x480 2-lane RAW10 and start streaming. */
int ov9281_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask)
{
    int Status;

    ov9281_reset(gpio, gpio_mask);

    /* Software reset, then let it settle. */
    (void)ov9281_write(iic_id, 0x0103, 0x01);
    usleep(10000);
    (void)ov9281_write(iic_id, 0x0100, 0x00);   /* ensure standby before configuring */

    for (size_t i = 0; i < sizeof(ov9281_mode_640x480)/sizeof(ov9281_mode_640x480[0]); i++) {
        Status = ov9281_write(iic_id, ov9281_mode_640x480[i].addr,
                                      ov9281_mode_640x480[i].data);
        if (Status != XST_SUCCESS) {
            xil_printf("  ov9281_config: I2C write FAILED at reg 0x%04x\r\n",
                       ov9281_mode_640x480[i].addr);
            return Status;
        }
    }

#if OV9281_USE_TEST_PATTERN
    ov9281_write(iic_id, 0x5e00, 0x80);   /* internal test pattern (walking/colour bars) */
    xil_printf("  ov9281_config: 640x480 2-lane RAW10, internal TEST PATTERN enabled\r\n");
#else
    xil_printf("  ov9281_config: 640x480 2-lane RAW10, live image\r\n");
#endif

    /* Read back the chip is still alive + a couple of key mode regs for sanity. */
    {
        uint8_t v3662 = 0, v0100 = 0;
        ov9281_read(iic_id, 0x3662, &v3662);
        usleep(1000);
        /* Stream on */
        Status = ov9281_write(iic_id, 0x0100, 0x01);
        usleep(5000);
        ov9281_read(iic_id, 0x0100, &v0100);
        xil_printf("  ov9281 readback: LANE_FMT(0x3662)=0x%02x STREAM(0x0100)=0x%02x\r\n",
                   v3662, v0100);
    }
    return Status;
}

/* Manual exposure in line units. Register value = lines<<4 (1/16-line units), split across
 * 0x3500[3:0]/0x3501[7:0]/0x3502[7:0]. See ov9281.h. */
int ov9281_set_exposure(uint8_t iic_id, uint32_t lines)
{
    uint32_t v = lines << 4;
    int s;
    IicReset(iic_id);   /* SCU200 camera I2C is flaky; clean the bus so the polled write can't hang */
    if ((s = ov9281_write(iic_id, 0x3500, (v >> 16) & 0x0F)) != XST_SUCCESS) return s;
    if ((s = ov9281_write(iic_id, 0x3501, (v >> 8)  & 0xFF)) != XST_SUCCESS) return s;
    if ((s = ov9281_write(iic_id, 0x3502,  v        & 0xFF)) != XST_SUCCESS) return s;
    return XST_SUCCESS;
}

/* Analog gain in 1/16x units (real-gain format). {0x3508[4:0],0x3509[7:0]} = gain<<4. */
int ov9281_set_gain(uint8_t iic_id, uint32_t gain_x16)
{
    int s;
    IicReset(iic_id);   /* SCU200 camera I2C is flaky; clean the bus so the polled write can't hang */
    if (gain_x16 < 16u)    gain_x16 = 16u;      /* 1.0x floor  */
    if (gain_x16 > 0x3FFu) gain_x16 = 0x3FFu;   /* ~63x ceiling */
    if ((s = ov9281_write(iic_id, 0x3508, (gain_x16 >> 8) & 0x1F)) != XST_SUCCESS) return s;
    if ((s = ov9281_write(iic_id, 0x3509,  gain_x16       & 0xFF)) != XST_SUCCESS) return s;
    return XST_SUCCESS;
}

/* Frame length in lines (0x380E/0x380F). See ov9281.h. */
int ov9281_set_vts(uint8_t iic_id, uint32_t vts)
{
    int s;
    IicReset(iic_id);   /* SCU200 camera I2C is flaky; clean the bus so the polled write can't hang */
    if ((s = ov9281_write(iic_id, 0x380e, (vts >> 8) & 0xFF)) != XST_SUCCESS) return s;
    if ((s = ov9281_write(iic_id, 0x380f,  vts       & 0xFF)) != XST_SUCCESS) return s;
    return XST_SUCCESS;
}

static int ov9281_write_table(uint8_t iic_id, const ov9281_reg_t *t, size_t n, uint8_t v3823)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t d = (t[i].addr == 0x3823 && t == ov9281_trigger_on) ? v3823 : t[i].data;
        int Status = ov9281_write(iic_id, t[i].addr, d);
        if (Status != XST_SUCCESS) {
            xil_printf("  ov9281 trigger: I2C write FAILED at reg 0x%04x\r\n", t[i].addr);
            return Status;
        }
        usleep(1000);
    }
    return XST_SUCCESS;
}

/* Switch FSIN external-trigger mode on/off (on = the selected variant). See ov9281.h. */
int ov9281_set_trigger_mode(uint8_t iic_id, int enable)
{
    int s;
    IicReset(iic_id);   /* SCU200 camera I2C is flaky; clean the bus so the polled writes can't hang */
    if (!enable)
        s = ov9281_write_table(iic_id, ov9281_trigger_off, sizeof(ov9281_trigger_off)/sizeof(ov9281_trigger_off[0]), 0);
    else if (s_trig_var == 3)
        s = ov9281_write_table(iic_id, ov9281_trigger_v3, sizeof(ov9281_trigger_v3)/sizeof(ov9281_trigger_v3[0]), 0);
    else
        s = ov9281_write_table(iic_id, ov9281_trigger_on, sizeof(ov9281_trigger_on)/sizeof(ov9281_trigger_on[0]),
                               s_trig_var == 1 ? 0x00 : s_trig_var == 2 ? 0x10 : 0x30);
    if (s != XST_SUCCESS) return s;
    uint8_t v3030 = 0, v303f = 0, v3823 = 0, v4f00 = 0, v0100 = 0;
    ov9281_read(iic_id, 0x3030, &v3030); ov9281_read(iic_id, 0x303f, &v303f);
    ov9281_read(iic_id, 0x3823, &v3823); ov9281_read(iic_id, 0x4f00, &v4f00);
    ov9281_read(iic_id, 0x0100, &v0100);
    xil_printf("  ov9281: FSIN trigger %s%s (0x3030=0x%02x 0x303F=0x%02x 0x3823=0x%02x 0x4F00=0x%02x stream=0x%02x)\r\n",
               enable ? "ON " : "OFF (free-run)", enable ? k_trig_var_name[s_trig_var] : "",
               v3030, v303f, v3823, v4f00, v0100);
    return XST_SUCCESS;
}

/* Experimental trigger variants (K key). Re-applies trigger mode with variant v. */
int ov9281_set_trigger_variant(uint8_t iic_id, unsigned v)
{
    s_trig_var = v % OV9281_TRIG_VARIANTS;
    int s = ov9281_set_trigger_mode(iic_id, 0);     /* back to the streaming baseline first */
    if (s != XST_SUCCESS) return s;
    return ov9281_set_trigger_mode(iic_id, 1);
}
unsigned    ov9281_trigger_variant(void)          { return s_trig_var; }
unsigned    ov9281_trigger_variants(void)         { return OV9281_TRIG_VARIANTS; }
const char *ov9281_trigger_variant_name(unsigned v){ return k_trig_var_name[v % OV9281_TRIG_VARIANTS]; }
uint32_t    ov9281_trigger_variant_vts(void)      { return s_trig_var == 3 ? 0xFFFFu : 0u; }

/* Reset via the camera power-enable GPIO (same scheme as IMX219/IMX477). */
int ov9281_reset(XGpio *gpio, uint32_t gpio_mask)
{
    XGpio_DiscreteClear(gpio, 1, gpio_mask);
    usleep(100000);
    XGpio_DiscreteSet(gpio, 1, gpio_mask);
    usleep(100000);
    return XST_SUCCESS;
}
