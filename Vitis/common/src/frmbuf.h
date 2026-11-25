/*
 * Copyright 2025 mariobergeron.com
 *
 */

#ifndef FRMBUF_H_
#define FRMBUF_H_

#include "xparameters.h"
#include "xv_frmbufrd_l2.h"
#include "xv_frmbufwr_l2.h"

// Frmbuf struct
typedef struct {
	XV_FrmbufWr_l2 FrmbufWr;
	XV_FrmbufRd_l2 FrmbufRd;
	UINTPTR FrmbufBufrBaseAddr;
	UINTPTR FrmbufWrBufrAddr;     /* last address PROGRAMMED into frmbuf_wr (used from the
	                               * frame after next: see FrmbufWrCallback) */
	UINTPTR FrmbufWrInFlightAddr; /* address frmbuf_wr latched at its last (auto)start, i.e.
	                               * the slot it is writing now */
	UINTPTR FrmbufRdBufrAddr;
    u32     FrameStrideBytes;
    u32     FrameLines;
    u32     FrameSizeBytes;
    /* Per-capture handoff: the FrmbufWr DONE ISR (FrmbufWrCallback) sets FrameReady=1 and
     * latches the just-completed buffer into FrameDoneAddr each frame. A thread-context
     * consumer takes it via FrmbufTakeFrame() to drive per-capture inference. FrameSeq is
     * a free-running frame counter for cadence sanity checks. */
    volatile u32     FrameReady;
    volatile UINTPTR FrameDoneAddr;
    volatile u32     FrameSeq;
    /* When set, the DONE ISR stops auto-advancing the display read address — the app drives
     * it (FrmbufTakeFrame + XVFrmbufRd_SetBufferAddr), so it can skip bad frames (e.g. the
     * OV9281 wake-settling frame in FSIN-trigger mode) instead of flashing them on screen. */
    volatile u8      AppDrivesRd;
    /* Slot the app is currently displaying (trigger mode). Informational (fcapz markers):
     * the ring rotation itself keeps the shown frame safe for ~2 frame periods, because a
     * slot is only rewritten three frames after it completed. 0 = no hold (free-run). */
    volatile UINTPTR DisplayHeldAddr;
} Frmbuf;

// Main functions for use by the outside
int FrmbufWrInit(Frmbuf *Frmbuf, UINTPTR BaseAddr, UINTPTR BufrBaseAddr);
int FrmbufRdInit(Frmbuf *Frmbuf, UINTPTR BaseAddr, UINTPTR BufrBaseAddr);
int FrmbufStart(Frmbuf *Frmbuf);

/* Atomically consume a completed frame: if a new frame has landed since the last call,
 * clear the ready flag, return its buffer address via *done_addr, and return 1; else 0.
 * Safe against the FrmbufWr DONE ISR. Call from thread context (main loop). */
int FrmbufTakeFrame(Frmbuf *Frmbuf, UINTPTR *done_addr);

/* Option A background image reader: standalone full-screen v_frmbuf_rd -> v_mix master.
 * No interrupt (auto-restart free-run). Returns the row stride via *stride_out; does NOT
 * start the core (caller loads DDR then calls XVFrmbufRd_Start). */
int BgFrmbufRdInit(XV_FrmbufRd_l2 *rd, UINTPTR BaseAddr, UINTPTR BufrAddr,
                   u16 width, u16 height, u32 *stride_out);

// Callback (ISR) functions
void *FrmbufRdCallback(void *data);
void *FrmbufWrCallback(void *data);

// Stride calculator (should probably be placed in a module with other useful video calc functions)
static u32 CalcStride(XVidC_ColorFormat Cfmt,u16 AXIMMDataWidth,XVidC_VideoStream *StreamPtr);

#endif /* FRMBUF_H_ */
