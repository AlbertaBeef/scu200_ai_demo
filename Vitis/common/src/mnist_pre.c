// mnist_pre.c — camera 640x480 RGB8 -> mbdpu01 28x28 INT8 input. See mnist_pre.h.
// SPDX-License-Identifier: Apache-2.0

#include "mnist_pre.h"

// Center-crop MNIST_CROP^2, box-average MNIST_BLK^2 blocks -> 28x28, RGB->luma,
// quantize 0..255 -> 0..127 (>>1), optional intensity invert.
void mnist_downscale_rgb8(const uint8_t *rgb, uint32_t stride, int invert, int8_t *img28)
{
    const uint32_t cx = (MNIST_FRAME_W - MNIST_CROP) / 2;   // 96
    const uint32_t cy = (MNIST_FRAME_H - MNIST_CROP) / 2;   // 16

    for (int oy = 0; oy < MNIST_DIM; oy++) {
        for (int ox = 0; ox < MNIST_DIM; ox++) {
            uint32_t sum = 0;
            for (int by = 0; by < MNIST_BLK; by++) {
                const uint8_t *row = rgb
                    + (uint32_t)(cy + oy*MNIST_BLK + by) * stride
                    + (uint32_t)(cx + ox*MNIST_BLK) * 3u;
                for (int bx = 0; bx < MNIST_BLK; bx++) {
                    uint8_t r = row[bx*3 + 0];
                    uint8_t g = row[bx*3 + 1];
                    uint8_t b = row[bx*3 + 2];
                    sum += ((uint32_t)r*77u + (uint32_t)g*150u + (uint32_t)b*29u) >> 8; // luma 0..255
                }
            }
            uint32_t gray = sum / (MNIST_BLK * MNIST_BLK);  // avg luma 0..255
            int q = (int)(gray >> 1);                       // -> 0..127
            if (invert) q = 127 - q;
            img28[oy*MNIST_DIM + ox] = (int8_t)q;
        }
    }
}

#ifndef __MNIST_PRE_HOST_TEST__   // MicroBlaze wrapper (needs the BSP cache API)
#include "xil_cache.h"

void mnist_preprocess_rgb8(uintptr_t cam_frmbuf, uint32_t stride, int invert, uintptr_t img_out)
{
    // Camera frame is DMA-written to DDR: invalidate so the CPU reads fresh pixels.
    Xil_DCacheInvalidateRange(cam_frmbuf, (uintptr_t)stride * MNIST_FRAME_H);
    mnist_downscale_rgb8((const uint8_t *)cam_frmbuf, stride, invert, (int8_t *)img_out);
    // mbdpu01's AXI master reads img_out from DDR: flush the CPU writes back.
    Xil_DCacheFlushRange(img_out, MNIST_N);
}

// 5x7 bitmap font, digits 0-9 (bit4 = leftmost column).
static const uint8_t FONT5x7[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};

// Lower-right "Classification" panel (above the bg label). ROI preview on top; below it a
// per-digit confidence bar chart: static 0-9 labels (drawn once) + live bars from the logits.
// Panel shifted right; ROI preview to the LEFT of the bar chart.
#define BAR_Y0   600            /* first bar row y */
#define BAR_DY   27             /* row pitch (label 21 + bar 18 fit) */
#define LBL_X    1490           /* digit labels 0-9 (static), right of the ROI */
#define LBL_S    3              /* label 5x7 font scale -> 15x21 px */
#define BAR_X    1525           /* bars start x */
#define BAR_MAX_W 260           /* full-scale bar width (-> ~1785, within screen) */
#define BAR_H    18
#define ROI_X    1340           /* ROI preview LEFT of the labels */
#define ROI_Y    680            /* ~vertically centered against the 10-row bar block */
#define ROI_S    4              /* 28x28 -> 112x112 ROI preview */

// Draw the static 0-9 digit labels once (they never change; only the bars update live).
void mnist_draw_static(uintptr_t fb, uint32_t stride)
{
    for (int i = 0; i < 10; i++) {
        int y0 = BAR_Y0 + i*BAR_DY;
        for (int r = 0; r < 7; r++) {
            uint8_t bits = FONT5x7[i][r];
            for (int dy = 0; dy < LBL_S; dy++) {
                uint8_t *p = (uint8_t *)(fb + (uintptr_t)(y0 + r*LBL_S + dy)*stride + (uintptr_t)LBL_X*3u);
                for (int c = 0; c < 5; c++) {
                    uint8_t v = ((bits >> (4 - c)) & 1) ? 255 : 0;
                    for (int dx = 0; dx < LBL_S; dx++) { *p++ = v; *p++ = v; *p++ = v; }
                }
            }
        }
    }
    Xil_DCacheFlushRange(fb + (uintptr_t)BAR_Y0*stride, (uintptr_t)(10*BAR_DY)*stride);
}

// Live: the ROI preview + one confidence bar per digit (min-max normalized logits,
// winner highlighted). Bars clear-then-fill so they shrink/grow correctly each frame.
void mnist_draw_live(uintptr_t fb, uint32_t stride, const int8_t *img28, const int32_t *logits)
{
    // ROI preview (row-pointer sequential writes)
    for (int r = 0; r < MNIST_DIM; r++) {
        const int8_t *src = &img28[r * MNIST_DIM];
        for (int dy = 0; dy < ROI_S; dy++) {
            uint8_t *p = (uint8_t *)(fb + (uintptr_t)(ROI_Y + r*ROI_S + dy)*stride + (uintptr_t)ROI_X*3u);
            for (int c = 0; c < MNIST_DIM; c++) {
                int q = src[c];
                uint8_t g = (uint8_t)(q < 0 ? 0 : (q > 127 ? 255 : q * 2));
                for (int dx = 0; dx < ROI_S; dx++) { *p++ = g; *p++ = g; *p++ = g; }
            }
        }
    }
    // normalize logits to bar lengths; find the winner
    int32_t mn = logits[0], mx = logits[0], arg = 0;
    for (int i = 1; i < 10; i++) {
        if (logits[i] < mn) mn = logits[i];
        if (logits[i] > mx) { mx = logits[i]; arg = i; }
    }
    int32_t rng = mx - mn; if (rng <= 0) rng = 1;
    for (int i = 0; i < 10; i++) {
        int len = (int)(((int64_t)(logits[i] - mn) * BAR_MAX_W) / rng);
        if (len < 0) len = 0; else if (len > BAR_MAX_W) len = BAR_MAX_W;
        uint8_t Rc = 0, Gc = (i == arg) ? 235 : 80, Bc = (i == arg) ? 235 : 80;  /* winner bright cyan */
        int by = BAR_Y0 + i*BAR_DY + 2;
        for (int dy = 0; dy < BAR_H; dy++) {
            uint8_t *p = (uint8_t *)(fb + (uintptr_t)(by + dy)*stride + (uintptr_t)BAR_X*3u);
            for (int x = 0; x < BAR_MAX_W; x++)
                if (x < len) { *p++ = Rc; *p++ = Gc; *p++ = Bc; }
                else         { *p++ = 0;  *p++ = 0;  *p++ = 0;  }
        }
    }
    int top  = (ROI_Y < BAR_Y0) ? ROI_Y : BAR_Y0;
    int roib = ROI_Y + MNIST_DIM*ROI_S;
    int barb = BAR_Y0 + 9*BAR_DY + 2 + BAR_H + 2;
    int bot  = (roib > barb) ? roib : barb;
    Xil_DCacheFlushRange(fb + (uintptr_t)top*stride, (uintptr_t)(bot - top)*stride);
}

/* ---- Small on-screen HUD (RPM / captures-per-second), drawn into the background ------ */

/* General 5x7 glyph rows (bit4 = leftmost column), for the characters the HUD needs. */
static const uint8_t GLYPH_SP[7] = {0,0,0,0,0,0,0};
static const uint8_t GLYPH_DOT[7]= {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
static const uint8_t GLYPH_SLH[7]= {0x01,0x02,0x04,0x04,0x08,0x10,0x10};
static const uint8_t GLYPH_DSH[7]= {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
static const uint8_t GLYPH_PLS[7]= {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
static const uint8_t GLYPH_A[7]  = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
static const uint8_t GLYPH_B[7]  = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
static const uint8_t GLYPH_C[7]  = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
static const uint8_t GLYPH_D[7]  = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
static const uint8_t GLYPH_E[7]  = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
static const uint8_t GLYPH_F[7]  = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
static const uint8_t GLYPH_G[7]  = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
static const uint8_t GLYPH_I[7]  = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
static const uint8_t GLYPH_L[7]  = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
static const uint8_t GLYPH_M[7]  = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
static const uint8_t GLYPH_N[7]  = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
static const uint8_t GLYPH_O[7]  = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t GLYPH_P[7]  = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
static const uint8_t GLYPH_R[7]  = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
static const uint8_t GLYPH_S[7]  = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
static const uint8_t GLYPH_T[7]  = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
static const uint8_t GLYPH_U[7]  = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t GLYPH_V[7]  = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
static const uint8_t GLYPH_X[7]  = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};

// Map a character to its 7-row bitmap (digits reuse the bar-chart FONT5x7). Unknown -> blank.
static const uint8_t *osd_glyph_rows(char c)
{
    if (c >= '0' && c <= '9') return FONT5x7[c - '0'];
    switch (c) {
    case ' ': return GLYPH_SP;  case '.': return GLYPH_DOT; case '/': return GLYPH_SLH;
    case '-': return GLYPH_DSH; case '+': return GLYPH_PLS; case 'A': return GLYPH_A;   case 'B': return GLYPH_B;
    case 'C': return GLYPH_C;   case 'D': return GLYPH_D;   case 'E': return GLYPH_E;
    case 'F': return GLYPH_F;   case 'G': return GLYPH_G;   case 'I': return GLYPH_I;
    case 'L': return GLYPH_L;   case 'M': return GLYPH_M;   case 'N': return GLYPH_N;
    case 'O': return GLYPH_O;   case 'P': return GLYPH_P;   case 'R': return GLYPH_R;
    case 'S': return GLYPH_S;   case 'T': return GLYPH_T;   case 'U': return GLYPH_U;
    case 'V': return GLYPH_V;   case 'X': return GLYPH_X;   default:  return GLYPH_SP;
    }
}

// One glyph cell, scaled. Every pixel is written (fg for set bits, bg for clear) so a
// redraw with a shorter number fully overwrites the old one (no ghosting). fg/bg = 0xRRGGBB.
static void osd_glyph(uintptr_t fb, uint32_t stride, int x, int y, int s,
                      const uint8_t rows[7], uint32_t fg, uint32_t bg)
{
    uint8_t fr = (fg>>16)&0xff, fg_ = (fg>>8)&0xff, fbl = fg&0xff;
    uint8_t br = (bg>>16)&0xff, bg_ = (bg>>8)&0xff, bbl = bg&0xff;
    for (int r = 0; r < 7; r++) {
        uint8_t bits = rows[r];
        for (int dy = 0; dy < s; dy++) {
            uint8_t *p = (uint8_t *)(fb + (uintptr_t)(y + r*s + dy)*stride + (uintptr_t)x*3u);
            for (int c = 0; c < 5; c++) {
                int on = (bits >> (4 - c)) & 1;
                uint8_t R = on?fr:br, G = on?fg_:bg_, B = on?fbl:bbl;
                for (int dx = 0; dx < s; dx++) { *p++ = R; *p++ = G; *p++ = B; }
            }
        }
    }
}

static void osd_text(uintptr_t fb, uint32_t stride, int x, int y, int s,
                     const char *str, uint32_t fg, uint32_t bg)
{
    for (; *str; str++) { osd_glyph(fb, stride, x, y, s, osd_glyph_rows(*str), fg, bg); x += 6*s; }
}

static int osd_textw(const char *str, int s) { int n = 0; while (*str++) n++; return n*6*s; }

static void osd_fill(uintptr_t fb, uint32_t stride, int x, int y, int w, int h, uint32_t color)
{
    uint8_t R = (color>>16)&0xff, G = (color>>8)&0xff, B = color&0xff;
    for (int r = 0; r < h; r++) {
        uint8_t *p = (uint8_t *)(fb + (uintptr_t)(y + r)*stride + (uintptr_t)x*3u);
        for (int c = 0; c < w; c++) { *p++ = R; *p++ = G; *p++ = B; }
    }
}

// decimal of v>=0 into buf, returns length.
static int osd_u2s(char *buf, int v)
{
    char t[12]; int n = 0; if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) buf[i] = t[n-1-i];
    buf[n] = 0; return n;
}

// Format a fixed-point x10 value ("12.3") into buf; negative -> "--".
static void osd_fx10(char *buf, int x10)
{
    if (x10 < 0) { buf[0]='-'; buf[1]='-'; buf[2]=0; return; }
    int n = osd_u2s(buf, x10/10);
    buf[n++] = '.'; buf[n++] = (char)('0' + x10%10); buf[n] = 0;
}

void mnist_stats_panel(uintptr_t fb, uint32_t stride, int x0, int y0, const mnist_stats_t *s)
{
    enum { W = 230, H = 320, PAD = 10, S = 3, PITCH = 28 };  /* S=3 -> 15x21 px, matches LBL_S */
    const uint32_t BG = 0x0A0E14u, WHITE = 0xF0F4F8u, CYAN = 0x22E6E6u,
                   GREEN = 0x2ECC71u, GREY = 0x707880u;
    char v[16];

    osd_fill(fb, stride, x0, y0, W, H, BG);          /* panel background */
    osd_fill(fb, stride, x0, y0, W, 3, CYAN);        /* top accent bar   */

    /* One label:value row each; value right-aligned, in colour `vcol`. */
    int y = y0 + 12;
    #define STAT_ROW(lbl, str, vcol) do { \
        osd_text(fb, stride, x0 + PAD, y, S, (lbl), WHITE, BG); \
        osd_text(fb, stride, x0 + W - PAD - osd_textw((str), S), y, S, (str), (vcol), BG); \
        y += PITCH; \
    } while (0)

    if (s->rpm < 0) { v[0]='-'; v[1]='-'; v[2]=0; } else osd_u2s(v, s->rpm);
    STAT_ROW("RPM", v, CYAN);
    osd_fx10(v, s->caps_x10); STAT_ROW("CAP", v, CYAN);   /* captures/second      */
    osd_fx10(v, s->pre_x10);  STAT_ROW("PRE", v, CYAN);   /* mbpre latency  (ms)  */
    osd_fx10(v, s->dpu_x10);  STAT_ROW("DPU", v, CYAN);   /* mbdpu01 latency (ms) */
    osd_fx10(v, s->ovl_x10);  STAT_ROW("OVL", v, CYAN);   /* overlay latency (ms) */
    STAT_ROW("TRIG", s->trig_on      ? "ON"  : "OFF", s->trig_on      ? GREEN : GREY);
    STAT_ROW("STRB", s->strobe_armed ? "ARM" : "OFF", s->strobe_armed ? GREEN : GREY);
    osd_u2s(v, s->expo); STAT_ROW("EXPO", v, CYAN);       /* exposure (lines)     */
    osd_fx10(v, s->gain_x10); STAT_ROW("GAIN", v, CYAN);  /* analog gain (x)      */
    osd_u2s(v, s->target); STAT_ROW("TGT", v, CYAN);      /* strobe target (counts) */
    osd_fx10(v, s->cpr_x10); STAT_ROW("CPR", v, CYAN);    /* counts per rev        */
    #undef STAT_ROW

    Xil_DCacheFlushRange(fb + (uintptr_t)y0*stride, (uintptr_t)H*stride);
}

#define GAUGE_H   58            /* gauge box height: label row + track/marker */

/* Shared gauge: label left, value text right-aligned, then a track spanning [mn, mx] with a
 * teal fill from the reference point to the value and a teal marker at the value. With
 * show_ref the reference is `ref` and gets a white tick (deviation-from-default view);
 * without it the fill runs from mn (plain level view). */
static void gauge_draw(uintptr_t fb, uint32_t stride, int x0, int y0, int w,
                       const char *label, const char *valtxt,
                       uint32_t val, uint32_t mn, uint32_t mx, uint32_t ref, int show_ref)
{
    const uint32_t BLACK = 0x000000u, WHITE = 0xF0F4F8u, TEAL = 0x22E6E6u, TRACK = 0x303840u;
    enum { S = 3, TRK_H = 8, MK_W = 6, MK_H = 22 };
    const int TRK_Y = y0 + 34;

    if (val < mn) val = mn;
    if (val > mx) val = mx;
    if (!show_ref) ref = mn;
    const uint32_t span = (mx > mn) ? mx - mn : 1u;   /* mx == mn: marker pinned at the left */
    int xv = (int)(((uint64_t)(val - mn) * (uint32_t)(w - 1) + span/2) / span);
    int xr = (int)(((uint64_t)(ref - mn) * (uint32_t)(w - 1) + span/2) / span);

    osd_fill(fb, stride, x0 - 4, y0, w + 8, GAUGE_H, BLACK);   /* clear box */

    osd_text(fb, stride, x0, y0, S, label, WHITE, BLACK);
    osd_text(fb, stride, x0 + w - osd_textw(valtxt, S), y0, S, valtxt, TEAL, BLACK);

    osd_fill(fb, stride, x0, TRK_Y, w, TRK_H, TRACK);
    int a = xv < xr ? xv : xr, b = xv < xr ? xr : xv;
    osd_fill(fb, stride, x0 + a, TRK_Y, b - a + 1, TRK_H, TEAL);
    if (show_ref) osd_fill(fb, stride, x0 + xr - 1, TRK_Y - 6, 2, TRK_H + 12, WHITE);
    int mk = x0 + xv - MK_W/2;
    if (mk < x0 - 3) mk = x0 - 3;
    if (mk > x0 + w + 3 - MK_W) mk = x0 + w + 3 - MK_W;
    osd_fill(fb, stride, mk, TRK_Y + TRK_H/2 - MK_H/2, MK_W, MK_H, TEAL);

    Xil_DCacheFlushRange(fb + (uintptr_t)y0*stride, (uintptr_t)GAUGE_H*stride);
}

void mnist_cpr_gauge(uintptr_t fb, uint32_t stride, int x0, int y0, int w,
                     uint32_t cpr_q16, uint32_t min_q16, uint32_t max_q16, uint32_t def_q16)
{
    char v[16];
    osd_fx10(v, (int)(((uint64_t)cpr_q16 * 10u + 32768u) >> 16));   /* rounded to 0.1 */
    gauge_draw(fb, stride, x0, y0, w, "CPR", v, cpr_q16, min_q16, max_q16, def_q16, 1);
}

void mnist_target_gauge(uintptr_t fb, uint32_t stride, int x0, int y0, int w,
                        uint32_t target, uint32_t target_max, int auto_on)
{
    char v[24];                                       /* white tick = mid-point (boot default) */
    int n = osd_u2s(v, (int)target);                  /* "value/max" — max moves with N and cpr */
    v[n++] = '/';
    osd_u2s(v + n, (int)target_max);
    gauge_draw(fb, stride, x0, y0, w, auto_on ? "AUTO" : "TGT", v, target, 0u, target_max,
               target_max / 2u, 1);
}

/* ---- dx/dy digit-position gauges framing the ROI preview (mbdpu02) ----------------------
 * The preview is the 28x28 model input at 4 screen px per model px, so a +-14 px gauge span
 * equals the preview's own extent: each marker sits exactly where the digit centre is drawn.
 *   DX: horizontal, under the preview      (x ROI_X..+112, y POS_DX_Y)       left -, right +
 *   DY: vertical, left of the preview      (x POS_DY_X, y ROI_Y..+112)       top +, bottom -
 * Values print below the DX gauge ("DX +1.2" / "DY -0.4", scale 2). Whole box repainted and
 * cache-flushed every call (drawn into the back buffer each capture, like the bars). */
#define POS_SPAN   (MNIST_DIM*ROI_S)        /* 112 screen px = 28 model px */
#define POS_PPX    ROI_S                    /* 4 screen px per model px */
#define POS_DX_Y   (ROI_Y + POS_SPAN + 8)   /* DX track top (8 px under the preview) */
#define POS_DY_X   (ROI_X - 16)             /* DY track left (8 px left of the preview) */
#define POS_TRK    6                        /* track thickness */
#define POS_MK_L   16                       /* marker length across the track */
#define POS_MK_W   4                        /* marker width along the track */
#define POS_TXT_Y  (POS_DX_Y + 18)          /* first value line */
#define POS_BOX_X0 (POS_DY_X - 6)
#define POS_BOX_Y1 (POS_TXT_Y + 2*18)       /* two text lines of 14 px + gap */

static void pos_fmt(char *buf, int v_x100)   /* "+1.2" / "-0.4", rounded to 0.1 */
{
    int neg = v_x100 < 0, a = neg ? -v_x100 : v_x100;
    int x10 = (a + 5) / 10;
    buf[0] = (x10 == 0) ? ' ' : (neg ? '-' : '+');
    osd_fx10(buf + 1, x10);
}

void mnist_draw_pos(uintptr_t fb, uint32_t stride, int dx_x100, int dy_x100)
{
    const uint32_t BLACK = 0x000000u, WHITE = 0xF0F4F8u, TEAL = 0x22E6E6u, TRACK = 0x303840u;
    const int half = POS_SPAN / 2, lim = half * 100 / POS_PPX;     /* +-14.00 px */
    int dx = dx_x100 > lim ? lim : (dx_x100 < -lim ? -lim : dx_x100);
    int dy = dy_x100 > lim ? lim : (dy_x100 < -lim ? -lim : dy_x100);
    int ox = (dx * POS_PPX) / 100, oy = (dy * POS_PPX) / 100;      /* screen px from centre */
    char v[12];

    /* clear: the strip under the preview (DX + text) and the column left of it (DY) */
    osd_fill(fb, stride, POS_BOX_X0, POS_DX_Y - 6, ROI_X + POS_SPAN + 4 - POS_BOX_X0,
             POS_BOX_Y1 - (POS_DX_Y - 6), BLACK);
    osd_fill(fb, stride, POS_BOX_X0, ROI_Y - 4, ROI_X - 2 - POS_BOX_X0, POS_SPAN + 8, BLACK);

    /* DX: track, fill centre->value, centre tick, marker */
    const int cx = ROI_X + half, ty = POS_DX_Y;
    osd_fill(fb, stride, ROI_X, ty, POS_SPAN, POS_TRK, TRACK);
    if (ox) osd_fill(fb, stride, ox < 0 ? cx + ox : cx, ty, ox < 0 ? -ox : ox, POS_TRK, TEAL);
    osd_fill(fb, stride, cx - 1, ty - 4, 2, POS_TRK + 8, WHITE);
    osd_fill(fb, stride, cx + ox - POS_MK_W/2, ty + POS_TRK/2 - POS_MK_L/2, POS_MK_W, POS_MK_L, TEAL);

    /* DY: track, fill centre->value (up = +), centre tick, marker */
    const int cy = ROI_Y + half, tx = POS_DY_X;
    osd_fill(fb, stride, tx, ROI_Y, POS_TRK, POS_SPAN, TRACK);
    if (oy) osd_fill(fb, stride, tx, oy > 0 ? cy - oy : cy, POS_TRK, oy > 0 ? oy : -oy, TEAL);
    osd_fill(fb, stride, tx - 4, cy - 1, POS_TRK + 8, 2, WHITE);
    osd_fill(fb, stride, tx + POS_TRK/2 - POS_MK_L/2, cy - oy - POS_MK_W/2, POS_MK_L, POS_MK_W, TEAL);

    /* values */
    osd_text(fb, stride, ROI_X, POS_TXT_Y, 2, "DX", WHITE, BLACK);
    pos_fmt(v, dx_x100); osd_text(fb, stride, ROI_X + POS_SPAN - osd_textw(v, 2), POS_TXT_Y, 2, v, TEAL, BLACK);
    osd_text(fb, stride, ROI_X, POS_TXT_Y + 18, 2, "DY", WHITE, BLACK);
    pos_fmt(v, dy_x100); osd_text(fb, stride, ROI_X + POS_SPAN - osd_textw(v, 2), POS_TXT_Y + 18, 2, v, TEAL, BLACK);

    Xil_DCacheFlushRange(fb + (uintptr_t)(ROI_Y - 4)*stride, (uintptr_t)(POS_BOX_Y1 - (ROI_Y - 4))*stride);
}

// Capture the crop region resampled to 112x112 grayscale (nearest-neighbour) for training data.
// Mono OV9281 -> R=G=B, so the green byte is the gray value.
void mnist_capture_crop112(uintptr_t cam_frmbuf, uint32_t stride, int x0, int y0, int side,
                           uint8_t *out)
{
    if (side < 1) side = 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + side > MNIST_FRAME_W) side = MNIST_FRAME_W - x0;
    if (y0 + side > MNIST_FRAME_H) { int s2 = MNIST_FRAME_H - y0; if (s2 < side) side = s2; }
    if (side < 1) side = 1;
    Xil_DCacheInvalidateRange(cam_frmbuf + (uintptr_t)y0*stride, (uintptr_t)side*stride);
    for (int oy = 0; oy < ROI_CAP_DIM; oy++) {
        int sy = y0 + (oy * side) / ROI_CAP_DIM;
        const uint8_t *row = (const uint8_t *)(cam_frmbuf + (uintptr_t)sy*stride);
        uint8_t *o = &out[oy*ROI_CAP_DIM];
        for (int ox = 0; ox < ROI_CAP_DIM; ox++) {
            int sx = x0 + (ox * side) / ROI_CAP_DIM;
            o[ox] = row[sx*3 + 1];   // green byte = gray (mono sensor)
        }
    }
}

// ROI box outline into a camera frame buffer, clamped to the 640x480 frame.
void mnist_draw_roi_box(uintptr_t fb, uint32_t stride, int x, int y, int side,
                        uint32_t color, int thick)
{
    int x1 = x + side, y1 = y + side;
    if (x  < 0) x = 0;   if (y  < 0) y = 0;
    if (x1 > MNIST_FRAME_W) x1 = MNIST_FRAME_W;
    if (y1 > MNIST_FRAME_H) y1 = MNIST_FRAME_H;
    int w = x1 - x, h = y1 - y;
    if (w <= 0 || h <= 0) return;
    int t = thick; if (t > w) t = w; if (t > h) t = h;

    osd_fill(fb, stride, x,      y,      w, t, color);   /* top    */
    osd_fill(fb, stride, x,      y1 - t, w, t, color);   /* bottom */
    osd_fill(fb, stride, x,      y,      t, h, color);   /* left   */
    osd_fill(fb, stride, x1 - t, y,      t, h, color);   /* right  */

    Xil_DCacheFlushRange(fb + (uintptr_t)y*stride, (uintptr_t)h*stride);  /* box rows -> DMA */
}
#endif
