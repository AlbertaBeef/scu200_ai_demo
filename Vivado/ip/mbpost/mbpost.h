// mbpost.h - layout + interface shared by the mbpost core and its testbench.
// SPDX-License-Identifier: Apache-2.0
#ifndef MBPOST_H
#define MBPOST_H
#include <stdint.h>
#include <ap_int.h>

typedef ap_uint<64> w64;
typedef ap_uint<8>  u8;

// ---- layout (must match mnist_pre.c) --------------------------------------------------------
#define ROI_X     1340
#define ROI_Y     680
#define ROI_PX    112
#define IMG_WPR   (ROI_PX / 8)          // 14 img words per preview row
#define BAR_X     1525
#define BAR_Y0    600
#define BAR_DY    27
#define BAR_W     260
#define BAR_H     18
#define POS_HALF  (ROI_PX / 2)          // 56
#define POS_TRK   6
#define POS_MK_L  16
#define POS_MK_W  4
#define DX_TY     (ROI_Y + ROI_PX + 8)  // 800: DX track top
#define DY_TX     (ROI_X - 16)          // 1324: DY track left
#define DXB_X0    (ROI_X - 4)           // 1336
#define DXB_X1    (ROI_X + ROI_PX + 4)  // 1456
#define DXB_Y0    (DX_TY - 6)           // 794
#define DXB_Y1    (DX_TY + POS_TRK + 6) // 812
#define DYB_X0    (DY_TX - 6)           // 1318
#define DYB_X1    (DY_TX + POS_TRK + 6) // 1336
#define DYB_Y0    (ROI_Y - 4)           // 676
#define DYB_Y1    (ROI_Y + ROI_PX + 4)  // 796
#define CX        (ROI_X + POS_HALF)    // 1396: DX zero
#define CY        (ROI_Y + POS_HALF)    // 736 : DY zero

// block A = preview + both gauge bands, 8-px aligned: x [1312,1456), y [676,812)
#define A_X0      ((DYB_X0 / 8) * 8)    // 1312
#define A_NG      ((DXB_X1 - A_X0 + 7) / 8)   // 18 groups
#define A_Y0      DYB_Y0
#define A_Y1      DXB_Y1
// bars: x [1520,1792)
#define B_X0      ((BAR_X / 8) * 8)     // 1520
#define B_NG      ((BAR_X + BAR_W - B_X0 + 7) / 8)  // 34 groups
#define MAX_NG    34
#define MAX_WORDS (MAX_NG * 3)

#define FB_WORDS  (1920 * 1080 * 3 / 8)

static const uint32_t C_BLACK = 0x000000u, C_WHITE = 0xF0F4F8u, C_TEAL = 0x22E6E6u,
                      C_TRACK = 0x303840u, C_WIN = 0x00EBEBu, C_BAR = 0x005050u;

void mbpost(const w64 *img, const int32_t *out, const w64 *bg, w64 *fb,
            uint32_t stride, int32_t ox, int32_t oy);

#endif
