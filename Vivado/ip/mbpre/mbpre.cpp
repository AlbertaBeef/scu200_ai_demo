// mbpre.cpp — SCU200 preprocessing coprocessor: camera 640x480 RGB8 (mono, R=G=B)
// -> 28x28 INT8 [0,127], matching mnist_pre.c exactly, offloaded from the MicroBlaze.
// SPDX-License-Identifier: Apache-2.0
//
// PROGRAMMABLE square centre crop: top-left (x0,y0) and a power-of-2 block size
// blk = 1<<blk_shift give a crop of (28*blk)x(28*blk); each blk x blk block box-averages
// to one of the 28x28 output pixels. Take luma (mono capture is R=G=B, so the green byte
// == luma), quantize 0..255 -> 0..127 (>>1), optional intensity invert (127-q for
// dark-digit-on-light-paper). One linear, row-sequential sweep of the crop so the m_axi
// reads stream instead of scatter.
//
// blk_shift range 0..4 -> blk 1..16 -> crop 28..448 (448 is the largest square that fits
// the 480-line frame). Software must keep x0+crop <= 640 and y0+crop <= 480.
//
// Interface (mirrors mbdpu01): m_axi 'gmem' for cam + img, s_axilite 'control'.

#include <stdint.h>
#include <ap_int.h>

#define FW    640
#define FH    480
#define DIM   28
#define MAX_SHIFT 4          // blk<=16, crop<=448

void mbpre(const uint8_t *cam, uint32_t stride, uint32_t invert,
           uint32_t x0, uint32_t y0, uint32_t blk_shift, int8_t *img)
{
#pragma HLS INTERFACE m_axi     port=cam    offset=slave bundle=gmem depth=921600
#pragma HLS INTERFACE m_axi     port=img    offset=slave bundle=gmem depth=784
#pragma HLS INTERFACE s_axilite port=cam        bundle=control
#pragma HLS INTERFACE s_axilite port=stride     bundle=control
#pragma HLS INTERFACE s_axilite port=invert     bundle=control
#pragma HLS INTERFACE s_axilite port=x0         bundle=control
#pragma HLS INTERFACE s_axilite port=y0         bundle=control
#pragma HLS INTERFACE s_axilite port=blk_shift  bundle=control
#pragma HLS INTERFACE s_axilite port=img        bundle=control
#pragma HLS INTERFACE s_axilite port=return     bundle=control

    uint32_t bs = blk_shift;
    if (bs > MAX_SHIFT) bs = MAX_SHIFT;                 // clamp so crop fits the frame
    const uint32_t crop      = (uint32_t)DIM << bs;     // 28*blk
    const uint32_t band_mask = ((uint32_t)1 << bs) - 1; // blk-1
    const uint32_t avg_shift = bs << 1;                 // /(blk*blk) = >> (2*bs)

    uint32_t acc[DIM];
#pragma HLS ARRAY_PARTITION variable=acc complete dim=1
    for (int i = 0; i < DIM; i++) acc[i] = 0;

    for (uint32_t cy = 0; cy < crop; cy++) {
        uint32_t oy   = cy >> bs;                        // output row 0..27
        uint32_t base = (y0 + cy) * stride + x0 * 3u;
        for (uint32_t cx = 0; cx < crop; cx++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=28 max=448
            uint8_t g = cam[base + cx * 3u + 1u];        // green == luma (mono R=G=B)
            acc[cx >> bs] += g;                          // cx/blk = output column
        }
        if ((cy & band_mask) == band_mask) {             // end of a blk-row band
            for (int ox = 0; ox < DIM; ox++) {
#pragma HLS PIPELINE II=1
                uint32_t gray = acc[ox] >> avg_shift;    // blk*blk box average
                int q = (int)(gray >> 1);                // 0..255 -> 0..127
                if (invert) q = 127 - q;
                img[oy * DIM + ox] = (int8_t)q;
                acc[ox] = 0;
            }
        }
    }
}

#ifndef __SYNTHESIS__
#include <cstdio>
#include <cstdlib>
static uint8_t g_cam[FH * FW * 3];
static int8_t  g_hw[DIM * DIM], g_ref[DIM * DIM];

// software golden == mnist_downscale_rgb8 (mono luma path), parameterised by crop
static void golden(uint32_t stride, int invert, uint32_t x0, uint32_t y0, uint32_t bs)
{
    uint32_t blk = 1u << bs;
    for (uint32_t oy = 0; oy < DIM; oy++)
        for (uint32_t ox = 0; ox < DIM; ox++) {
            uint32_t sum = 0;
            for (uint32_t by = 0; by < blk; by++)
                for (uint32_t bx = 0; bx < blk; bx++) {
                    const uint8_t *p = g_cam + (uint32_t)(y0 + oy*blk + by)*stride
                                             + (uint32_t)(x0 + ox*blk + bx)*3u;
                    uint8_t r = p[0], gg = p[1], b = p[2];
                    sum += ((uint32_t)r*77u + (uint32_t)gg*150u + (uint32_t)b*29u) >> 8;
                }
            uint32_t gray = sum / (blk*blk);
            int q = (int)(gray >> 1);
            if (invert) q = 127 - q;
            g_ref[oy*DIM + ox] = (int8_t)q;
        }
}

static int run(uint32_t stride, int invert, uint32_t x0, uint32_t y0, uint32_t bs)
{
    golden(stride, invert, x0, y0, bs);
    mbpre(g_cam, stride, invert, x0, y0, bs, g_hw);
    int e = 0;
    for (int i = 0; i < DIM*DIM; i++)
        if (g_hw[i] != g_ref[i]) { if (e<5) printf("  MISMATCH %d hw=%d ref=%d\n", i, g_hw[i], g_ref[i]); e++; }
    printf("  crop blk=%u (x0=%u y0=%u): %s (%d mismatches)\n",
           (unsigned)(1u<<bs), (unsigned)x0, (unsigned)y0, e ? "FAIL" : "PASS", e);
    return e;
}

int main()
{
    uint32_t s = 777, stride = FW * 3;
    #define RND ((s = s*1103515245u + 12345u), (int)((s >> 16) & 0xff))
    for (int i = 0; i < FH*FW; i++) { int v = RND; g_cam[i*3]=v; g_cam[i*3+1]=v; g_cam[i*3+2]=v; } // mono
    int e = 0;
    e += run(stride, 1, 264, 184, 2);   // 112 centre crop (blk 4) — the current default
    e += run(stride, 1,  96,  16, 4);   // 448 centre crop (blk 16) — the old hardwired case
    e += run(stride, 0, 306, 226, 1);   //  56 crop (blk 2), no invert
    e += run(stride, 1, 100, 100, 0);   //  28 crop (blk 1), 1:1 no averaging
    printf("mbpre C-sim: %s\n", e ? "FAIL" : "PASS");
    return e ? 1 : 0;
}
#endif
