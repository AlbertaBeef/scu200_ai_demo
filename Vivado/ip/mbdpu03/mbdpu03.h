// mbdpu03.h — geometry, blob offsets, control bits and top-level prototype shared by the HLS
// core (mbdpu03.cpp) and its C testbench (mbdpu03_tb.cpp). See mbdpu03.cpp for the network.
// SPDX-License-Identifier: Apache-2.0
#ifndef MBDPU03_H
#define MBDPU03_H

#include <stdint.h>
#include <ap_int.h>

// ---- network geometry ----
#define IN_DIM  112                 // model input side (mono)
#define C1      8                   // conv1 out channels (in = 1)
#define C2      16
#define C3      32
#define C4      32
#define KS      3
#define P1      56                  // pooled sides after conv1..conv4
#define P2      28
#define P3      14
#define P4      7
#define FC_IN   (P4*P4*C4)          // 1568
#define FC_CLS  10
#define FC_POS  2
#define FC_OUT  (FC_CLS + FC_POS)   // 12
#define OUT_N   (FC_CLS + 1 + FC_POS)  // 13: logits, argmax, dx, dy
#define IMG_N   (IN_DIM*IN_DIM)     // 12544

// ---- engine geometry (conv2..conv4 share one MAC array) ----
#define WIC     8                   // int8 channels packed per 64-bit word
#define LANES   16                  // output-channel lanes (x WIC MACs each per cycle)
// per-lane weight words: conv2 (1 oc group) + conv3 (2 groups) + conv4 (2 groups) = 9+36+72 = 117
#define LW_N    (9*(C1/WIC)*(C2/LANES) + 9*(C2/WIC)*(C3/LANES) + 9*(C3/WIC)*(C4/LANES))
#define LW_OFF2 0
#define LW_OFF3 (9*(C1/WIC)*(C2/LANES))                 // 9
#define LW_OFF4 (LW_OFF3 + 9*(C2/WIC)*(C3/LANES))       // 45
#define LB_N    5                   // per-lane biases: c2, c3 grp0/1, c4 grp0/1
#define FC_WORDS (FC_IN/WIC)        // 196 words per FC row

// ---- weight-blob offsets (int8 elements; trainer order w[oc][ic][kh][kw], fc[row][h][w][c]) ----
#define OFF_C1W 0
#define OFF_C2W (OFF_C1W + C1*1*KS*KS)     // 72
#define OFF_C3W (OFF_C2W + C2*C1*KS*KS)    // 1224
#define OFF_C4W (OFF_C3W + C3*C2*KS*KS)    // 5832
#define OFF_FCW (OFF_C4W + C4*C3*KS*KS)    // 15048 (rows 0..9 class, 10..11 position)
#define WBLOB_N (OFF_FCW + FC_OUT*FC_IN)   // 33864

// ---- param-blob offsets (int32 elements) ----
#define OFF_C1B  0
#define OFF_C2B  (OFF_C1B + C1)            // 8
#define OFF_C3B  (OFF_C2B + C2)            // 24
#define OFF_C4B  (OFF_C3B + C3)            // 56
#define OFF_FCB  (OFF_C4B + C4)            // 88  (class biases)
#define OFF_M    (OFF_FCB + FC_CLS)        // 98  ({M0, shift} x 4: conv1..conv4)
#define OFF_POSB (OFF_M + 8)               // 106 (position biases dx, dy)
#define OFF_POSK (OFF_POSB + FC_POS)       // 108 (firmware-only: px*100 multiplier)
#define OFF_POSH (OFF_POSK + 1)            // 109 (firmware-only: shift)
#define PBLOB_N  (OFF_POSH + 1)            // 110

// ---- ctrl register bits ----
#define CTRL_RELOAD  0x1u   // (re)load weights + params from DDR into the on-chip stores first
#define CTRL_SRC_IMG 0x2u   // input = the 112x112 int8 image at img (no camera read, no write-back)
#define CTRL_INVERT  0x4u   // camera path: q = 127 - q
#define BLK_SHIFT_MAX 2     // camera crop = 112 << blk_shift (112 / 224 / 448), box-averaged

// camera frame (RGB8, mono R=G=B) bounds, for the testbench and documentation
#define CAM_W   640
#define CAM_H   480

typedef int8_t       i8;
typedef int32_t      i32;
typedef ap_uint<64>  w64;

void mbdpu03(const i8* wblob, const i32* pblob, i8* img, i32* out,
             const w64* cam, uint32_t stride, uint32_t x0, uint32_t y0,
             uint32_t blk_shift, uint32_t ctrl);

#endif // MBDPU03_H
