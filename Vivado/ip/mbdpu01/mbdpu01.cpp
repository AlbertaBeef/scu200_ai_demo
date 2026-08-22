// mbdpu01.cpp — SCU200 AI demo custom DPU: quantized MNIST CNN (INT8).
// SPDX-License-Identifier: Apache-2.0
//
// mbdpu01 = the MNIST classifier core of the SCU200 rotating-disc AI demo.
// Real INT8 conv/pool/FC compute datapath in the MicroBlaze-hosted shell.
//
// Network (symmetric per-layer INT8; INT32 accumulate; TFLite-style requant):
//   in 28x28x1
//   -> Conv1 1->8,  3x3 pad1  -> +bias -> ReLU -> requant(M0,shift) -> 28x28x8
//   -> MaxPool 2x2                                                   -> 14x14x8
//   -> Conv2 8->16, 3x3 pad1  -> +bias -> ReLU -> requant           -> 14x14x16
//   -> MaxPool 2x2                                                   -> 7x7x16
//   -> FC 784->10             -> +bias  (raw INT32 logits)          -> 10
//   -> argmax
//
// Weights + params + image are streamed from DDR into on-chip arrays (the DPU pattern:
// retrain without resynthesis — load a new blob at runtime, like the background image).
//
// Interfaces (all four pointers share one AXI4 master 'gmem', distinct s_axilite offsets):
//   wblob : int8  weight blob   (conv1_w[8][1][3][3], conv2_w[16][8][3][3], fc_w[10][784]) = 9064
//   pblob : int32 param blob    (conv1_b[8], conv2_b[16], fc_b[10], c1_M0,c1_sh, c2_M0,c2_sh) = 38
//   img   : int8  input image   (28*28 = 784, row-major)
//   out   : int32 result        (10 logits, then [10] = argmax prediction) = 11

#include <stdint.h>
#include <ap_int.h>

// ---- network geometry ----
#define IN_H   28
#define IN_W   28
#define C1     8            // conv1 out channels
#define C2     16           // conv2 out channels
#define KS     3            // kernel size
#define P1_H   14           // after conv1 + pool
#define P1_W   14
#define P2_H   7            // after conv2 + pool
#define P2_W   7
#define FC_IN  (P2_H*P2_W*C2)   // 784
#define FC_OUT 10

// ---- weight-blob offsets (int8 elements) ----
#define OFF_C1W 0
#define OFF_C2W (OFF_C1W + C1*1*KS*KS)     // 72
#define OFF_FCW (OFF_C2W + C2*C1*KS*KS)    // 1224
#define WBLOB_N (OFF_FCW + FC_OUT*FC_IN)   // 9064

// ---- param-blob offsets (int32 elements) ----
#define OFF_C1B 0
#define OFF_C2B (OFF_C1B + C1)             // 8
#define OFF_FCB (OFF_C2B + C2)             // 24
#define OFF_C1M (OFF_FCB + FC_OUT)         // 34  (conv1 M0)
#define OFF_C1S (OFF_C1M + 1)              // 35  (conv1 shift)
#define OFF_C2M (OFF_C1S + 1)              // 36  (conv2 M0)
#define OFF_C2S (OFF_C2M + 1)              // 37  (conv2 shift)
#define PBLOB_N (OFF_C2S + 1)              // 38

typedef int8_t   i8;
typedef int32_t  i32;

// TFLite-style requant of an INT32 accumulator to INT8, with optional ReLU.
//   y = clamp( round( (acc+bias) * M0 / 2^shift ) )
// The multiplier M0 is trained to fit in <=17 bits (see train_mnist.py), so the
// (acc+bias)*M0 product is a 27x18 multiply -> ONE DSP48 (fast + no fabric multiplier),
// which fixes both the 300 MHz critical path and the LUT the int64 multiply consumed.
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

void mbdpu01(const i8* wblob, const i32* pblob, const i8* img, i32* out)
{
#pragma HLS INTERFACE m_axi     port=wblob offset=slave bundle=gmem depth=9064
#pragma HLS INTERFACE m_axi     port=pblob offset=slave bundle=gmem depth=38
#pragma HLS INTERFACE m_axi     port=img   offset=slave bundle=gmem depth=784
#pragma HLS INTERFACE m_axi     port=out   offset=slave bundle=gmem depth=11
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
    for (int i = 0; i < FC_OUT; i++) fcb[i] = pblob[OFF_FCB + i];
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

    // ==== FC 784->10 (raw INT32 logits), flatten p2 as [oh][ow][oc] ====
    i32 logits[FC_OUT];
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
        logits[o] = acc;
        out[o] = acc;
        if (acc > bestv) { bestv = acc; best = o; }
    }
    out[FC_OUT] = best;   // argmax prediction
}

// =====================================================================
//  Self-checking C testbench (host only; excluded from synthesis).
//  Generates deterministic weights/image, computes a golden reference with
//  the SAME integer arithmetic, and verifies mbdpu01 is bit-exact. This proves
//  the datapath (not accuracy — that needs the trained MNIST blob at runtime).
// =====================================================================
#ifndef __SYNTHESIS__
#include <cstdio>
#include <cstdlib>

static int8_t  g_w[WBLOB_N];
static int32_t g_p[PBLOB_N];
static int8_t  g_img[IN_H*IN_W];
static int32_t g_out[FC_OUT+1];
static int32_t g_ref[FC_OUT+1];

static int8_t sat8(int v){ if(v>127)v=127; if(v<-127)v=-127; return (int8_t)v; }

// plain-C golden (independent re-implementation of the forward pass)
static void golden()
{
    int8_t a1[IN_H][IN_W][C1], p1[P1_H][P1_W][C1];
    int8_t a2[P1_H][P1_W][C2], p2[P2_H][P2_W][C2];
    int32_t c1M=g_p[OFF_C1M], c1S=g_p[OFF_C1S], c2M=g_p[OFF_C2M], c2S=g_p[OFF_C2S];
    #define REQ(acc,bias,M,S) sat8((int)(((( (int64_t)((acc)+(bias)) )*(M)) + ((int64_t)1<<((S)-1))) >> (S)) < 0 ? 0 : \
                                   (int)(((( (int64_t)((acc)+(bias)) )*(M)) + ((int64_t)1<<((S)-1))) >> (S)))
    for(int oc=0;oc<C1;oc++)for(int oh=0;oh<IN_H;oh++)for(int ow=0;ow<IN_W;ow++){
        int32_t acc=0;
        for(int kh=0;kh<KS;kh++)for(int kw=0;kw<KS;kw++){int ih=oh+kh-1,iw=ow+kw-1;
            if(ih>=0&&ih<IN_H&&iw>=0&&iw<IN_W) acc+=(int32_t)g_img[ih*IN_W+iw]*g_w[OFF_C1W+((oc*1+0)*KS+kh)*KS+kw];}
        a1[oh][ow][oc]=REQ(acc,g_p[OFF_C1B+oc],c1M,c1S);
    }
    for(int oc=0;oc<C1;oc++)for(int oh=0;oh<P1_H;oh++)for(int ow=0;ow<P1_W;ow++){int8_t m=-128;
        for(int dh=0;dh<2;dh++)for(int dw=0;dw<2;dw++){int8_t v=a1[oh*2+dh][ow*2+dw][oc]; if(v>m)m=v;} p1[oh][ow][oc]=m;}
    for(int oc=0;oc<C2;oc++)for(int oh=0;oh<P1_H;oh++)for(int ow=0;ow<P1_W;ow++){
        int32_t acc=0;
        for(int ic=0;ic<C1;ic++)for(int kh=0;kh<KS;kh++)for(int kw=0;kw<KS;kw++){int ih=oh+kh-1,iw=ow+kw-1;
            if(ih>=0&&ih<P1_H&&iw>=0&&iw<P1_W) acc+=(int32_t)p1[ih][iw][ic]*g_w[OFF_C2W+(((oc*C1+ic)*KS)+kh)*KS+kw];}
        a2[oh][ow][oc]=REQ(acc,g_p[OFF_C2B+oc],c2M,c2S);
    }
    for(int oc=0;oc<C2;oc++)for(int oh=0;oh<P2_H;oh++)for(int ow=0;ow<P2_W;ow++){int8_t m=-128;
        for(int dh=0;dh<2;dh++)for(int dw=0;dw<2;dw++){int8_t v=a2[oh*2+dh][ow*2+dw][oc]; if(v>m)m=v;} p2[oh][ow][oc]=m;}
    int best=0; int32_t bv=-2147483647;
    for(int o=0;o<FC_OUT;o++){int32_t acc=g_p[OFF_FCB+o]; int idx=0;
        for(int oh=0;oh<P2_H;oh++)for(int ow=0;ow<P2_W;ow++)for(int oc=0;oc<C2;oc++){
            acc+=(int32_t)p2[oh][ow][oc]*g_w[OFF_FCW+o*FC_IN+idx]; idx++;}
        g_ref[o]=acc; if(acc>bv){bv=acc;best=o;}}
    g_ref[FC_OUT]=best;
}

int main()
{
    // deterministic pseudo-random small weights + image (LCG, no libs needed)
    uint32_t s=12345;
    #define RND ((s = s*1103515245u+12345u), (int)((s>>16)&0xff))
    for(int i=0;i<WBLOB_N;i++) g_w[i]=(int8_t)((RND%15)-7);      // weights in [-7,7]
    for(int i=0;i<IN_H*IN_W;i++) g_img[i]=(int8_t)(RND%50);      // image in [0,49]
    for(int i=0;i<C1+C2+FC_OUT;i++) g_p[i]=(RND%64)-32;          // biases
    g_p[OFF_C1M]=1<<14; g_p[OFF_C1S]=15;                         // requant ~ *0.5
    g_p[OFF_C2M]=1<<14; g_p[OFF_C2S]=16;                         // requant ~ *0.25

    golden();
    mbdpu01(g_w, g_p, g_img, g_out);

    int errs=0;
    for(int o=0;o<=FC_OUT;o++) if(g_out[o]!=g_ref[o]){
        printf("  MISMATCH out[%d] hw=%d ref=%d\n", o, g_out[o], g_ref[o]); errs++;
    }
    printf("mbdpu01 C-sim: logits[0..9] pred=%d  (ref pred=%d)  -> %s\n",
           g_out[FC_OUT], g_ref[FC_OUT], errs==0 ? "PASS" : "FAIL");
    return errs ? 1 : 0;
}
#endif // __SYNTHESIS__
