// disc_strobe.c — AXI-Lite control/read for the disc_strobe core. See disc_strobe.h.
// SPDX-License-Identifier: Apache-2.0
//
// The core is 4x-quadrature + Q16 fractional phase accumulator: cpr / target / fire_step are
// written in Q16 (counts x65536). `position` reads back as an integer 4x count (acc>>16).
#include "disc_strobe.h"
#ifdef DISC_STROBE_PRESENT

#include "xil_io.h"
#include "xil_printf.h"

// Register offsets from the HLS-generated xdisc_strobe_hw.h (bundle=ctrl).
#define R_CPR          0x10   // RW  (Q16)
#define R_TARGET       0x18   // RW  (Q16)
#define R_PULSE        0x20   // RW
#define R_ENABLE       0x28   // RW
#define R_FIRE_STEP    0x30   // RW: Q16 counts between fire points (= cpr/N)
#define R_NUM_FIRES    0x38   // RW: fire exactly this many pulses/rev (= N)
#define R_POSITION     0x40   // RO: integer 4x count
#define R_REV_COUNT    0x50   // RO

#define CPR_TRIM_Q16   6554u  // C/V live-trim step = 0.1 count (0.1 * 65536)

static uint32_t g_cpr_q16 = DISC_STROBE_CPR_Q16_DEFAULT;  // fractional counts/rev, Q16
static uint32_t g_target  = 0;                            // target angle in integer counts
static uint32_t g_pulse   = DISC_STROBE_PULSE_DEFAULT;
static uint32_t g_en      = 0;
static uint32_t g_ncap    = 8;                            // captures per revolution
static uint32_t g_step_q16 = DISC_STROBE_CPR_Q16_DEFAULT; // fire_step (Q16) = cpr / g_ncap

static disc_strobe_cam_cb_t g_cam_cb = 0;   /* camera FSIN-trigger toggle (optional) */
static int                  g_cam_on = 0;   /* shadow of the sensor trigger-mode state */

void disc_strobe_set_cam_cb(disc_strobe_cam_cb_t cb) { g_cam_cb = cb; }

static inline void wr(uint32_t off, uint32_t v) { Xil_Out32(DISC_STROBE_BASE + off, v); }
static inline uint32_t rd(uint32_t off)         { return Xil_In32(DISC_STROBE_BASE + off); }

/* Largest useful target: the first fire point must lie strictly inside the first step (target <
 * cpr/N), else the last of the N points lands past cpr and is never reached (fewer than N
 * captures/rev; target >= cpr fires none). Any larger offset just repeats a reachable grid. */
static uint32_t target_max(void) { return g_step_q16 ? (g_step_q16 - 1u) >> 16 : 0u; }

/* fire_step = cpr / captures, kept in Q16 (the divide lives here so the fabric stays add-only).
 * Recomputed whenever cpr or the capture count changes, so a cpr trim rescales the whole
 * N-point grid together. Clamp: N >= 1, step >= 1. */
static void recompute_step(void)
{
    uint32_t n = g_ncap ? g_ncap : 1u;
    g_step_q16 = g_cpr_q16 / n;
    if (g_step_q16 == 0) g_step_q16 = 1u;
    wr(R_FIRE_STEP, g_step_q16);
    wr(R_NUM_FIRES, n);          // core fires EXACTLY n pulses/rev (no tight pair near wrap)
    if (g_target > target_max()) {           // step shrank (N up / cpr down): re-clamp target
        g_target = target_max();
        wr(R_TARGET, g_target * DISC_STROBE_Q16);
    }
}

/* cpr fine-trim in Q16 (used by the C/V keys for live drift-null). */
static void set_cpr_q16(uint32_t cpr_q16)
{
    if (cpr_q16 < DISC_STROBE_CPR_Q16_MIN) cpr_q16 = DISC_STROBE_CPR_Q16_MIN;
    if (cpr_q16 > DISC_STROBE_CPR_Q16_MAX) cpr_q16 = DISC_STROBE_CPR_Q16_MAX;
    g_cpr_q16 = cpr_q16;
    wr(R_CPR, g_cpr_q16);
    recompute_step();
}

void disc_strobe_set_cpr(uint32_t cpr)      { set_cpr_q16(cpr * DISC_STROBE_Q16); }   /* whole counts */
void disc_strobe_set_target(uint32_t angle) { uint32_t m = target_max(); g_target = angle > m ? m : angle;
                                              wr(R_TARGET, g_target * DISC_STROBE_Q16); }
void disc_strobe_set_pulse(uint32_t clks)   { g_pulse = clks;   wr(R_PULSE, g_pulse); }
void disc_strobe_enable(int on)             { g_en = on ? 1u : 0u; wr(R_ENABLE, g_en); }
void disc_strobe_set_captures(uint32_t n)   { g_ncap = n ? n : 1u;  recompute_step(); }

uint32_t disc_strobe_position(void) { return rd(R_POSITION); }
uint32_t disc_strobe_revs(void)     { return rd(R_REV_COUNT); }
uint32_t disc_strobe_cpr(void)      { return g_cpr_q16 >> 16; }   /* integer counts for fine-count RPM */
uint32_t disc_strobe_cpr_x10(void)  { return (uint32_t)(((uint64_t)g_cpr_q16 * 10u + 32768u) >> 16); }
uint32_t disc_strobe_cpr_q16(void)  { return g_cpr_q16; }
uint32_t disc_strobe_target(void)   { return g_target; }
uint32_t disc_strobe_target_max(void) { return target_max(); }
uint32_t disc_strobe_captures(void) { return g_ncap; }
int      disc_strobe_enabled(void)  { return (int)g_en; }

/* cpr as whole.hundredths for prints. */
#define CPR_WHOLE  (g_cpr_q16 >> 16)
#define CPR_FRAC2  (((g_cpr_q16 & 0xFFFFu) * 100u) >> 16)
#define CPR_LIMIT  (g_cpr_q16 == DISC_STROBE_CPR_Q16_MIN ? " (min)" : \
                    g_cpr_q16 == DISC_STROBE_CPR_Q16_MAX ? " (max)" : "")

void disc_strobe_init(void)
{
    g_ncap = 8;
    wr(R_CPR,    g_cpr_q16);
    wr(R_PULSE,  g_pulse);
    recompute_step();          // fire_step = cpr / 8 -> 8 captures per rev (default)
    g_target = target_max() / 2u;            // default target: mid-point of its 0..cpr/N range
    wr(R_TARGET, g_target * DISC_STROBE_Q16);
    wr(R_ENABLE, 0);           // start with the strobe OFF (OV9281 free-runs until armed)
    g_en = 0;
    xil_printf("strobe: init cpr=%lu.%02lu target=%lu pulse=%lu clk captures=%lu (OFF, 4x/Q16)\r\n",
               (unsigned long)CPR_WHOLE, (unsigned long)CPR_FRAC2,
               (unsigned long)g_target, (unsigned long)g_pulse, (unsigned long)g_ncap);
}

void disc_strobe_status(void)
{
    xil_printf("strobe: en=%lu cpr=%lu.%02lu target=%lu pulse=%lu captures=%lu step=%lu.%02lu  "
               "pos=%lu revs=%lu\r\n",
               (unsigned long)g_en, (unsigned long)CPR_WHOLE, (unsigned long)CPR_FRAC2,
               (unsigned long)g_target, (unsigned long)g_pulse, (unsigned long)g_ncap,
               (unsigned long)(g_step_q16 >> 16), (unsigned long)(((g_step_q16 & 0xFFFFu)*100u)>>16),
               (unsigned long)disc_strobe_position(), (unsigned long)disc_strobe_revs());
}

// Console keys (routed from motor_poll). Uppercase to avoid clashing with motor keys.
//   S strobe on/off   F camera FSIN trigger on/off   N captures/rev cycle (1,2,4,8,16)
//   T target +5   G target -5   C cpr +0.1   V cpr -0.1   Y status
int disc_strobe_poll_key(int ch)
{
    switch (ch) {
    case 'S': disc_strobe_enable(!g_en);
              xil_printf("strobe: %s\r\n", g_en ? "ARMED" : "off"); return 1;
    case 'N': { uint32_t n = g_ncap << 1; if (n > 16u) n = 1u;   /* 1->2->4->8->16->1 */
                disc_strobe_set_captures(n);
                xil_printf("strobe: captures=%lu/rev  step=%lu.%02lu counts  target=%lu (max %lu)\r\n",
                           (unsigned long)g_ncap, (unsigned long)(g_step_q16>>16),
                           (unsigned long)(((g_step_q16 & 0xFFFFu)*100u)>>16),
                           (unsigned long)g_target, (unsigned long)target_max()); } return 1;
    case 'F': if (g_cam_cb) { g_cam_on = !g_cam_on; g_cam_cb(g_cam_on);
                  xil_printf("strobe: camera FSIN trigger %s\r\n", g_cam_on ? "ON" : "OFF"); }
              else xil_printf("strobe: no camera trigger hook registered\r\n");
              return 1;
    case 'T': disc_strobe_set_target(g_target + 5);            /* clamped to target_max() */
              xil_printf("strobe: target=%lu%s\r\n", (unsigned long)g_target,
                         g_target == target_max() ? " (max)" : ""); return 1;
    case 'G': disc_strobe_set_target(g_target >= 5 ? g_target - 5 : 0);
              xil_printf("strobe: target=%lu%s\r\n", (unsigned long)g_target,
                         g_target == 0u ? " (min)" : ""); return 1;
    case 'C': set_cpr_q16(g_cpr_q16 + CPR_TRIM_Q16);            /* +0.1 count (drift-null), clamped */
              xil_printf("strobe: cpr=%lu.%02lu%s\r\n", (unsigned long)CPR_WHOLE, (unsigned long)CPR_FRAC2, CPR_LIMIT); return 1;
    case 'V': set_cpr_q16(g_cpr_q16 - CPR_TRIM_Q16);            /* -0.1 count, clamped (g_cpr_q16 >= MIN > trim) */
              xil_printf("strobe: cpr=%lu.%02lu%s\r\n", (unsigned long)CPR_WHOLE, (unsigned long)CPR_FRAC2, CPR_LIMIT); return 1;
    case 'Y': disc_strobe_status(); return 1;
    default:  return 0;
    }
}

#endif // DISC_STROBE_PRESENT
