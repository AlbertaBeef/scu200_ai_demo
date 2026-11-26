// mbpost.cpp - SCU200 overlay renderer (scu200_ai_demo03): draws the classification overlay
// straight into the HDMI background frame buffer in DDR, replacing ~42 ms of MicroBlaze drawing
// (cache-miss bound) with ~1 ms of burst AXI traffic.
// SPDX-License-Identifier: Apache-2.0
//
// Draws, per call, exactly what Vitis/common/src/mnist_pre.c draw_live_full + draw_pos_full draw
// on demo03 (112x112 input, 1:1 preview), minus labels/value text (labels are in the background
// image, values stay in firmware):
//   preview : 112x112 at (1340,680), grey g = q<0 ? 0 : 2q from mbdpu03's 112x112 int8 input
//   bars    : 10 x (260x18) at (1525, 602+27i); min-max normalised logits, winner bright cyan
//   DX band : (1336,794) 120x18 - black, track, teal fill centre->ox, white centre tick, marker
//   DY band : (1318,676) 18x120 - same, vertical (oy > 0 = up)
//
// Rows are processed as 8-pixel aligned spans (24 B = 3 x 64-bit words) so every DDR access is an
// aligned burst: each span is burst-read from the pristine background (bg), the overlay pixels are
// composed over it, and the span is burst-written to fb. Pixels in the padded span outside the
// overlay keep the background, so alignment never damages the art and "clearing" restores it.
//
// Interface: m_axi 'gmem' (img, out, bg, fb), s_axilite 'control' (all args + return).

#include "mbpost.h"

typedef ap_uint<9> u9;                  // x offset inside a span (0..271)

// per-pixel colour codes (resolved to RGB at the end)
enum { K_BG = 0, K_BLACK, K_TRACK, K_TEAL, K_WHITE, K_GREY, K_WIN, K_BAR };

// span-relative x constants (block A span starts at A_X0, bars span at B_X0)
#define RA(x)  ((x) - A_X0)
#define RB(x)  ((x) - B_X0)

struct rowp {                           // everything about a row that is constant along it
    bool mode_bar;                      // false = block A row, true = bar row
    bool prev, dxb, dx_trk, dx_tick, dx_mk, dyb, dy_trk, dy_fill, dy_tick, dy_mk;
    u9   fa, fb, ma, mb;                // DX fill [fa,fb) and marker [ma,mb), span-relative
    u9   bend;                          // bar: coloured while xl < bend
    bool bwin;
};

static inline bool inr(u9 v, int a, int b) { return v >= (unsigned)a && v < (unsigned)b; }

static ap_uint<3> pix(const rowp &p, u9 xl)
{
#pragma HLS INLINE
    if (p.mode_bar) {
        if (!inr(xl, RB(BAR_X), RB(BAR_X + BAR_W))) return K_BG;
        return xl < p.bend ? (p.bwin ? K_WIN : K_BAR) : K_BLACK;
    }
    if (p.prev && inr(xl, RA(ROI_X), RA(ROI_X + ROI_PX))) return K_GREY;
    if (p.dxb && inr(xl, RA(DXB_X0), RA(DXB_X1))) {
        ap_uint<3> k = K_BLACK;
        if (p.dx_trk && inr(xl, RA(ROI_X), RA(ROI_X + ROI_PX))) k = K_TRACK;
        if (p.dx_trk && xl >= p.fa && xl < p.fb) k = K_TEAL;
        if (p.dx_tick && inr(xl, RA(CX - 1), RA(CX + 1))) k = K_WHITE;
        if (p.dx_mk && xl >= p.ma && xl < p.mb) k = K_TEAL;
        return k;
    }
    if (p.dyb && inr(xl, RA(DYB_X0), RA(DYB_X1))) {
        ap_uint<3> k = K_BLACK;
        if (p.dy_trk && inr(xl, RA(DY_TX), RA(DY_TX + POS_TRK))) k = K_TRACK;
        if (p.dy_fill && inr(xl, RA(DY_TX), RA(DY_TX + POS_TRK))) k = K_TEAL;
        if (p.dy_tick && inr(xl, RA(DY_TX - 4), RA(DY_TX + POS_TRK + 4))) k = K_WHITE;
        if (p.dy_mk && inr(xl, RA(DY_TX + POS_TRK/2 - POS_MK_L/2), RA(DY_TX + POS_TRK/2 + POS_MK_L/2))) k = K_TEAL;
        return k;
    }
    return K_BG;
}

static inline bool iny(int v, int a, int b) { return v >= a && v < b; }

// one 8-px aligned span of row y: bg -> compose -> fb (the single instance: block A and bar rows)
static void do_row(const w64 *bg, w64 *fb, uint32_t stride, int y, int xa, int ng,
                   const rowp &p, const w64 imgw[ROI_PX * IMG_WPR])
{
#pragma HLS INLINE off
    w64 line[MAX_WORDS];
#pragma HLS ARRAY_PARTITION variable=line cyclic factor=3
    const uint32_t base = ((uint32_t)y * stride + (uint32_t)xa * 3u) >> 3;
    const int nw = ng * 3;
rd: for (int w = 0; w < nw; w++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=54 max=102
        line[w] = bg[base + w];
    }
    const int r = y - ROI_Y;
cmp: for (int g = 0; g < ng; g++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=18 max=34
        // preview bytes (block A only): span x = A_X0 + 8g, preview col = 8g - 28 = 8m + 4
        // -> words m (bytes 4..7) and m+1 (bytes 0..3), m = g - 4
        const int m = g - 4;
        w64 wa = 0, wb = 0;
        if (p.prev && m >= 0 && m < IMG_WPR)          wa = imgw[r * IMG_WPR + m];
        if (p.prev && m + 1 >= 0 && m + 1 < IMG_WPR)  wb = imgw[r * IMG_WPR + m + 1];
        w64 o[3];
        for (int k = 0; k < 3; k++) o[k] = line[3 * g + k];
        for (int j = 0; j < 8; j++) {
#pragma HLS UNROLL
            const u9 xl = (u9)(8 * g + j);
            const ap_uint<3> k = pix(p, xl);
            const u8 pv = j < 4 ? (u8)wa.range(8 * (4 + j) + 7, 8 * (4 + j))
                                : (u8)wb.range(8 * (j - 4) + 7, 8 * (j - 4));
            const u8 gq = pv[7] ? (u8)0 : (u8)(pv << 1);
            u8 R, G, B;
            switch ((int)k) {
            case K_BLACK: R = 0;    G = 0;    B = 0;    break;
            case K_TRACK: R = 0x30; G = 0x38; B = 0x40; break;
            case K_TEAL:  R = 0x22; G = 0xE6; B = 0xE6; break;
            case K_WHITE: R = 0xF0; G = 0xF4; B = 0xF8; break;
            case K_GREY:  R = gq;   G = gq;   B = gq;   break;
            case K_WIN:   R = 0;    G = 0xEB; B = 0xEB; break;
            default:      R = 0;    G = 0x50; B = 0x50; break;   // K_BAR (K_BG unused)
            }
            if (k != K_BG) {
                const int b0 = 3 * j;                  // byte index inside the 24-byte group
                o[(b0 + 0) / 8].range(8 * ((b0 + 0) % 8) + 7, 8 * ((b0 + 0) % 8)) = R;
                o[(b0 + 1) / 8].range(8 * ((b0 + 1) % 8) + 7, 8 * ((b0 + 1) % 8)) = G;
                o[(b0 + 2) / 8].range(8 * ((b0 + 2) % 8) + 7, 8 * ((b0 + 2) % 8)) = B;
            }
        }
        for (int k = 0; k < 3; k++) line[3 * g + k] = o[k];
    }
wr: for (int w = 0; w < nw; w++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=54 max=102
        fb[base + w] = line[w];
    }
}

void mbpost(const w64 *img, const int32_t *out, const w64 *bg, w64 *fb,
            uint32_t stride, int32_t ox, int32_t oy)
{
#pragma HLS INTERFACE m_axi     port=img offset=slave bundle=gmem depth=1568 max_read_burst_length=128 num_read_outstanding=2
#pragma HLS INTERFACE m_axi     port=out offset=slave bundle=gmem depth=13 max_read_burst_length=128 num_read_outstanding=2
#pragma HLS INTERFACE m_axi     port=bg  offset=slave bundle=gmem depth=777600 max_read_burst_length=128 num_read_outstanding=2
#pragma HLS INTERFACE m_axi     port=fb  offset=slave bundle=gmem depth=777600 max_write_burst_length=128 num_write_outstanding=2
#pragma HLS INTERFACE s_axilite port=img    bundle=control
#pragma HLS INTERFACE s_axilite port=out    bundle=control
#pragma HLS INTERFACE s_axilite port=bg     bundle=control
#pragma HLS INTERFACE s_axilite port=fb     bundle=control
#pragma HLS INTERFACE s_axilite port=stride bundle=control
#pragma HLS INTERFACE s_axilite port=ox     bundle=control
#pragma HLS INTERFACE s_axilite port=oy     bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    // gauge offsets: firmware clamps to +-POS_HALF; clamp again so a bad value can't run off
    int gx = ox > POS_HALF ? POS_HALF : (ox < -POS_HALF ? -POS_HALF : (int)ox);
    int gy = oy > POS_HALF ? POS_HALF : (oy < -POS_HALF ? -POS_HALF : (int)oy);

    // ---- bars: lengths exactly as mnist_pre.c (int32 wrap on the differences) ----
    int32_t lg[10];
#pragma HLS ARRAY_PARTITION variable=lg complete
lg_rd: for (int i = 0; i < 10; i++) {
#pragma HLS PIPELINE II=1
        lg[i] = out[i];
    }
    int32_t mn = lg[0], mx = lg[0];
    int arg = 0;
    for (int i = 1; i < 10; i++) {
#pragma HLS UNROLL
        if (lg[i] < mn) mn = lg[i];
        if (lg[i] > mx) { mx = lg[i]; arg = i; }
    }
    int32_t rng = (int32_t)((uint32_t)mx - (uint32_t)mn);
    if (rng <= 0) rng = 1;
    int len[10];
len: for (int i = 0; i < 10; i++) {
        // floor((d * 260) / rng), d = lg - mn in [0, rng] so the quotient is < 512: 9-step
        // restoring long division (shift + subtract per step, no multiplier/divider)
        int32_t d = (int32_t)((uint32_t)lg[i] - (uint32_t)mn);
        int64_t rem = (int64_t)d * BAR_W;
        const int64_t den = rng;
        int L = 0;
        if (rem > 0) {
            for (int b = 8; b >= 0; b--) {
#pragma HLS PIPELINE II=1
                int64_t t = den << b;
                if (rem >= t) { rem -= t; L |= 1 << b; }
            }
        }
        len[i] = L > BAR_W ? BAR_W : L;
    }

    // ---- preview input -> on-chip ----
    w64 imgw[ROI_PX * IMG_WPR];
#pragma HLS ARRAY_PARTITION variable=imgw cyclic factor=2
img_rd: for (int i = 0; i < ROI_PX * IMG_WPR; i++) {
#pragma HLS PIPELINE II=1
        imgw[i] = img[i];
    }

    // ---- rows: block A (preview + DX/DY bands) then the 10 bars, through ONE do_row ----
    const int fa = gx < 0 ? CX + gx : CX, fbx = gx < 0 ? CX : CX + gx;   // DX fill [fa,fb)
    const int ma = CX + gx - POS_MK_W/2,  mb = CX + gx + POS_MK_W/2;     // DX marker
    const int NA = A_Y1 - A_Y0, NROWS = NA + 10 * BAR_H;
    int bi = 0, bdy = 0;
rows: for (int t = 0; t < NROWS; t++) {
        rowp p;
        int y, xa, ng;
        if (t < NA) {
            y = A_Y0 + t; xa = A_X0; ng = A_NG;
            p.mode_bar = false;
            p.prev    = iny(y, ROI_Y, ROI_Y + ROI_PX);
            p.dxb     = iny(y, DXB_Y0, DXB_Y1);
            p.dx_trk  = iny(y, DX_TY, DX_TY + POS_TRK);
            p.dx_tick = iny(y, DX_TY - 4, DX_TY + POS_TRK + 4);
            p.dx_mk   = iny(y, DX_TY + POS_TRK/2 - POS_MK_L/2, DX_TY + POS_TRK/2 + POS_MK_L/2);
            p.dyb     = iny(y, DYB_Y0, DYB_Y1);
            p.dy_trk  = iny(y, ROI_Y, ROI_Y + ROI_PX);
            p.dy_fill = (gy > 0 && iny(y, CY - gy, CY)) || (gy < 0 && iny(y, CY, CY - gy));
            p.dy_tick = iny(y, CY - 1, CY + 1);
            p.dy_mk   = iny(y, CY - gy - POS_MK_W/2, CY - gy + POS_MK_W/2);
            p.fa = RA(fa); p.fb = RA(fbx); p.ma = RA(ma); p.mb = RA(mb);
            p.bend = 0; p.bwin = false;
        } else {
            y = BAR_Y0 + bi * BAR_DY + 2 + bdy; xa = B_X0; ng = B_NG;
            p.mode_bar = true;
            p.prev = p.dxb = p.dx_trk = p.dx_tick = p.dx_mk = false;
            p.dyb = p.dy_trk = p.dy_fill = p.dy_tick = p.dy_mk = false;
            p.fa = p.fb = p.ma = p.mb = 0;
            p.bend = RB(BAR_X) + len[bi]; p.bwin = (bi == arg);
            if (++bdy == BAR_H) { bdy = 0; bi++; }
        }
        do_row(bg, fb, stride, y, xa, ng, p, imgw);
    }
}

