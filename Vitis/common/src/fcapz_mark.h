/* fcapz_mark.h - firmware event markers for the scu200_ai_demo03_fcapz timeline capture.
 *
 * The fcapz_mark AXI GPIO (present only in the *_fcapz build) feeds 16 marker bits into the
 * fcapz_evtcap event word, next to the hardware events (FSIN, frame-buffer slots/IRQs,
 * mbdpu03 reads). Each write is one uncached 32-bit store (~10 cycles). In every other
 * build the macros compile to nothing.
 *
 * Channel A (offset 0x0) - main loop only:
 *   [0] take     a completed frame was taken (FrmbufTakeFrame) - high for the whole capture
 *   [1] dim      frame rejected by the good-frame gate (trigger mode)
 *   [2] infer    mbdpu03_infer running (crop read + CNN)
 *   [3] overlay  ROI preview / bars drawn + bg pageflip
 *   [4] roibox   ROI outline drawn into the camera frame
 *   [5] held     trigger mode: worker set DisplayHeldAddr / display to this frame
 *   [7:6] slot   camera slot being processed (0..2, 3 = none)
 * Channel B (offset 0x8) - FrmbufWrCallback (ISR) only:
 *   [0] isr      inside FrmbufWrCallback
 *   [2:1] done   slot the ISR believes was just completed (FrameDoneAddr)
 *   [4:3] next   slot the ISR programmed as the next write address
 *   [5] app_rd   AppDrivesRd (worker owns the display address)
 *   [7:6] held   DisplayHeldAddr slot (3 = none)
 * Separate channels so the ISR and the main loop never read-modify-write one register.
 */
#ifndef FCAPZ_MARK_H_
#define FCAPZ_MARK_H_

#include "xparameters.h"
#include "xil_io.h"
#include "xil_types.h"

#ifdef XPAR_FCAPZ_MARK_BASEADDR
#define FCAPZ_MARK_PRESENT 1
#define FMARK_A(v) Xil_Out32(XPAR_FCAPZ_MARK_BASEADDR + 0x0u, (u32)(v))
#define FMARK_B(v) Xil_Out32(XPAR_FCAPZ_MARK_BASEADDR + 0x8u, (u32)(v))
#else
#define FMARK_A(v) ((void)(v))
#define FMARK_B(v) ((void)(v))
#endif

#define FMA_TAKE    0x01u
#define FMA_DIM     0x02u
#define FMA_INFER   0x04u
#define FMA_OVERLAY 0x08u
#define FMA_ROIBOX  0x10u
#define FMA_HELD    0x20u
#define FMA_SLOT(s) (((u32)(s) & 3u) << 6)

#define FMB_ISR     0x01u
#define FMB_DONE(s) (((u32)(s) & 3u) << 1)
#define FMB_NEXT(s) (((u32)(s) & 3u) << 3)
#define FMB_APPRD   0x20u
#define FMB_HELD(s) (((u32)(s) & 3u) << 6)

/* Frame-ring slot of a buffer address (3 = not a slot / 0). Compare chain, no divide. */
static inline u32 fmark_slot(UINTPTR base, u32 size, UINTPTR a)
{
	if (a == 0 || a < base) return 3u;
	UINTPTR o = a - base;
	return o < size ? 0u : o < 2u * size ? 1u : o < 3u * size ? 2u : 3u;
}

#endif /* FCAPZ_MARK_H_ */
