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

int mbdpu01_infer(uint32_t base, uint64_t wblob, uint64_t pblob,
                  uint64_t img, uint64_t out, int32_t logits[10])
{
    // The core reads these blobs from DDR via its AXI master; flush the CPU-written
    // copies so it sees fresh data. (Harmless if a blob was loaded by JTAG.)
    Xil_DCacheFlushRange((UINTPTR)wblob, MBDPU01_WBLOB_BYTES);
    Xil_DCacheFlushRange((UINTPTR)pblob, MBDPU01_PBLOB_BYTES);
    Xil_DCacheFlushRange((UINTPTR)img,   MBDPU01_IMG_BYTES);

    mbdpu01_start(base, wblob, pblob, img, out);
    while (!mbdpu01_done(base)) { /* poll; swap for the ap_done IRQ in the real flow */ }

    // The core wrote the result to DDR via its master; invalidate before the CPU reads.
    Xil_DCacheInvalidateRange((UINTPTR)out, MBDPU01_OUT_BYTES);
    volatile int32_t *o = (volatile int32_t *)(uintptr_t)out;
    if (logits) for (int i = 0; i < 10; i++) logits[i] = o[i];
    return (int)o[10];   // argmax prediction
}
