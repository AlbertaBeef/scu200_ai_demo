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
