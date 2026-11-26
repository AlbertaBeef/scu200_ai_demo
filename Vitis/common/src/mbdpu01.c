// mbdpu01.c — MicroBlaze bare-metal driver for the mbdpu01 HLS core. See mbdpu01.h.
// SPDX-License-Identifier: Apache-2.0

#include "mbdpu01.h"
#include "xil_io.h"
#include "xil_cache.h"

void mbdpu01_init(uint32_t base)
{
    Xil_Out32(base + MBDPU01_GIE, 0x1u);   // global interrupt enable
    Xil_Out32(base + MBDPU01_IER, 0x1u);   // route ap_done to the interrupt line
}

int mbdpu01_idle(uint32_t base)
{
    return (int)((Xil_In32(base + MBDPU01_AP_CTRL) >> 2) & 0x1u);   // ap_idle
}

int mbdpu01_done(uint32_t base)
{
    return (int)((Xil_In32(base + MBDPU01_AP_CTRL) >> 1) & 0x1u);   // ap_done
}

void mbdpu01_start(uint32_t base, uint64_t wblob, uint64_t pblob, uint64_t img, uint64_t out)
{
    Xil_Out32(base + MBDPU01_WBLOB_LO, (uint32_t)(wblob & 0xFFFFFFFFu));
    Xil_Out32(base + MBDPU01_WBLOB_HI, (uint32_t)(wblob >> 32));
    Xil_Out32(base + MBDPU01_PBLOB_LO, (uint32_t)(pblob & 0xFFFFFFFFu));
    Xil_Out32(base + MBDPU01_PBLOB_HI, (uint32_t)(pblob >> 32));
    Xil_Out32(base + MBDPU01_IMG_LO,   (uint32_t)(img  & 0xFFFFFFFFu));
    Xil_Out32(base + MBDPU01_IMG_HI,   (uint32_t)(img  >> 32));
    Xil_Out32(base + MBDPU01_OUT_LO,   (uint32_t)(out  & 0xFFFFFFFFu));
    Xil_Out32(base + MBDPU01_OUT_HI,   (uint32_t)(out  >> 32));
    Xil_Out32(base + MBDPU01_AP_CTRL,  0x1u);   // ap_start (auto-clears when accepted)
}

/* Shared blocking run: flush inputs, start, poll ap_done, invalidate the out buffer.
 * Returns a pointer to the (now coherent) DDR out buffer. */
static volatile int32_t *infer_run(uint32_t base, uint64_t wblob, uint64_t pblob, uint64_t img,
                                   uint64_t out, uint32_t wbytes, uint32_t pbytes, uint32_t obytes)
{
    // The core reads these blobs from DDR via its AXI master; flush the CPU-written
    // copies so it sees fresh data. (Harmless if a blob was loaded by JTAG.)
    Xil_DCacheFlushRange((UINTPTR)wblob, wbytes);
    Xil_DCacheFlushRange((UINTPTR)pblob, pbytes);
    Xil_DCacheFlushRange((UINTPTR)img,   MBDPU01_IMG_BYTES);

    mbdpu01_start(base, wblob, pblob, img, out);
    while (!mbdpu01_done(base)) { /* poll; swap for the ap_done IRQ in the real flow */ }

    // The core wrote the result to DDR via its master; invalidate before the CPU reads.
    Xil_DCacheInvalidateRange((UINTPTR)out, obytes);
    return (volatile int32_t *)(uintptr_t)out;
}

int mbdpu01_infer(uint32_t base, uint64_t wblob, uint64_t pblob,
                  uint64_t img, uint64_t out, int32_t logits[10])
{
    volatile int32_t *o = infer_run(base, wblob, pblob, img, out,
                                    MBDPU01_WBLOB_BYTES, MBDPU01_PBLOB_BYTES, MBDPU01_OUT_BYTES);
    if (logits) for (int i = 0; i < 10; i++) logits[i] = o[i];
    return (int)o[10];   // argmax prediction
}

int mbdpu02_infer(uint32_t base, uint64_t wblob, uint64_t pblob,
                  uint64_t img, uint64_t out, int32_t logits[10], int32_t pos_x100[2])
{
    volatile int32_t *o = infer_run(base, wblob, pblob, img, out,
                                    MBDPU02_WBLOB_BYTES, MBDPU02_PBLOB_BYTES, MBDPU02_OUT_BYTES);
    if (logits) for (int i = 0; i < 10; i++) logits[i] = o[i];
    if (pos_x100) {
        // Scale lives in the param blob (written by the trainer, JTAG-loaded; never CPU-written,
        // so invalidating its line is safe and picks up a live reload_weights).
        volatile int32_t *p = (volatile int32_t *)(uintptr_t)pblob;
        Xil_DCacheInvalidateRange((UINTPTR)&p[MBDPU02_PBLOB_POSK], 8u);
        int64_t k = p[MBDPU02_PBLOB_POSK];
        int     sh = (int)p[MBDPU02_PBLOB_POSH];
        if (sh < 0 || sh > 62) sh = 0;
        pos_x100[0] = (int32_t)(((int64_t)o[11] * k) >> sh);
        pos_x100[1] = (int32_t)(((int64_t)o[12] * k) >> sh);
    }
    return (int)o[10];   // argmax prediction
}

/* ---- mbdpu03 ---------------------------------------------------------------------------- */
static uint32_t pblob_sig(uint64_t pblob)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)pblob;
    Xil_DCacheInvalidateRange((UINTPTR)pblob, MBDPU03_PBLOB_BYTES);
    uint32_t h = 2166136261u;                       /* FNV-1a over the 110 param words */
    for (uint32_t i = 0; i < MBDPU03_PBLOB_BYTES / 4u; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int mbdpu03_infer(uint32_t base, uint64_t wblob, uint64_t pblob, uint64_t img, uint64_t out,
                  uint64_t cam, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t blk_shift,
                  int32_t logits[10], int32_t pos_x100[2])
{
    static int      loaded = 0;
    static uint32_t sig = 0;
    uint32_t ctrl = 0;
    uint32_t s = pblob_sig(pblob);                  /* new params (reload_weights) -> reload */
    if (!loaded || s != sig) {
        Xil_DCacheFlushRange((UINTPTR)wblob, MBDPU03_WBLOB_BYTES);
        Xil_DCacheFlushRange((UINTPTR)pblob, MBDPU03_PBLOB_BYTES);
        ctrl |= MBDPU03_CTRL_RELOAD;
        loaded = 1; sig = s;
    }
    Xil_Out32(base + MBDPU03_CAM_LO,    (uint32_t)(cam & 0xFFFFFFFFu));
    Xil_Out32(base + MBDPU03_CAM_HI,    (uint32_t)(cam >> 32));
    Xil_Out32(base + MBDPU03_STRIDE,    stride);
    Xil_Out32(base + MBDPU03_X0,        x0);
    Xil_Out32(base + MBDPU03_Y0,        y0);
    Xil_Out32(base + MBDPU03_BLK_SHIFT, blk_shift);
    Xil_Out32(base + MBDPU03_CTRL,      ctrl);
    mbdpu01_start(base, wblob, pblob, img, out);
    while (!mbdpu01_done(base)) { }

    Xil_DCacheInvalidateRange((UINTPTR)out, MBDPU03_OUT_BYTES);
    Xil_DCacheInvalidateRange((UINTPTR)img, MBDPU03_IMG_BYTES);   /* core wrote the 112x112 input */
    volatile int32_t *o = (volatile int32_t *)(uintptr_t)out;
    if (logits) for (int i = 0; i < 10; i++) logits[i] = o[i];
    if (pos_x100) {
        volatile int32_t *p = (volatile int32_t *)(uintptr_t)pblob;   /* invalidated by pblob_sig */
        int64_t k = p[MBDPU03_PBLOB_POSK];
        int     sh = (int)p[MBDPU03_PBLOB_POSH];
        if (sh < 0 || sh > 62) sh = 0;
        pos_x100[0] = (int32_t)(((int64_t)o[11] * k) >> sh);
        pos_x100[1] = (int32_t)(((int64_t)o[12] * k) >> sh);
    }
    return (int)o[10];
}

void mbpost_start(uint32_t base, uint64_t img, uint64_t out, uint64_t bg, uint64_t fb,
                  uint32_t stride, int32_t ox, int32_t oy)
{
    Xil_Out32(base + MBPOST_IMG_LO, (uint32_t)(img & 0xFFFFFFFFu));
    Xil_Out32(base + MBPOST_IMG_HI, (uint32_t)(img >> 32));
    Xil_Out32(base + MBPOST_OUT_LO, (uint32_t)(out & 0xFFFFFFFFu));
    Xil_Out32(base + MBPOST_OUT_HI, (uint32_t)(out >> 32));
    Xil_Out32(base + MBPOST_BG_LO,  (uint32_t)(bg & 0xFFFFFFFFu));
    Xil_Out32(base + MBPOST_BG_HI,  (uint32_t)(bg >> 32));
    Xil_Out32(base + MBPOST_FB_LO,  (uint32_t)(fb & 0xFFFFFFFFu));
    Xil_Out32(base + MBPOST_FB_HI,  (uint32_t)(fb >> 32));
    Xil_Out32(base + MBPOST_STRIDE, stride);
    Xil_Out32(base + MBPOST_OX,     (uint32_t)ox);
    Xil_Out32(base + MBPOST_OY,     (uint32_t)oy);
    Xil_Out32(base + MBPOST_AP_CTRL, 0x1u);                 /* ap_start */
}

void mbpost_draw(uint32_t base, uint64_t img, uint64_t out, uint64_t bg, uint64_t fb,
                 uint32_t stride, int32_t ox, int32_t oy)
{
    mbpost_start(base, img, out, bg, fb, stride, ox, oy);
    while (((Xil_In32(base + MBPOST_AP_CTRL) >> 1) & 0x1u) == 0u) { }   /* ap_done */
}
