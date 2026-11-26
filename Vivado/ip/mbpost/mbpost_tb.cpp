// mbpost_tb.cpp - C-sim: whole-frame byte compare against a golden model ported from
// Vitis/common/src/mnist_pre.c (draw_live_full + draw_pos_full bands).
// SPDX-License-Identifier: Apache-2.0
#include "mbpost.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define FBW 1920
#define FBH 1080
#define STRIDE (FBW * 3)
static uint8_t g_bg[FBH * STRIDE], g_hw[FBH * STRIDE], g_ref[FBH * STRIDE], g_init[FBH * STRIDE];
static int8_t  g_img[ROI_PX * ROI_PX];
static w64     g_imgw[ROI_PX * IMG_WPR];
static int32_t g_out[13];

static void fill(uint8_t *fb, int x, int y, int w, int h, uint32_t c)   // == osd_fill
{
    for (int r = 0; r < h; r++) {
        uint8_t *p = fb + (y + r) * STRIDE + x * 3;
        for (int i = 0; i < w; i++) { *p++ = (c >> 16) & 0xff; *p++ = (c >> 8) & 0xff; *p++ = c & 0xff; }
    }
}
static void restore(uint8_t *fb, int x, int y, int w, int h)       // padded span = background
{
    for (int r = 0; r < h; r++) memcpy(fb + (y + r) * STRIDE + x * 3, g_bg + (y + r) * STRIDE + x * 3, w * 3);
}

// golden: mnist_pre.c draw_live_full (sc=1) + draw_pos_full bands, on top of the padded spans
static void golden(int ox, int oy)
{
    memcpy(g_ref, g_init, sizeof g_ref);
    restore(g_ref, A_X0, A_Y0, A_NG * 8, A_Y1 - A_Y0);
    for (int i = 0; i < 10; i++) restore(g_ref, B_X0, BAR_Y0 + i * BAR_DY + 2, B_NG * 8, BAR_H);
    // preview
    for (int r = 0; r < ROI_PX; r++)
        for (int c = 0; c < ROI_PX; c++) {
            int q = g_img[r * ROI_PX + c];
            uint8_t g = (uint8_t)(q < 0 ? 0 : (q > 127 ? 255 : q * 2));
            uint8_t *p = g_ref + (ROI_Y + r) * STRIDE + (ROI_X + c) * 3;
            p[0] = p[1] = p[2] = g;
        }
    // bars
    int32_t mn = g_out[0], mx = g_out[0], arg = 0;
    for (int i = 1; i < 10; i++) { if (g_out[i] < mn) mn = g_out[i]; if (g_out[i] > mx) { mx = g_out[i]; arg = i; } }
    int32_t rng = mx - mn; if (rng <= 0) rng = 1;
    for (int i = 0; i < 10; i++) {
        int len = (int)(((int64_t)(g_out[i] - mn) * BAR_W) / rng);
        if (len < 0) len = 0; else if (len > BAR_W) len = BAR_W;
        uint8_t Gc = (i == arg) ? 235 : 80;
        int by = BAR_Y0 + i * BAR_DY + 2;
        for (int dy = 0; dy < BAR_H; dy++) {
            uint8_t *p = g_ref + (by + dy) * STRIDE + BAR_X * 3;
            for (int x = 0; x < BAR_W; x++)
                if (x < len) { *p++ = 0; *p++ = Gc; *p++ = Gc; } else { *p++ = 0; *p++ = 0; *p++ = 0; }
        }
    }
    // gauges (draw_pos_full order)
    fill(g_ref, DXB_X0, DXB_Y0, DXB_X1 - DXB_X0, DXB_Y1 - DXB_Y0, C_BLACK);
    fill(g_ref, DYB_X0, DYB_Y0, DYB_X1 - DYB_X0, DYB_Y1 - DYB_Y0, C_BLACK);
    fill(g_ref, ROI_X, DX_TY, ROI_PX, POS_TRK, C_TRACK);
    if (ox) fill(g_ref, ox < 0 ? CX + ox : CX, DX_TY, ox < 0 ? -ox : ox, POS_TRK, C_TEAL);
    fill(g_ref, CX - 1, DX_TY - 4, 2, POS_TRK + 8, C_WHITE);
    fill(g_ref, CX + ox - POS_MK_W/2, DX_TY + POS_TRK/2 - POS_MK_L/2, POS_MK_W, POS_MK_L, C_TEAL);
    fill(g_ref, DY_TX, ROI_Y, POS_TRK, ROI_PX, C_TRACK);
    if (oy) fill(g_ref, DY_TX, oy > 0 ? CY - oy : CY, POS_TRK, oy > 0 ? oy : -oy, C_TEAL);
    fill(g_ref, DY_TX - 4, CY - 1, POS_TRK + 8, 2, C_WHITE);
    fill(g_ref, DY_TX + POS_TRK/2 - POS_MK_L/2, CY - oy - POS_MK_W/2, POS_MK_L, POS_MK_W, C_TEAL);
}

static uint32_t s = 12345;
static int rnd() { s = s * 1103515245u + 12345u; return (int)((s >> 16) & 0x7fff); }

static int run(const char *name, int ox, int oy)
{
    for (int i = 0; i < ROI_PX * ROI_PX; i++) g_img[i] = (int8_t)(rnd() & 0xff);
    for (int i = 0; i < ROI_PX * IMG_WPR; i++) {
        w64 w = 0;
        for (int k = 0; k < 8; k++) w.range(8 * k + 7, 8 * k) = (uint8_t)g_img[i * 8 + k];
        g_imgw[i] = w;
    }
    for (int i = 0; i < FBH * STRIDE; i++) g_init[i] = (uint8_t)rnd();   // stale fb content
    memcpy(g_hw, g_init, sizeof g_hw);
    golden(ox, oy);
    mbpost(g_imgw, g_out, (const w64 *)g_bg, (w64 *)g_hw, STRIDE, ox, oy);
    int e = 0;
    for (int i = 0; i < FBH * STRIDE; i++)
        if (g_hw[i] != g_ref[i]) {
            if (e < 5) printf("  MISMATCH byte %d (x %d y %d c %d) hw=%d ref=%d\n", i,
                              (i % STRIDE) / 3, i / STRIDE, i % 3, g_hw[i], g_ref[i]);
            e++;
        }
    printf("  %-28s ox=%4d oy=%4d: %s (%d mismatches)\n", name, ox, oy, e ? "FAIL" : "PASS", e);
    return e;
}

int main()
{
    for (int i = 0; i < FBH * STRIDE; i++) g_bg[i] = (uint8_t)rnd();
    int e = 0;
    for (int i = 0; i < 10; i++) g_out[i] = (rnd() << 10) - (1 << 24);
    e += run("random logits", 0, 0);
    e += run("random logits", 23, -17);
    e += run("random logits", -56, 56);
    e += run("random logits", 56, -56);
    for (int i = 0; i < 10; i++) g_out[i] = 1000;
    e += run("all-equal logits", 5, 5);
    for (int i = 0; i < 10; i++) g_out[i] = (i == 3 || i == 7) ? 500 : -200;
    e += run("tied max (first wins)", -1, 1);
    for (int i = 0; i < 10; i++) g_out[i] = i * 7;
    e += run("small range", 1, -1);
    printf("mbpost C-sim: %s\n", e ? "FAIL" : "PASS");
    return e ? 1 : 0;
}
