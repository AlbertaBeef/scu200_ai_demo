// mbdpu01.h — MicroBlaze bare-metal driver for the mbdpu01 HLS core (milestone 2).
// SPDX-License-Identifier: Apache-2.0
//
// mbdpu01 = quantized MNIST CNN (INT8). Reads weight/param/image blobs from DDR,
// writes 10 INT32 logits + argmax to a DDR out buffer. See Vivado/ip/mbdpu01/.
//
// AXI4-Lite "control" register map — CONFIRMED against the HLS-generated xmbdpu01_hw.h
// (build/scu200/mbdpu01.prj/sol1/impl/misc/drivers/mbdpu01_v1_0/src/). Re-verify if the
// mbdpu01() signature changes (HLS reassigns offsets when ports are added/reordered).
#ifndef MBDPU01_H
#define MBDPU01_H

#include <stdint.h>

// --- control block (standard HLS ap_ctrl_hs) ---
#define MBDPU01_AP_CTRL   0x00u  // b0 ap_start, b1 ap_done, b2 ap_idle, b3 ap_ready
#define MBDPU01_GIE       0x04u  // global interrupt enable
#define MBDPU01_IER       0x08u  // ip interrupt enable
#define MBDPU01_ISR       0x0Cu  // ip interrupt status

// --- 64-bit DDR pointer registers (lo/hi) ---
#define MBDPU01_WBLOB_LO  0x10u  // weights   (int8[9064])
#define MBDPU01_WBLOB_HI  0x14u
#define MBDPU01_PBLOB_LO  0x1Cu  // params    (int32[38]: biases + M0/shift)
#define MBDPU01_PBLOB_HI  0x20u
#define MBDPU01_IMG_LO    0x28u  // image     (int8[784], 0..127)
#define MBDPU01_IMG_HI    0x2Cu
#define MBDPU01_OUT_LO    0x34u  // result    (int32[11]: 10 logits + [10]=argmax)
#define MBDPU01_OUT_HI    0x38u

// --- blob sizes (bytes) for host-side DDR loading / cache management ---
#define MBDPU01_WBLOB_BYTES 9064u        // int8 weights
#define MBDPU01_PBLOB_BYTES (38u*4u)     // int32 params
#define MBDPU01_IMG_BYTES   784u         // int8 image (28*28)
#define MBDPU01_OUT_N       11u          // int32 words out (10 logits + argmax)
#define MBDPU01_OUT_BYTES   (MBDPU01_OUT_N*4u)

void mbdpu01_init(uint32_t base);        // enable the ap_done interrupt (optional)
int  mbdpu01_idle(uint32_t base);        // 1 when core free to accept a new start
int  mbdpu01_done(uint32_t base);        // 1 when ap_done is set (polling path)
void mbdpu01_start(uint32_t base, uint64_t wblob, uint64_t pblob, uint64_t img, uint64_t out);

// Blocking inference: flushes the input blobs, starts the core, polls done, invalidates
// the out buffer, and returns the argmax prediction (out[10]). `logits` (may be NULL) is
// filled with the 10 raw INT32 logits. All four addresses are DDR byte addresses.
int  mbdpu01_infer(uint32_t base, uint64_t wblob, uint64_t pblob,
                   uint64_t img, uint64_t out, int32_t logits[10]);

// --- mbdpu02 = mbdpu01 + a dx/dy digit-position head (Vivado/ip/mbdpu02/) ---------------
// Same ports and register map (verified against xmbdpu02_hw.h), so the MBDPU01_* register
// offsets and mbdpu01_init/idle/done/start apply unchanged. Larger blobs + output:
#define MBDPU02_WBLOB_BYTES 10632u       // int8 weights (fc grows 10 -> 12 rows)
#define MBDPU02_PBLOB_BYTES (42u*4u)     // int32 params (+ pos_b[2], pos_k, pos_sh)
#define MBDPU02_OUT_N       13u          // 10 logits, [10] argmax, [11] dx acc, [12] dy acc
#define MBDPU02_OUT_BYTES   (MBDPU02_OUT_N*4u)
#define MBDPU02_PBLOB_POSK  40u          // pblob index: px*100 multiplier
#define MBDPU02_PBLOB_POSH  41u          // pblob index: shift

// Blocking inference on mbdpu02. As mbdpu01_infer, plus `pos_x100` (may be NULL) receives the
// digit position in 1/100 model pixels, 0 = centred: [0] dx (left -ve, right +ve),
// [1] dy (top +ve, bottom -ve). Converted as (acc * pblob[40]) >> pblob[41].
int  mbdpu02_infer(uint32_t base, uint64_t wblob, uint64_t pblob,
                   uint64_t img, uint64_t out, int32_t logits[10], int32_t pos_x100[2]);

// --- mbdpu03 = 112x112 mono CNN + dx/dy head, camera crop fused in (Vivado/ip/mbdpu03/) -----
// The four pointer registers (wblob/pblob/img/out) keep the MBDPU01_* offsets; the extra
// registers below are CONFIRMED against the HLS-generated xmbdpu03_hw.h.
#define MBDPU03_CAM_LO     0x40u  // camera frame (RGB8 mono) DDR address
#define MBDPU03_CAM_HI     0x44u
#define MBDPU03_STRIDE     0x4Cu  // frame stride (bytes)
#define MBDPU03_X0         0x54u  // crop top-left
#define MBDPU03_Y0         0x5Cu
#define MBDPU03_BLK_SHIFT  0x64u  // zoom: crop = 112 << blk_shift (0..2), box-averaged to 112x112
#define MBDPU03_CTRL       0x6Cu  // bits below
#define MBDPU03_CTRL_RELOAD  0x1u // (re)load weights + params into the core's on-chip stores
#define MBDPU03_CTRL_SRC_IMG 0x2u // input from img (self-test) instead of the camera
#define MBDPU03_CTRL_INVERT  0x4u // camera path: 127 - q
#define MBDPU03_DIM          112u
#define MBDPU03_WBLOB_BYTES  33864u      // int8 weights (4 conv + fc 12x1568)
#define MBDPU03_PBLOB_BYTES  (110u*4u)   // int32 params
#define MBDPU03_IMG_BYTES    (112u*112u) // 112x112 int8 model input (written back by the core)
#define MBDPU03_OUT_N        13u
#define MBDPU03_OUT_BYTES    (MBDPU03_OUT_N*4u)
#define MBDPU03_PBLOB_POSK   108u        // pblob index: px*100 multiplier (112-px units)
#define MBDPU03_PBLOB_POSH   109u        // pblob index: shift

// Blocking inference on mbdpu03 straight from a camera frame: the core crops (x0,y0, zoom
// blk_shift), converts to 0..127 (no invert: the model is trained inverted), writes the 112x112
// input back to img, and runs the CNN. Weights are cached on chip; they are (re)loaded on the
// first call and whenever the param blob changes (e.g. after reload_weights.tcl). Returns the
// argmax; logits / pos_x100 (1/100 px, 112-px units) may be NULL. img is cache-invalidated
// on return so the CPU can draw it.
int  mbdpu03_infer(uint32_t base, uint64_t wblob, uint64_t pblob, uint64_t img, uint64_t out,
                   uint64_t cam, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t blk_shift,
                   int32_t logits[10], int32_t pos_x100[2]);

#endif // MBDPU01_H
