// mbdpu03.cpp — SCU200 AI demo custom DPU: 112x112 mono INT8 MNIST CNN + digit-position head,
// with the camera crop/zoom (mbpre's job) fused in and the big buffers in UltraRAM.
// SPDX-License-Identifier: Apache-2.0
//
// Network (symmetric per-layer INT8; INT32 accumulate; TFLite-style requant, as mbdpu01/02):
//   in 112x112x1 (0..127)
//   -> conv1 1->8   3x3 pad1 +b ReLU requant, maxpool2 -> 56x56x8
//   -> conv2 8->16  3x3 pad1 +b ReLU requant, maxpool2 -> 28x28x16
//   -> conv3 16->32 3x3 pad1 +b ReLU requant, maxpool2 -> 14x14x32
//   -> conv4 32->32 3x3 pad1 +b ReLU requant, maxpool2 ->  7x7x32
//   -> FC 1568->12 +b (raw INT32): out[0..9] logits, out[10] argmax, out[11] dx, out[12] dy
// Firmware converts the position accumulators with px*100 = (acc * pblob[108]) >> pblob[109]
// (112-px units; dx left -ve / right +ve, dy top +ve / bottom -ve).
//
// Pooling is done on the raw accumulators and requantised once per pooled output. requant() is
// monotonic non-decreasing (M0 > 0), so max(requant(a)) == requant(max(a)): bit-exact with the
// trainer, which requantises first and pools after.
//
// Datapath:
//   input   DATAFLOW with conv1, three processes linked by FIFOs:
//           rd_rows  bursts each crop row (64-bit words) from the camera frame into a FIFO; it
//                    runs ahead of the extractor, so row n+1 is fetched while row n is used;
//           extract  takes the G byte (= luma, mono R=G=B), box-averages blk x blk (blk =
//                    1<<blk_shift, crop 112/224/448), >>1 to 0..127, optional invert, and
//                    streams 112x112 pixels on (plus a copy for the DDR preview write-back);
//           conv1s   starts as soon as pixels arrive.
//           With CTRL_SRC_IMG the 112x112 input is read from img instead (testbench / self-test).
//   conv1   one pixel per clock: 3x3 window from two line buffers, 8 oc x 9 taps in parallel.
//   conv2-4 ONE shared engine (ALLOCATION limit=1): LANES=16 output lanes x 8 input channels
//           (one 64-bit word) per clock = 128 MAC/clk (2 MACs per DSP). conv3/conv4 (32 oc) run
//           two 16-oc groups per pooled pixel. (32 lanes fit the core but pushed the full
//           demo03 design to 94% LUTs and failed timing in the DDR/MIPI domains.)
//   fc      8 MAC/clk (one feature word per clock).
//
// Storage: weights + params load from DDR only when CTRL_RELOAD is set (static on-chip stores
// survive between calls). Feature maps ping-pong between uram_fm0 / uram_fm1 (4096 x 64 each,
// 8 channels per word, [h][w][channel-group] order); FC weights live in uram_fcw. Vitis HLS
// 2025.2 does not offer impl=uram on the beta xcsu200p, so these are generated as BRAM and the
// exported RTL of every uram_* memory is patched to UltraRAM by run_hls.tcl (patch_uram.py).

#include "mbdpu03.h"
#include <hls_stream.h>

#define FM_N   4096                 // words per feature-map bank (p1 = 56*56*1 = 3136 max)
#define IMG_W  (IMG_N/8)            // 1568 64-bit words in the 112x112 input image

// ------------------------------------------------------------------------------- helpers
static i8 requant(i32 acc, i32 bias, i32 M0, int shift, bool relu)
{
#pragma HLS INLINE
    ap_int<27> s    = (ap_int<27>)(acc + bias);   // accumulator + bias (|.| < 2^24)
    ap_int<18> m    = (ap_int<18>)M0;             // requant multiplier (|.| <= 2^17)
    ap_int<45> prod = s * m;                       // 27x18 -> single DSP48
    ap_int<45> rnd  = (shift > 0) ? (ap_int<45>(1) << (shift - 1)) : (ap_int<45>)0;
    int32_t r = (int32_t)((prod + rnd) >> shift);
    if (relu && r < 0) r = 0;
    if (r > 127)  r = 127;
    if (r < -127) r = -127;
    return (i8)r;
}

// Two int8 MACs per DSP48E2 (shared operand x): A = (wb << 18) + wa, P = A * x =
// x*wa + (x*wb << 18). Summed over n products, the total is exactly Sa + (Sb << 18); Sa fits the
// low 18 bits as long as |Sa| < 2^17 (8 x 127 x 127 = 129032 OK; 9 taps is not, so conv1 splits).
static void dot_pair(const ap_int<8> x[], const ap_int<8> wa[], const ap_int<8> wb[], int n,
                     i32 &sa, i32 &sb)
{
#pragma HLS INLINE
    ap_int<48> tot = 0;
    for (int k = 0; k < n; k++) {
#pragma HLS UNROLL
        ap_int<27> A = ((ap_int<27>)wb[k] << 18) + (ap_int<27>)wa[k];
        ap_int<45> p = A * (ap_int<18>)x[k];
#pragma HLS BIND_OP variable=p op=mul impl=dsp
        tot += p;
    }
    ap_int<18> lo = tot.range(17, 0);
    sa = (i32)lo;
    sb = (i32)((tot - (ap_int<48>)lo) >> 18);
}

static void dot8x2(w64 x, w64 wa, w64 wb, i32 &sa, i32 &sb)
{
#pragma HLS INLINE
    ap_int<8> xv[WIC], av[WIC], bv[WIC];
#pragma HLS ARRAY_PARTITION variable=xv complete
#pragma HLS ARRAY_PARTITION variable=av complete
#pragma HLS ARRAY_PARTITION variable=bv complete
    for (int k = 0; k < WIC; k++) {
#pragma HLS UNROLL
        xv[k] = x.range(8 * k + 7, 8 * k);
        av[k] = wa.range(8 * k + 7, 8 * k);
        bv[k] = wb.range(8 * k + 7, 8 * k);
    }
    dot_pair(xv, av, bv, WIC, sa, sb);
}

static inline i32 imax(i32 a, i32 b) { return a > b ? a : b; }

// ------------------------------------------------------------------------------- weights
static void load_weights(const i8* wblob, const i32* pblob,
                         i8 c1w[C1][9], i32 c1b[C1], w64 lw[LANES][LW_N], i32 lb[LANES][LB_N],
                         w64 uram_fcw[FC_OUT * FC_WORDS], i32 fcb[FC_OUT], i32 Mq[4], i32 Sq[4])
{
    for (int i = 0; i < C1 * 9; i++) c1w[i / 9][i % 9] = wblob[OFF_C1W + i];
    for (int i = 0; i < C1; i++) c1b[i] = pblob[OFF_C1B + i];
    for (int l = 0; l < LANES; l++) {
        lb[l][0] = pblob[OFF_C2B + l];
        lb[l][1] = pblob[OFF_C3B + l];
        lb[l][2] = pblob[OFF_C3B + LANES + l];
        lb[l][3] = pblob[OFF_C4B + l];
        lb[l][4] = pblob[OFF_C4B + LANES + l];
    }
    for (int i = 0; i < FC_CLS; i++) fcb[i] = pblob[OFF_FCB + i];
    for (int i = 0; i < FC_POS; i++) fcb[FC_CLS + i] = pblob[OFF_POSB + i];
    for (int i = 0; i < 4; i++) { Mq[i] = pblob[OFF_M + 2 * i]; Sq[i] = pblob[OFF_M + 2 * i + 1]; }

    // conv2..conv4 -> per-lane word stores: word (woff + (ocg*9 + tap)*ICG + g) of lane l packs
    // ic = g*8..g*8+7 of oc = ocg*16 + l. Each word is assembled from 8 (strided) blob bytes,
    // then written once: no read-modify-write over the lane banks. Reload-only path, so the
    // scattered DDR reads do not matter.
    for (int L = 0; L < 3; L++) {
        const int IC   = (L == 0) ? C1 : (L == 1) ? C2 : C3;
        const int OC   = (L == 0) ? C2 : (L == 1) ? C3 : C4;
        const int off  = (L == 0) ? OFF_C2W : (L == 1) ? OFF_C3W : OFF_C4W;
        const int lwo  = (L == 0) ? LW_OFF2 : (L == 1) ? LW_OFF3 : LW_OFF4;
        const int ICG  = IC / WIC;
        for (int og = 0; og < OC / LANES; og++)
        for (int l = 0; l < LANES; l++) {
            const int oc = og * LANES + l;
            const int woff = lwo + og * 9 * ICG;
            for (int t = 0; t < 9; t++) {
                for (int g = 0; g < ICG; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=4
                    w64 w = 0;
                    for (int k = 0; k < WIC; k++) {
#pragma HLS PIPELINE II=1
                        const int ic = g * WIC + k;
                        w.range(8 * k + 7, 8 * k) = (ap_uint<8>)(uint8_t)wblob[off + (oc * IC + ic) * 9 + t];
                    }
                    lw[l][woff + t * ICG + g] = w;
                }
            }
        }
    }

    // FC: row-major [row][1568] bytes -> uram_fcw[row*196 + idx/8], byte idx%8 (8 bytes per word)
    w64 w = 0;
    for (int i = 0; i < FC_OUT * FC_IN; i++) {
#pragma HLS PIPELINE II=1
        int k = i & 7;
        w.range(8 * k + 7, 8 * k) = (ap_uint<8>)(uint8_t)wblob[OFF_FCW + i];
        if (k == 7) uram_fcw[i >> 3] = w;
    }
}

// ------------------------------------------------------------------------------- input
// Row geometry shared by the reader and the extractor: first word, byte offset, word count.
static void row_geom(uint32_t cy, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t crop,
                     bool src_img, uint32_t& w0, uint32_t& off, uint32_t& nw)
{
#pragma HLS INLINE
    if (src_img) { w0 = cy * (IN_DIM / 8); off = 0; nw = IN_DIM / 8; return; }
    const uint32_t start = (y0 + cy) * stride + x0 * 3u;
    w0 = start >> 3; off = start & 7u; nw = (off + crop * 3u + 7u) >> 3;
}

// Stage A: one burst per crop row into the word FIFO.
static void rd_rows(const w64* cam, const w64* img, uint32_t stride, uint32_t x0, uint32_t y0,
                    uint32_t bs, bool src_img, hls::stream<w64>& ws)
{
    const uint32_t crop = (uint32_t)IN_DIM << bs;
    for (uint32_t cy = 0; cy < crop; cy++) {
#pragma HLS LOOP_TRIPCOUNT min=112 max=448
        uint32_t w0, off, nw;
        row_geom(cy, stride, x0, y0, crop, src_img, w0, off, nw);
        if (src_img) {
            for (uint32_t j = 0; j < nw; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=14 max=14
                ws.write(img[w0 + j]);
            }
        } else {
            for (uint32_t j = 0; j < nw; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=42 max=169
                ws.write(cam[w0 + j]);
            }
        }
    }
}

// Stage B: one crop pixel per clock. Each crop pixel needs at most one new word (3 bytes per
// pixel < 8 per word), so the word FIFO is consumed on the fly; leftover words are drained.
static void extract(uint32_t stride, uint32_t x0, uint32_t y0, uint32_t bs, bool src_img,
                    bool inv, hls::stream<w64>& ws, hls::stream<i8>& ps, i8 imgbuf[IMG_N])
{
    const uint32_t crop = (uint32_t)IN_DIM << bs;
    const uint32_t mask = (1u << bs) - 1u;
    const uint32_t ash  = bs << 1;                       // / (blk*blk)
    const uint32_t step = src_img ? 1u : 3u;             // bytes per crop pixel
    const uint32_t pofs = src_img ? 0u : 1u;             // green byte of RGB8

    uint32_t acc[IN_DIM];
    int oy = 0;
    for (uint32_t cy = 0; cy < crop; cy++) {
#pragma HLS LOOP_TRIPCOUNT min=112 max=448
        uint32_t w0, off, nw;
        row_geom(cy, stride, x0, y0, crop, src_img, w0, off, nw);
        w64 cur = ws.read();
        uint32_t have = 1;                               // words taken from this row so far
        if (((off + pofs) >> 3) != 0) { cur = ws.read(); have = 2; }
        const bool first = (cy & mask) == 0, last = (cy & mask) == mask;
        uint32_t run = 0;
        for (uint32_t cx = 0; cx < crop; cx++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=112 max=448
#pragma HLS DEPENDENCE variable=acc inter false
            const uint32_t b = off + cx * step + pofs;
            if ((b >> 3) >= have) { cur = ws.read(); ++have; }
            const uint32_t g = cur.range(8 * (b & 7) + 7, 8 * (b & 7));
            run += g;
            if ((cx & mask) == mask) {
                const uint32_t ox  = cx >> bs;
                const uint32_t tot = (first ? 0u : acc[ox]) + run;
                run = 0;
                if (last) {
                    i8 q;
                    if (src_img) q = (i8)g;
                    else {
                        int v = (int)((tot >> ash) >> 1);  // box average, 0..255 -> 0..127
                        if (inv) v = 127 - v;
                        q = (i8)v;
                    }
                    ps.write(q);
                    imgbuf[oy * IN_DIM + (int)ox] = q;
                } else {
                    acc[ox] = tot;
                }
            }
        }
        for (; have < nw; ++have) {                      // unused tail word(s) of this row
#pragma HLS LOOP_TRIPCOUNT min=0 max=1
            ws.read();
        }
        if (last) ++oy;
    }
}

// ------------------------------------------------------------------------------- conv1
// Stage C: one step per clock over input rows ir = 0..112 and columns c = 0..112 (one flush
// row/column of zero padding). Two line buffers hold rows ir-2, ir-1; with the incoming pixel
// they form window column c of conv row r = ir-1. The conv output for column c-1 is formed and
// 2x2 max-pooled on the fly, so conv1 trails the pixel stream by one row.
static void conv1s(hls::stream<i8>& ps, const i8 c1w[C1][9], const i32 c1b[C1], i32 M0, i32 sh,
                   w64 dst[FM_N])
{
    i8  lb0[IN_DIM], lb1[IN_DIM];
    i8  win[3][3];
#pragma HLS ARRAY_PARTITION variable=win complete dim=0
    i32 colmax[C1];
#pragma HLS ARRAY_PARTITION variable=colmax complete
    i32 rowbuf[P1][C1];
#pragma HLS ARRAY_PARTITION variable=rowbuf complete dim=2

    int ir = 0, c = 0;
    for (int i = 0; i < (IN_DIM + 1) * (IN_DIM + 1); i++) {
#pragma HLS PIPELINE II=1
        const int r = ir - 1;
        i8 col[3] = {0, 0, 0};
#pragma HLS ARRAY_PARTITION variable=col complete
        if (c < IN_DIM) {
            const i8 a = (ir >= 2) ? lb0[c] : (i8)0;     // row ir-2
            const i8 m = (ir >= 1) ? lb1[c] : (i8)0;     // row ir-1
            i8 p = 0;
            if (ir < IN_DIM) p = ps.read();              // row ir (row 112 = padding)
            lb0[c] = m; lb1[c] = p;
            col[0] = a; col[1] = m; col[2] = p;
        }
        for (int k = 0; k < 3; k++) {
#pragma HLS UNROLL
            if (c == 0) { win[k][0] = 0; win[k][1] = 0; }
            else        { win[k][0] = win[k][1]; win[k][1] = win[k][2]; }
            win[k][2] = col[k];
        }
        if (r >= 0 && c >= 1) {
            const int cc = c - 1;
            w64 word = 0;
            ap_int<8> xv[9];
#pragma HLS ARRAY_PARTITION variable=xv complete
            for (int t = 0; t < 9; t++) {
#pragma HLS UNROLL
                xv[t] = win[t / 3][t % 3];
            }
            i32 accs[C1];
#pragma HLS ARRAY_PARTITION variable=accs complete
            for (int oc = 0; oc < C1; oc += 2) {
#pragma HLS UNROLL
                ap_int<8> wa[9], wb[9];
#pragma HLS ARRAY_PARTITION variable=wa complete
#pragma HLS ARRAY_PARTITION variable=wb complete
                for (int t = 0; t < 9; t++) {
#pragma HLS UNROLL
                    wa[t] = c1w[oc][t]; wb[t] = c1w[oc + 1][t];
                }
                i32 a0, b0, a1, b1;
                dot_pair(xv, wa, wb, 5, a0, b0);              // taps 0..4
                dot_pair(xv + 5, wa + 5, wb + 5, 4, a1, b1);  // taps 5..8 (keeps |sum| < 2^17)
                accs[oc] = a0 + a1; accs[oc + 1] = b0 + b1;
            }
            for (int oc = 0; oc < C1; oc++) {
#pragma HLS UNROLL
                const i32 acc = accs[oc];
                if ((cc & 1) == 0) {
                    colmax[oc] = acc;
                } else {
                    i32 cm = imax(colmax[oc], acc);
                    if ((r & 1) == 0) rowbuf[cc >> 1][oc] = cm;
                    else {
                        i8 q = requant(imax(rowbuf[cc >> 1][oc], cm), c1b[oc], M0, sh, true);
                        word.range(8 * oc + 7, 8 * oc) = (ap_uint<8>)(uint8_t)q;
                    }
                }
            }
            if ((cc & 1) && (r & 1)) dst[(r >> 1) * P1 + (cc >> 1)] = word;
        }
        if (++c == IN_DIM + 1) { c = 0; ++ir; }
    }
}

// Input + conv1 as one dataflow region: the camera read overlaps conv1.
static void front(const w64* cam, const w64* img, uint32_t stride, uint32_t x0, uint32_t y0,
                  uint32_t bs, bool src_img, bool inv, const i8 c1w[C1][9], const i32 c1b[C1],
                  i32 M0, i32 sh, w64 dst[FM_N], i8 imgbuf[IMG_N])
{
#pragma HLS DATAFLOW
    hls::stream<w64> ws("ws");
#pragma HLS STREAM variable=ws depth=512
    hls::stream<i8> ps("ps");
#pragma HLS STREAM variable=ps depth=256
    rd_rows(cam, img, stride, x0, y0, bs, src_img, ws);
    extract(stride, x0, y0, bs, src_img, inv, ws, ps, imgbuf);
    conv1s(ps, c1w, c1b, M0, sh, dst);
}

// ------------------------------------------------------------------------------- conv2..4
// Shared engine. Input map src: H x H x (ICG*8) as words [(h*H + w)*ICG + g]. Iterates pooled
// outputs (ph,pw) -> oc groups (OCG x 16 oc) -> conv positions in the 2x2 window -> 9 taps ->
// ICG channel words, one word per clock into all LANES lanes. Each finished group yields two
// output words, queued and written one per clock while the next group computes. Output map:
// PH x PH x (OCG*16) as words [(ph*PH + pw)*OCG*2 + ocg*2 + k].
static void engine(const w64 src[FM_N], w64 dst[FM_N], const w64 lw[LANES][LW_N],
                   const i32 lb[LANES][LB_N], int H, int ICG, int OCG, int woff,
                   i32 M0, i32 sh, int lbase)
{
    const int PH    = H >> 1;
    const int NPOS  = 4;
    const int TOTAL = PH * PH * OCG * NPOS * 9 * ICG;

    i32 acc[LANES], mx[LANES];
#pragma HLS ARRAY_PARTITION variable=acc complete
#pragma HLS ARRAY_PARTITION variable=mx complete
    w64 pend[2];
#pragma HLS ARRAY_PARTITION variable=pend complete
    for (int l = 0; l < LANES; l++) {
#pragma HLS UNROLL
        acc[l] = 0; mx[l] = -2147483647;
    }
    int npend = 0, paddr = 0;
    int ph = 0, pw = 0, og = 0, p = 0, t = 0, kh = 0, kw = 0, g = 0;

    for (int i = 0; i < TOTAL; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=14112 max=28224
        const int r  = 2 * ph + (p >> 1) + kh - 1;
        const int c  = 2 * pw + (p & 1) + kw - 1;
        const bool v = (r >= 0) && (r < H) && (c >= 0) && (c < H);
        const w64 x  = v ? src[(r * H + c) * ICG + g] : (w64)0;
        const int widx = woff + (og * 9 + t) * ICG + g;
        const bool last_tap = (t == 8) && (g == ICG - 1);
        const bool last_pos = last_tap && (p == NPOS - 1);

        for (int l = 0; l < LANES; l += 2) {
#pragma HLS UNROLL
            i32 s0, s1;
            dot8x2(x, lw[l][widx], lw[l + 1][widx], s0, s1);
            const i32 a0 = acc[l] + s0, a1 = acc[l + 1] + s1;
            if (last_tap) { mx[l] = imax(mx[l], a0); mx[l + 1] = imax(mx[l + 1], a1); acc[l] = 0; acc[l + 1] = 0; }
            else          { acc[l] = a0; acc[l + 1] = a1; }
        }

        if (last_pos) {
            i8 q[LANES];
#pragma HLS ARRAY_PARTITION variable=q complete
            for (int l = 0; l < LANES; l++) {
#pragma HLS UNROLL
                q[l] = requant(mx[l], lb[l][lbase + og], M0, sh, true);
                mx[l] = -2147483647;
            }
            for (int k = 0; k < 2; k++) {
#pragma HLS UNROLL
                w64 w = 0;
                for (int b = 0; b < WIC; b++) {
#pragma HLS UNROLL
                    w.range(8 * b + 7, 8 * b) = (ap_uint<8>)(uint8_t)q[k * WIC + b];
                }
                pend[k] = w;
            }
            npend = 2;
            paddr = ((ph * PH + pw) * OCG + og) * 2;
        } else if (npend > 0) {
            dst[paddr] = pend[0];
            pend[0] = pend[1];
            ++paddr; --npend;
        }

        // counters: g -> (kh,kw)/t -> p -> og -> pw -> ph
        if (++g == ICG) {
            g = 0;
            if (++kw == 3) { kw = 0; ++kh; }
            if (++t == 9) {
                t = 0; kh = 0; kw = 0;
                if (++p == NPOS) {
                    p = 0;
                    if (++og == OCG) {
                        og = 0;
                        if (++pw == PH) { pw = 0; ++ph; }
                    }
                }
            }
        }
    }
    for (int k = 0; k < 2; k++) {
#pragma HLS PIPELINE II=1
        if (k < npend) dst[paddr + k] = pend[k];
    }
}

// ------------------------------------------------------------------------------- fc
static void fc(const w64 src[FM_N], const w64 uram_fcw[FC_OUT * FC_WORDS], const i32 fcb[FC_OUT],
               i32* out)
{
    // two rows per clock (rows 2rp, 2rp+1 share the feature word -> packed DSP MACs)
    i32 res[FC_OUT];
#pragma HLS ARRAY_PARTITION variable=res complete
    i32 acc0 = 0, acc1 = 0;
    int rp = 0, wd = 0;
    for (int i = 0; i < (FC_OUT / 2) * FC_WORDS; i++) {
#pragma HLS PIPELINE II=1
        i32 s0, s1;
        dot8x2(src[wd], uram_fcw[(2 * rp) * FC_WORDS + wd], uram_fcw[(2 * rp + 1) * FC_WORDS + wd], s0, s1);
        const i32 a0 = ((wd == 0) ? fcb[2 * rp] : acc0) + s0;
        const i32 a1 = ((wd == 0) ? fcb[2 * rp + 1] : acc1) + s1;
        acc0 = a0; acc1 = a1;
        if (wd == FC_WORDS - 1) {
            res[2 * rp] = a0; res[2 * rp + 1] = a1;
            wd = 0; ++rp;
        } else {
            ++wd;
        }
    }
    int best = 0; i32 bestv = res[0];
    for (int o = 0; o < FC_OUT; o++) {
#pragma HLS UNROLL
        if (o < FC_CLS) {
            out[o] = res[o];
            if (res[o] > bestv) { bestv = res[o]; best = o; }
        } else {
            out[o + 1] = res[o];         // skip the argmax slot
        }
    }
    out[FC_CLS] = best;
}

// ------------------------------------------------------------------------------- top
void mbdpu03(const i8* wblob, const i32* pblob, w64* img, i32* out,
             const w64* cam, uint32_t stride, uint32_t x0, uint32_t y0,
             uint32_t blk_shift, uint32_t ctrl)
{
#pragma HLS INTERFACE m_axi     port=wblob offset=slave bundle=gmem depth=33864
#pragma HLS INTERFACE m_axi     port=pblob offset=slave bundle=gmem depth=110
#pragma HLS INTERFACE m_axi     port=img   offset=slave bundle=gmem depth=1568
#pragma HLS INTERFACE m_axi     port=out   offset=slave bundle=gmem depth=13
#pragma HLS INTERFACE m_axi     port=cam   offset=slave bundle=gmem depth=115200 max_read_burst_length=256
#pragma HLS INTERFACE s_axilite port=wblob     bundle=control
#pragma HLS INTERFACE s_axilite port=pblob     bundle=control
#pragma HLS INTERFACE s_axilite port=img       bundle=control
#pragma HLS INTERFACE s_axilite port=out       bundle=control
#pragma HLS INTERFACE s_axilite port=cam       bundle=control
#pragma HLS INTERFACE s_axilite port=stride    bundle=control
#pragma HLS INTERFACE s_axilite port=x0        bundle=control
#pragma HLS INTERFACE s_axilite port=y0        bundle=control
#pragma HLS INTERFACE s_axilite port=blk_shift bundle=control
#pragma HLS INTERFACE s_axilite port=ctrl      bundle=control
#pragma HLS INTERFACE s_axilite port=return    bundle=control
#pragma HLS ALLOCATION function instances=engine limit=1

    // ---- persistent on-chip stores (loaded on CTRL_RELOAD) ----
    static i8  c1w[C1][9];
#pragma HLS ARRAY_PARTITION variable=c1w complete dim=0
    static i32 c1b[C1];
#pragma HLS ARRAY_PARTITION variable=c1b complete
    static w64 lw[LANES][LW_N];
#pragma HLS ARRAY_PARTITION variable=lw complete dim=1
    static i32 lb[LANES][LB_N];
#pragma HLS ARRAY_PARTITION variable=lb complete dim=1
    static w64 uram_fcw[FC_OUT * FC_WORDS];
#pragma HLS bind_storage variable=uram_fcw type=ram_2p impl=bram
    static i32 fcb[FC_OUT];
#pragma HLS ARRAY_PARTITION variable=fcb complete
    static i32 Mq[4], Sq[4];
#pragma HLS ARRAY_PARTITION variable=Mq complete
#pragma HLS ARRAY_PARTITION variable=Sq complete

    // ---- per-call buffers ----
    static i8  imgbuf[IMG_N];
#pragma HLS ARRAY_PARTITION variable=imgbuf cyclic factor=8
    static w64 uram_fm0[FM_N], uram_fm1[FM_N];
#pragma HLS bind_storage variable=uram_fm0 type=ram_2p impl=bram
#pragma HLS bind_storage variable=uram_fm1 type=ram_2p impl=bram

    if (ctrl & CTRL_RELOAD)
        load_weights(wblob, pblob, c1w, c1b, lw, lb, uram_fcw, fcb, Mq, Sq);

    const bool src_img = (ctrl & CTRL_SRC_IMG) != 0;
    uint32_t bs = blk_shift > BLK_SHIFT_MAX ? (uint32_t)BLK_SHIFT_MAX : blk_shift;
    if (src_img) bs = 0;
    front(cam, img, stride, x0, y0, bs, src_img, (ctrl & CTRL_INVERT) != 0, c1w, c1b,
          Mq[0], Sq[0], uram_fm0, imgbuf);
    if (!src_img) {                                       // 112x112 input -> DDR, for the preview
        for (int i = 0; i < IMG_W; i++) {
#pragma HLS PIPELINE II=1
            w64 w;
            for (int k = 0; k < 8; k++) {
#pragma HLS UNROLL
                w.range(8 * k + 7, 8 * k) = (ap_uint<8>)(uint8_t)imgbuf[i * 8 + k];
            }
            img[i] = w;
        }
    }
    engine(uram_fm0, uram_fm1, lw, lb, P1, C1 / WIC, C2 / LANES, LW_OFF2,
           Mq[1], Sq[1], 0);                                               // 56x56x8  -> 28x28x16
    engine(uram_fm1, uram_fm0, lw, lb, P2, C2 / WIC, C3 / LANES, LW_OFF3,
           Mq[2], Sq[2], 1);                                               // 28x28x16 -> 14x14x32
    engine(uram_fm0, uram_fm1, lw, lb, P3, C3 / WIC, C4 / LANES, LW_OFF4,
           Mq[3], Sq[3], 3);                                               // 14x14x32 -> 7x7x32
    fc(uram_fm1, uram_fcw, fcb, out);
}
