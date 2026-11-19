// mbdpu02.h — geometry, blob offsets and top-level prototype shared by the HLS core
// (mbdpu02.cpp) and its C testbench (mbdpu02_tb.cpp). See mbdpu02.cpp for the network.
// SPDX-License-Identifier: Apache-2.0
#ifndef MBDPU02_H
#define MBDPU02_H

#include <stdint.h>

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
#define FC_CLS 10               // class logits
#define FC_POS 2                // position head (dx, dy)
#define FC_OUT (FC_CLS + FC_POS)// 12 FC rows
#define OUT_N  (FC_CLS + 1 + FC_POS)  // 13: logits, argmax, dx, dy

// ---- weight-blob offsets (int8 elements) ----
#define OFF_C1W 0
#define OFF_C2W (OFF_C1W + C1*1*KS*KS)     // 72
#define OFF_FCW (OFF_C2W + C2*C1*KS*KS)    // 1224 (rows 0..9 class, rows 10..11 position)
#define WBLOB_N (OFF_FCW + FC_OUT*FC_IN)   // 10632

// ---- param-blob offsets (int32 elements) ----
#define OFF_C1B  0
#define OFF_C2B  (OFF_C1B + C1)            // 8
#define OFF_FCB  (OFF_C2B + C2)            // 24  (class biases)
#define OFF_C1M  (OFF_FCB + FC_CLS)        // 34  (conv1 M0)
#define OFF_C1S  (OFF_C1M + 1)             // 35  (conv1 shift)
#define OFF_C2M  (OFF_C1S + 1)             // 36  (conv2 M0)
#define OFF_C2S  (OFF_C2M + 1)             // 37  (conv2 shift)
#define OFF_POSB (OFF_C2S + 1)             // 38  (position biases dx, dy)
#define OFF_POSK (OFF_POSB + FC_POS)       // 40  (firmware-only: px*100 multiplier)
#define OFF_POSH (OFF_POSK + 1)            // 41  (firmware-only: shift)
#define PBLOB_N  (OFF_POSH + 1)            // 42

typedef int8_t   i8;
typedef int32_t  i32;

void mbdpu02(const i8* wblob, const i32* pblob, const i8* img, i32* out);

#endif // MBDPU02_H
