// mbdpu02.cpp — SCU200 AI demo custom DPU: quantized MNIST CNN (INT8) + digit-position head.
// SPDX-License-Identifier: Apache-2.0
//
// mbdpu02 = mbdpu01 (the rotating-disc MNIST classifier) with a second output head that
// regresses where the digit sits inside the 28x28 ROI. Same conv trunk, same quantization;
// the FC simply grows from 10 to 12 rows and the two extra rows are the position head.
//
// Network (symmetric per-layer INT8; INT32 accumulate; TFLite-style requant):
//   in 28x28x1
//   -> Conv1 1->8,  3x3 pad1  -> +bias -> ReLU -> requant(M0,shift) -> 28x28x8
//   -> MaxPool 2x2                                                   -> 14x14x8
//   -> Conv2 8->16, 3x3 pad1  -> +bias -> ReLU -> requant           -> 14x14x16
//   -> MaxPool 2x2                                                   -> 7x7x16
//   -> FC 784->12             -> +bias  (raw INT32)                 -> 10 logits + dx + dy
//   -> argmax over the 10 logits
//
// Position head: out[11] = dx accumulator, out[12] = dy accumulator (raw INT32, no requant).
// Firmware converts with the scale the trainer stores at the end of the param blob:
//   px*100 = (acc * pblob[40]) >> pblob[41]   (dx: left -ve/right +ve, dy: top +ve/bottom -ve)
//
// Interfaces (all four pointers share one AXI4 master 'gmem', distinct s_axilite offsets —
// the SAME ports as mbdpu01, so the register map is unchanged):
//   wblob : int8  (conv1_w[8][1][3][3], conv2_w[16][8][3][3], fc_w[12][784])            = 10632
//   pblob : int32 (conv1_b[8], conv2_b[16], fc_b[10], c1_M0,c1_sh, c2_M0,c2_sh,
//                  pos_b[2], pos_k, pos_sh)                                              = 42
//   img   : int8  input image (28*28 = 784, row-major)
//   out   : int32 result (10 logits, [10] = argmax, [11] = dx, [12] = dy)                 = 13
// Both blobs are prefix-compatible with mbdpu01's (head rows/params appended at the end).

#include "mbdpu02.h"
#include <ap_int.h>

// TFLite-style requant of an INT32 accumulator to INT8, with optional ReLU (as mbdpu01).
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

void mbdpu02(const i8* wblob, const i32* pblob, const i8* img, i32* out)
{
#pragma HLS INTERFACE m_axi     port=wblob offset=slave bundle=gmem depth=10632
#pragma HLS INTERFACE m_axi     port=pblob offset=slave bundle=gmem depth=42
#pragma HLS INTERFACE m_axi     port=img   offset=slave bundle=gmem depth=784
#pragma HLS INTERFACE m_axi     port=out   offset=slave bundle=gmem depth=13
#pragma HLS INTERFACE s_axilite port=wblob bundle=control
#pragma HLS INTERFACE s_axilite port=pblob bundle=control
#pragma HLS INTERFACE s_axilite port=img   bundle=control
#pragma HLS INTERFACE s_axilite port=out   bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    // ---- on-chip weights / params ----
    static i8  c1w[C1][1][KS][KS];
    static i8  c2w[C2][C1][KS][KS];
    static i8  fcw[FC_OUT][FC_IN];
    static i32 c1b[C1], c2b[C2], fcb[FC_OUT];
    i32 c1_M0, c1_sh, c2_M0, c2_sh;

    // ---- feature maps ----
    static i8 x0[IN_H][IN_W];        // input
    static i8 a1[IN_H][IN_W][C1];    // conv1 out (pre-pool)
    static i8 p1[P1_H][P1_W][C1];    // pool1 out
    static i8 a2[IN_H/2][IN_W/2][C2];// conv2 out (pre-pool), 14x14x16
    static i8 p2[P2_H][P2_W][C2];    // pool2 out

    // ==== load weights from DDR ====
    for (int i = 0; i < C1*1*KS*KS; i++)  ((i8*)c1w)[i] = wblob[OFF_C1W + i];
    for (int i = 0; i < C2*C1*KS*KS; i++) ((i8*)c2w)[i] = wblob[OFF_C2W + i];
    for (int i = 0; i < FC_OUT*FC_IN; i++)((i8*)fcw)[i] = wblob[OFF_FCW + i];
    for (int i = 0; i < C1; i++) c1b[i] = pblob[OFF_C1B + i];
    for (int i = 0; i < C2; i++) c2b[i] = pblob[OFF_C2B + i];
    for (int i = 0; i < FC_CLS; i++) fcb[i] = pblob[OFF_FCB + i];
    for (int i = 0; i < FC_POS; i++) fcb[FC_CLS + i] = pblob[OFF_POSB + i];
    c1_M0 = pblob[OFF_C1M]; c1_sh = pblob[OFF_C1S];
    c2_M0 = pblob[OFF_C2M]; c2_sh = pblob[OFF_C2S];

    // ==== load image ====
    for (int h = 0; h < IN_H; h++)
        for (int w = 0; w < IN_W; w++)
            x0[h][w] = img[h*IN_W + w];

    // ==== Conv1 (1->C1, 3x3, pad1) + ReLU + requant ====
    for (int oc = 0; oc < C1; oc++)
        for (int oh = 0; oh < IN_H; oh++)
            for (int ow = 0; ow < IN_W; ow++) {
                i32 acc = 0;
                for (int kh = 0; kh < KS; kh++)
                    for (int kw = 0; kw < KS; kw++) {
                        int ih = oh + kh - 1, iw = ow + kw - 1;
                        if (ih >= 0 && ih < IN_H && iw >= 0 && iw < IN_W)
                            acc += (i32)x0[ih][iw] * (i32)c1w[oc][0][kh][kw];
                    }
                a1[oh][ow][oc] = requant(acc, c1b[oc], c1_M0, c1_sh, true);
            }

    // ==== MaxPool1 2x2 -> 14x14x8 ====
    for (int oc = 0; oc < C1; oc++)
        for (int oh = 0; oh < P1_H; oh++)
            for (int ow = 0; ow < P1_W; ow++) {
                i8 m = -128;
                for (int dh = 0; dh < 2; dh++)
                    for (int dw = 0; dw < 2; dw++) {
                        i8 v = a1[oh*2+dh][ow*2+dw][oc];
                        if (v > m) m = v;
                    }
                p1[oh][ow][oc] = m;
            }

    // ==== Conv2 (C1->C2, 3x3, pad1) + ReLU + requant ====
    for (int oc = 0; oc < C2; oc++)
        for (int oh = 0; oh < P1_H; oh++)
            for (int ow = 0; ow < P1_W; ow++) {
                i32 acc = 0;
                for (int ic = 0; ic < C1; ic++)
                    for (int kh = 0; kh < KS; kh++)
                        for (int kw = 0; kw < KS; kw++) {
                            int ih = oh + kh - 1, iw = ow + kw - 1;
                            if (ih >= 0 && ih < P1_H && iw >= 0 && iw < P1_W)
                                acc += (i32)p1[ih][iw][ic] * (i32)c2w[oc][ic][kh][kw];
                        }
                a2[oh][ow][oc] = requant(acc, c2b[oc], c2_M0, c2_sh, true);
            }

    // ==== MaxPool2 2x2 -> 7x7x16 ====
    for (int oc = 0; oc < C2; oc++)
        for (int oh = 0; oh < P2_H; oh++)
            for (int ow = 0; ow < P2_W; ow++) {
                i8 m = -128;
                for (int dh = 0; dh < 2; dh++)
                    for (int dw = 0; dw < 2; dw++) {
                        i8 v = a2[oh*2+dh][ow*2+dw][oc];
                        if (v > m) m = v;
                    }
                p2[oh][ow][oc] = m;
            }

    // ==== FC 784->12 (raw INT32), flatten p2 as [oh][ow][oc] ====
    // rows 0..9 -> out[0..9] (logits, argmax over these only); rows 10,11 -> out[11], out[12]
    int best = 0; i32 bestv = -2147483647;
    for (int o = 0; o < FC_OUT; o++) {
        i32 acc = fcb[o];
        int idx = 0;
        for (int oh = 0; oh < P2_H; oh++)
            for (int ow = 0; ow < P2_W; ow++)
                for (int oc = 0; oc < C2; oc++) {
                    acc += (i32)p2[oh][ow][oc] * (i32)fcw[o][idx];
                    idx++;
                }
        if (o < FC_CLS) {
            out[o] = acc;
            if (acc > bestv) { bestv = acc; best = o; }
        } else {
            out[o + 1] = acc;           // skip the argmax slot
        }
    }
    out[FC_CLS] = best;   // argmax prediction (same slot as mbdpu01)
}
