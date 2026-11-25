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
//   input   camera frame (RGB8, mono R=G=B) -> crop (x0,y0), zoom blk = 1<<blk_shift (crop 112/
//           224/448, blk x blk box average), luma = G byte, >>1 to 0..127, optional invert. The
//           112x112 result is written back to img (for the HDMI preview / gallery). With
//           CTRL_SRC_IMG the input is read from img instead (testbench / self-test).
//   conv1   one pixel per clock: 3x3 window from 3 row-banks, 8 oc x 9 taps in parallel.
//   conv2-4 ONE shared engine (ALLOCATION limit=1): LANES=32 output lanes x 8 input channels
//           (one 64-bit word) per clock = 256 MAC/clk. For conv2 (16 oc) lanes 16..31 compute
//           the neighbouring conv column of the same 2x2 pool window, so the array stays full.
//   fc      8 MAC/clk (one feature word per clock).
//
// Storage: weights + params load from DDR only when CTRL_RELOAD is set (static on-chip stores
// survive between calls). Feature maps ping-pong between uram_fm0 / uram_fm1 (4096 x 64 each,
// 8 channels per word, [h][w][channel-group] order); FC weights live in uram_fcw. Vitis HLS
// 2025.2 does not offer impl=uram on the beta xcsu200p, so these are generated as BRAM and the
// exported RTL of every uram_* memory is patched to UltraRAM by run_hls.tcl (patch_uram.py).

#include "mbdpu03.h"

#define FM_N   4096                 // words per feature-map bank (p1 = 56*56*1 = 3136 max)
#define XB_N   (38*IN_DIM)          // bytes per input row-bank (row r -> bank r%3, row r/3)
#define ROWW_N 170                  // camera row words (448*3 B + 7 B misalignment) / 8

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

static void xin_write(i8 xb0[XB_N], i8 xb1[XB_N], i8 xb2[XB_N], int bank, int addr, i8 v)
{
#pragma HLS INLINE
    if (bank == 0) xb0[addr] = v; else if (bank == 1) xb1[addr] = v; else xb2[addr] = v;
}

// ------------------------------------------------------------------------------- weights
static void load_weights(const i8* wblob, const i32* pblob,
                         i8 c1w[C1][9], i32 c1b[C1], w64 lw[LANES][LW_N], i32 lb[LANES][3],
                         w64 uram_fcw[FC_OUT * FC_WORDS], i32 fcb[FC_OUT], i32 Mq[4], i32 Sq[4])
{
    for (int i = 0; i < C1 * 9; i++) c1w[i / 9][i % 9] = wblob[OFF_C1W + i];
    for (int i = 0; i < C1; i++) c1b[i] = pblob[OFF_C1B + i];
    for (int l = 0; l < LANES; l++) {
        lb[l][0] = pblob[OFF_C2B + (l & (C2 - 1))];
        lb[l][1] = pblob[OFF_C3B + l];
        lb[l][2] = pblob[OFF_C4B + l];
    }
    for (int i = 0; i < FC_CLS; i++) fcb[i] = pblob[OFF_FCB + i];
    for (int i = 0; i < FC_POS; i++) fcb[FC_CLS + i] = pblob[OFF_POSB + i];
    for (int i = 0; i < 4; i++) { Mq[i] = pblob[OFF_M + 2 * i]; Sq[i] = pblob[OFF_M + 2 * i + 1]; }

    // conv2..conv4 -> per-lane word stores: word (woff + tap*ICG + g) of lane l packs
    // ic = g*8..g*8+7 of oc (l, or l&15 for conv2 so lanes 16..31 duplicate 0..15). Each word is
    // assembled from 8 (strided) blob bytes, then written once: no read-modify-write over the
    // 32 lane banks. Reload-only path, so the scattered DDR reads do not matter.
    for (int L = 0; L < 3; L++) {
        const int IC   = (L == 0) ? C1 : (L == 1) ? C2 : C3;
        const int off  = (L == 0) ? OFF_C2W : (L == 1) ? OFF_C3W : OFF_C4W;
        const int woff = (L == 0) ? 0 : (L == 1) ? 9 * (C1 / WIC) : 9 * (C1 / WIC) + 9 * (C2 / WIC);
        const int ICG  = IC / WIC;
        for (int l = 0; l < LANES; l++) {
            const int oc = (L == 0) ? (l & (C2 - 1)) : l;
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
static void load_img(const i8* img, i8 xb0[XB_N], i8 xb1[XB_N], i8 xb2[XB_N])
{
    int oy = 0, ox = 0, bank = 0, brow = 0;
    for (int i = 0; i < IMG_N; i++) {
#pragma HLS PIPELINE II=1
        xin_write(xb0, xb1, xb2, bank, brow * IN_DIM + ox, img[i]);
        if (++ox == IN_DIM) {
            ox = 0; ++oy;
            if (++bank == 3) { bank = 0; ++brow; }
        }
    }
}

static void load_cam(const w64* cam, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t bs,
                     bool inv, i8* img, i8 xb0[XB_N], i8 xb1[XB_N], i8 xb2[XB_N])
{
    if (bs > BLK_SHIFT_MAX) bs = BLK_SHIFT_MAX;
    const uint32_t crop = (uint32_t)IN_DIM << bs;
    const uint32_t mask = (1u << bs) - 1u;
    const uint32_t ash  = bs << 1;                       // / (blk*blk)

    w64 roww[ROWW_N];
    uint32_t acc[IN_DIM];
    int oy = 0, bank = 0, brow = 0;

    for (uint32_t cy = 0; cy < crop; cy++) {
#pragma HLS LOOP_TRIPCOUNT min=112 max=448
        const uint32_t start = (y0 + cy) * stride + x0 * 3u;
        const uint32_t w0 = start >> 3, off = start & 7u;
        const uint32_t nw = (off + crop * 3u + 7u) >> 3;
        for (uint32_t j = 0; j < nw; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=42 max=169
            roww[j] = cam[w0 + j];
        }
        const bool first = (cy & mask) == 0, last = (cy & mask) == mask;
        uint32_t run = 0;
        for (uint32_t cx = 0; cx < crop; cx++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=112 max=448
#pragma HLS DEPENDENCE variable=acc inter false
            const uint32_t b = off + cx * 3u + 1u;       // green byte == luma (mono)
            const uint32_t g = roww[b >> 3].range(8 * (b & 7) + 7, 8 * (b & 7));
            run += g;
            if ((cx & mask) == mask) {
                const uint32_t ox  = cx >> bs;
                const uint32_t tot = (first ? 0u : acc[ox]) + run;
                run = 0;
                if (last) {
                    int q = (int)((tot >> ash) >> 1);      // box average, 0..255 -> 0..127
                    if (inv) q = 127 - q;
                    xin_write(xb0, xb1, xb2, bank, brow * IN_DIM + (int)ox, (i8)q);
                    img[oy * IN_DIM + (int)ox] = (i8)q;
                } else {
                    acc[ox] = tot;
                }
            }
        }
        if (last) {
            ++oy;
            if (++bank == 3) { bank = 0; ++brow; }
        }
    }
}

// ------------------------------------------------------------------------------- conv1
// One conv pixel per clock over r=0..111, c=0..112 (one flush column). Window column c enters
// on the right; the conv output for column c-1 is formed and 2x2 max-pooled on the fly.
static void conv1(const i8 xb0[XB_N], const i8 xb1[XB_N], const i8 xb2[XB_N],
                  const i8 c1w[C1][9], const i32 c1b[C1], i32 M0, i32 sh, w64 dst[FM_N])
{
    i8  win[3][3];
#pragma HLS ARRAY_PARTITION variable=win complete dim=0
    i32 colmax[C1];
#pragma HLS ARRAY_PARTITION variable=colmax complete
    i32 rowbuf[P1][C1];
#pragma HLS ARRAY_PARTITION variable=rowbuf complete dim=2

    int r = 0, c = 0;
    int rb = 0, rrow = 0;            // bank of row r (r % 3) and its row inside the bank (r / 3)
    for (int i = 0; i < IN_DIM * (IN_DIM + 1); i++) {
#pragma HLS PIPELINE II=1
        // rows r-1, r, r+1 live in three different banks (row % 3); read each bank once.
        // Bank j holds window row k = (j - rb + 1) mod 3, i.e. image row r-1+k.
        i8 col[3];
#pragma HLS ARRAY_PARTITION variable=col complete
        for (int j = 0; j < 3; j++) {
#pragma HLS UNROLL
            int k = j - rb + 1;  if (k < 0) k += 3;  if (k > 2) k -= 3;
            const int s   = rb + k - 1;                         // -1..3: bank index before wrap
            const int brr = rrow + (s < 0 ? -1 : (s > 2 ? 1 : 0));
            const int row = r - 1 + k;
            const bool ok = (row >= 0) && (row < IN_DIM) && (c < IN_DIM);
            const int a = brr * IN_DIM + c;
            i8 x = 0;
            if (ok) x = (j == 0) ? xb0[a] : (j == 1) ? xb1[a] : xb2[a];
            col[k] = x;
        }
        for (int k = 0; k < 3; k++) {
#pragma HLS UNROLL
            if (c == 0) { win[k][0] = 0; win[k][1] = 0; }
            else        { win[k][0] = win[k][1]; win[k][1] = win[k][2]; }
            win[k][2] = col[k];
        }
        if (c >= 1) {
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
        if (++c == IN_DIM + 1) {
            c = 0; ++r;
            if (++rb == 3) { rb = 0; ++rrow; }
        }
    }
}

// ------------------------------------------------------------------------------- conv2..4
// Shared engine. Input map src: H x H x (ICG*8) as words [(h*H + w)*ICG + g]. Iterates pooled
// outputs (ph,pw) -> conv positions in the 2x2 window -> 9 taps -> ICG channel words, one word
// per clock into all LANES lanes. oc16: lanes 0..15 = col 2pw, lanes 16..31 = col 2pw+1 (same oc),
// so a pooled output takes 2 positions (dy) instead of 4. Pooled words are queued and written
// one per clock while the next window computes.
static void engine(const w64 src[FM_N], w64 dst[FM_N], const w64 lw[LANES][LW_N],
                   const i32 lb[LANES][3], int H, int ICG, bool oc16, int woff,
                   i32 M0, i32 sh, int li)
{
    const int PH   = H >> 1;
    const int NPOS = oc16 ? 2 : 4;
    const int NW   = oc16 ? 2 : 4;              // output words per pooled pixel
    const int TOTAL = PH * PH * NPOS * 9 * ICG;

    i32 acc[LANES], mx[LANES];
#pragma HLS ARRAY_PARTITION variable=acc complete
#pragma HLS ARRAY_PARTITION variable=mx complete
    w64 pend[4];
#pragma HLS ARRAY_PARTITION variable=pend complete
    for (int l = 0; l < LANES; l++) {
#pragma HLS UNROLL
        acc[l] = 0; mx[l] = -2147483647;
    }
    int npend = 0, paddr = 0;
    int ph = 0, pw = 0, p = 0, t = 0, kh = 0, kw = 0, g = 0;

    for (int i = 0; i < TOTAL; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=7056 max=14112
        const int dy = oc16 ? p : (p >> 1);
        const int dx = oc16 ? 0 : (p & 1);
        const int r  = 2 * ph + dy + kh - 1;
        const int ca = 2 * pw + dx + kw - 1;
        const int cb = ca + 1;
        const bool rv = (r >= 0) && (r < H);
        const bool va = rv && (ca >= 0) && (ca < H);
        const bool vb = oc16 && rv && (cb >= 0) && (cb < H);
        const w64 xa = va ? src[(r * H + ca) * ICG + g] : (w64)0;
        const w64 xb = vb ? src[(r * H + cb) * ICG + g] : (w64)0;
        const int widx = woff + t * ICG + g;
        const bool last_tap = (t == 8) && (g == ICG - 1);
        const bool last_pos = last_tap && (p == NPOS - 1);

        for (int l = 0; l < LANES; l += 2) {
#pragma HLS UNROLL
            const w64 x = (oc16 && l >= C2) ? xb : xa;   // lanes l, l+1 share x (same half)
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
                const i32 m = (oc16 && l < C2) ? imax(mx[l], mx[l + C2]) : mx[l];
                q[l] = requant(m, lb[l][li], M0, sh, true);
                mx[l] = -2147483647;
            }
            for (int k = 0; k < 4; k++) {
#pragma HLS UNROLL
                w64 w = 0;
                for (int b = 0; b < WIC; b++) {
#pragma HLS UNROLL
                    w.range(8 * b + 7, 8 * b) = (ap_uint<8>)(uint8_t)q[k * WIC + b];
                }
                pend[k] = w;
            }
            npend = NW;
            paddr = (ph * PH + pw) * NW;
        } else if (npend > 0) {
            dst[paddr] = pend[0];
            pend[0] = pend[1]; pend[1] = pend[2]; pend[2] = pend[3];
            ++paddr; --npend;
        }

        // counters: g -> (kh,kw)/t -> p -> pw -> ph
        if (++g == ICG) {
            g = 0;
            if (++kw == 3) { kw = 0; ++kh; }
            if (++t == 9) {
                t = 0; kh = 0; kw = 0;
                if (++p == NPOS) {
                    p = 0;
                    if (++pw == PH) { pw = 0; ++ph; }
                }
            }
        }
    }
    for (int k = 0; k < 4; k++) {
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
void mbdpu03(const i8* wblob, const i32* pblob, i8* img, i32* out,
             const w64* cam, uint32_t stride, uint32_t x0, uint32_t y0,
             uint32_t blk_shift, uint32_t ctrl)
{
#pragma HLS INTERFACE m_axi     port=wblob offset=slave bundle=gmem depth=33864
#pragma HLS INTERFACE m_axi     port=pblob offset=slave bundle=gmem depth=110
#pragma HLS INTERFACE m_axi     port=img   offset=slave bundle=gmem depth=12544
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
    static i32 lb[LANES][3];
#pragma HLS ARRAY_PARTITION variable=lb complete dim=1
    static w64 uram_fcw[FC_OUT * FC_WORDS];
#pragma HLS bind_storage variable=uram_fcw type=ram_2p impl=bram
    static i32 fcb[FC_OUT];
#pragma HLS ARRAY_PARTITION variable=fcb complete
    static i32 Mq[4], Sq[4];
#pragma HLS ARRAY_PARTITION variable=Mq complete
#pragma HLS ARRAY_PARTITION variable=Sq complete

    // ---- per-call buffers ----
    static i8  xb0[XB_N], xb1[XB_N], xb2[XB_N];
    static w64 uram_fm0[FM_N], uram_fm1[FM_N];
#pragma HLS bind_storage variable=uram_fm0 type=ram_2p impl=bram
#pragma HLS bind_storage variable=uram_fm1 type=ram_2p impl=bram

    if (ctrl & CTRL_RELOAD)
        load_weights(wblob, pblob, c1w, c1b, lw, lb, uram_fcw, fcb, Mq, Sq);

    if (ctrl & CTRL_SRC_IMG) load_img(img, xb0, xb1, xb2);
    else load_cam(cam, stride, x0, y0, blk_shift, (ctrl & CTRL_INVERT) != 0, img, xb0, xb1, xb2);

    conv1(xb0, xb1, xb2, c1w, c1b, Mq[0], Sq[0], uram_fm0);
    engine(uram_fm0, uram_fm1, lw, lb, P1, C1 / WIC, true, 0,
           Mq[1], Sq[1], 0);                                               // 56x56x8  -> 28x28x16
    engine(uram_fm1, uram_fm0, lw, lb, P2, C2 / WIC, false, 9 * (C1 / WIC),
           Mq[2], Sq[2], 1);                                               // 28x28x16 -> 14x14x32
    engine(uram_fm0, uram_fm1, lw, lb, P3, C3 / WIC, false, 9 * (C1 / WIC) + 9 * (C2 / WIC),
           Mq[3], Sq[3], 2);                                               // 14x14x32 -> 7x7x32
    fc(uram_fm1, uram_fcw, fcb, out);
}
