/* 
 * Copyright 2025 mariobergeron.com
 * 
 * The functions in this module setup the Frame buffer Rd and Wr for pass-through
 * such that video frames coming into the FrmbufWr are written to memory and then
 * immediately read back from memory by the FrmbufRd. The init function is given
 * a pointer to the Frmbuf struct which contains the Rd and Wr cores, as well
 * as the variables for keeping track of the read/write addresses. Each
 * Frmbuf is given it's own "base address" at which it will store three frames.
 * The callback functions rotate the read/write addresses such that the next
 * frame to read is the one that we just wrote, and the pointer to the next
 * write location is incremented and wrapped around when needed.
 *
******************************************************************************/

#include "frmbuf.h"
#include "fcapz_mark.h"
#include "xparameters.h"
#include "config.h"
#include "xinterrupt_wrap.h"
#include "xil_exception.h"   /* Xil_ExceptionDisable/Enable: portable ISR guard (MB + ARM) */

#define COLOR_FORMAT_INDEX 7 // RGB8 (frmbuf IP only supports RGB8, not RGBX8)

/* Frame-buffer stride alignment in bytes = 2 * (display samples-per-clock) * 4.
 * SCU200 runs the read/display frame buffer at 4 ppc, so 32. Override at build
 * time if the display ppc changes. */
#ifndef FRMBUF_STRIDE_ALIGN
#define FRMBUF_STRIDE_ALIGN 32
#endif

#define NUM_TEST_FORMATS 26

//mapping between memory and streaming video formats
typedef struct {
  XVidC_ColorFormat MemFormat;
  XVidC_ColorFormat StreamFormat;
  u16 FormatBits;
} VideoFormats;

VideoFormats ColorFormats[NUM_TEST_FORMATS] =
{
  //memory format            stream format        bits per component
  {XVIDC_CSF_MEM_RGBX8,      XVIDC_CSF_RGB,       8},
  {XVIDC_CSF_MEM_YUVX8,      XVIDC_CSF_YCRCB_444, 8},
  {XVIDC_CSF_MEM_YUYV8,      XVIDC_CSF_YCRCB_422, 8},
  {XVIDC_CSF_MEM_RGBX10,     XVIDC_CSF_RGB,       10},
  {XVIDC_CSF_MEM_YUVX10,     XVIDC_CSF_YCRCB_444, 10},
  {XVIDC_CSF_MEM_Y_UV8,      XVIDC_CSF_YCRCB_422, 8},
  {XVIDC_CSF_MEM_Y_UV8_420,  XVIDC_CSF_YCRCB_420, 8},
  {XVIDC_CSF_MEM_RGB8,       XVIDC_CSF_RGB,       8},
  {XVIDC_CSF_MEM_YUV8,       XVIDC_CSF_YCRCB_444, 8},
  {XVIDC_CSF_MEM_Y_UV10,     XVIDC_CSF_YCRCB_422, 10},
  {XVIDC_CSF_MEM_Y_UV10_420, XVIDC_CSF_YCRCB_420, 10},
  {XVIDC_CSF_MEM_Y8,         XVIDC_CSF_YONLY, 8},
  {XVIDC_CSF_MEM_Y10,        XVIDC_CSF_YONLY, 10},
  {XVIDC_CSF_MEM_BGRX8,      XVIDC_CSF_RGB,       8},
  {XVIDC_CSF_MEM_UYVY8,      XVIDC_CSF_YCRCB_422, 8},
  {XVIDC_CSF_MEM_BGR8,       XVIDC_CSF_RGB,       8},
  {XVIDC_CSF_MEM_RGBX12,     XVIDC_CSF_RGB,       12},
  {XVIDC_CSF_MEM_RGB16,      XVIDC_CSF_RGB,       16},
  {XVIDC_CSF_MEM_YUVX12,     XVIDC_CSF_YCRCB_444, 12},
  {XVIDC_CSF_MEM_YUV16,      XVIDC_CSF_YCRCB_444, 16},
  {XVIDC_CSF_MEM_Y_UV12,     XVIDC_CSF_YCRCB_422, 12},
  {XVIDC_CSF_MEM_Y_UV16,     XVIDC_CSF_YCRCB_422, 16},
  {XVIDC_CSF_MEM_Y_UV12_420, XVIDC_CSF_YCRCB_420, 12},
  {XVIDC_CSF_MEM_Y_UV16_420, XVIDC_CSF_YCRCB_420, 16},
  {XVIDC_CSF_MEM_Y12,        XVIDC_CSF_YONLY, 12},
  {XVIDC_CSF_MEM_Y16,        XVIDC_CSF_YONLY, 16}
};

/*
 * Initialize Frame Buffer Write
 * - Enable interrupts
 * - Assign the callback function
 * - Enable auto-restart mode
 * - Sets the stream parameters to align with the VProc SS output stream
 */
int FrmbufWrInit(Frmbuf *Frmbuf, UINTPTR BaseAddr, UINTPTR BufrBaseAddr)
{
	XVidC_VideoStream FrmbufWrStream;
	int FrmbufWrStride;
	XVidC_VideoTiming const *TimingPtr;
	XVidC_ColorFormat Cfmt;
	int Status;
    // Init Frame buffers
	Status = XVFrmbufWr_Initialize(&(Frmbuf->FrmbufWr), BaseAddr);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR:: Frame Buffer Write initialization failed\r\n");
		return(XST_FAILURE);
	}

	// Setup interrupt system
    Status = XSetupInterruptSystem(&(Frmbuf->FrmbufWr), &XVFrmbufWr_InterruptHandler,
				       Frmbuf->FrmbufWr.FrmbufWr.Config.IntrId,
				       Frmbuf->FrmbufWr.FrmbufWr.Config.IntrParent,
				       XINTERRUPT_DEFAULT_PRIORITY);

	if (Status != XST_SUCCESS) {
		xil_printf("ERR:: Frame Buffer Write interrupt connect failed!\r\n");
		return XST_FAILURE;
	}
	// Frame Buffer Write video stream properties
	FrmbufWrStream.PixPerClk  = Frmbuf->FrmbufWr.FrmbufWr.Config.PixPerClk;
	FrmbufWrStream.ColorDepth = Frmbuf->FrmbufWr.FrmbufWr.Config.MaxDataWidth;
	Cfmt = ColorFormats[COLOR_FORMAT_INDEX].MemFormat;
	FrmbufWrStream.ColorFormatId = ColorFormats[COLOR_FORMAT_INDEX].StreamFormat;
	FrmbufWrStream.VmId = XVidC_GetVideoModeId(VPROC_WIDTH_OUT,VPROC_HEIGHT_OUT,VPROC_FRAMERATE_OUT,FALSE);
    TimingPtr = XVidC_GetTimingInfo(FrmbufWrStream.VmId);
    FrmbufWrStream.Timing = *TimingPtr;
    FrmbufWrStream.FrameRate = XVidC_GetFrameRate(FrmbufWrStream.VmId);
    FrmbufWrStride = CalcStride(Cfmt,Frmbuf->FrmbufWr.FrmbufWr.Config.AXIMMDataWidth,&FrmbufWrStream);

	// Assign the Frame buffer callbacks
	XVFrmbufWr_SetCallback(&(Frmbuf->FrmbufWr), XVFRMBUFWR_HANDLER_DONE, FrmbufWrCallback,
			(void *)Frmbuf);

	// Configure Frame Buffers
	Frmbuf->FrmbufWrBufrAddr = BufrBaseAddr;
	Frmbuf->FrmbufBufrBaseAddr = BufrBaseAddr;
	Status = XVFrmbufWr_SetMemFormat(&(Frmbuf->FrmbufWr), FrmbufWrStride, Cfmt, &FrmbufWrStream);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Unable to configure Frame Buffer Write\r\n");
		return(XST_FAILURE);
	}
	Status = XVFrmbufWr_SetBufferAddr(&(Frmbuf->FrmbufWr), Frmbuf->FrmbufWrBufrAddr);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Unable to configure Frame Buffer Write buffer address\r\n");
		return(XST_FAILURE);
	}

    Frmbuf->FrameStrideBytes = FrmbufWrStride;
    Frmbuf->FrameLines       = FrmbufWrStream.Timing.VActive;
    Frmbuf->FrameSizeBytes   = Frmbuf->FrameStrideBytes * Frmbuf->FrameLines;

    // Per-capture handoff state (populated by the DONE ISR, consumed by FrmbufTakeFrame)
    Frmbuf->FrameReady    = 0;
    Frmbuf->FrameDoneAddr = 0;
    Frmbuf->FrameSeq      = 0;
    Frmbuf->AppDrivesRd   = 0;   // ISR drives the display by default (free-run)
    Frmbuf->DisplayHeldAddr = 0; // no held slot until the app takes over the display

	// Enable Interrupt
	XVFrmbufWr_InterruptEnable(&(Frmbuf->FrmbufWr), XVFRMBUFWR_IRQ_DONE_MASK);

	// Frame Buffers in Auto-restart mode
	XV_frmbufwr_EnableAutoRestart(&(Frmbuf->FrmbufWr.FrmbufWr));

	return(XST_SUCCESS);
}

/*
 * Initialize Frame Buffer Read
 * - Enable interrupts
 * - Assign the callback function
 * - Enable auto-restart mode
 * - Sets the stream parameters to align with the VProc SS output stream
 */
int FrmbufRdInit(Frmbuf *Frmbuf, UINTPTR BaseAddr, UINTPTR BufrBaseAddr)
{
	XVidC_VideoStream FrmbufRdStream;
	int FrmbufRdStride;
	XVidC_VideoTiming const *TimingPtr;
	XVidC_ColorFormat Cfmt;
	int Status;
    // Init Frame buffers
	Status = XVFrmbufRd_Initialize(&(Frmbuf->FrmbufRd), BaseAddr);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR:: Frame Buffer Read initialization failed\r\n");
		return(XST_FAILURE);
	}

	// Setup interrupt system
    Status = XSetupInterruptSystem(&(Frmbuf->FrmbufRd), &XVFrmbufRd_InterruptHandler,
				       Frmbuf->FrmbufRd.FrmbufRd.Config.IntrId,
				       Frmbuf->FrmbufRd.FrmbufRd.Config.IntrParent,
				       XINTERRUPT_DEFAULT_PRIORITY);

	if (Status != XST_SUCCESS) {
		xil_printf("ERR:: Frame Buffer Read interrupt connect failed!\r\n");
		return XST_FAILURE;
	}
	// Frame Buffer Read video stream properties
	FrmbufRdStream.PixPerClk  = Frmbuf->FrmbufRd.FrmbufRd.Config.PixPerClk;
	FrmbufRdStream.ColorDepth = Frmbuf->FrmbufRd.FrmbufRd.Config.MaxDataWidth;
	Cfmt = ColorFormats[COLOR_FORMAT_INDEX].MemFormat;
	FrmbufRdStream.ColorFormatId = ColorFormats[COLOR_FORMAT_INDEX].StreamFormat;
	FrmbufRdStream.VmId = XVidC_GetVideoModeId(VPROC_WIDTH_OUT,VPROC_HEIGHT_OUT,VPROC_FRAMERATE_OUT,FALSE);
    TimingPtr = XVidC_GetTimingInfo(FrmbufRdStream.VmId);
    FrmbufRdStream.Timing = *TimingPtr;
    FrmbufRdStream.FrameRate = XVidC_GetFrameRate(FrmbufRdStream.VmId);
    FrmbufRdStride = CalcStride(Cfmt,Frmbuf->FrmbufRd.FrmbufRd.Config.AXIMMDataWidth,&FrmbufRdStream);

	// Assign the Frame buffer callbacks
	XVFrmbufRd_SetCallback(&(Frmbuf->FrmbufRd), XVFRMBUFRD_HANDLER_DONE, FrmbufRdCallback,
			(void *)Frmbuf);

	// Configure Frame Buffers
	Frmbuf->FrmbufRdBufrAddr = BufrBaseAddr;
	Frmbuf->FrmbufBufrBaseAddr = BufrBaseAddr;
	Status = XVFrmbufRd_SetMemFormat(&(Frmbuf->FrmbufRd), FrmbufRdStride, Cfmt, &FrmbufRdStream);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Unable to configure Frame Buffer Read 0x%X\r\n",Status);
		return(XST_FAILURE);
	}
	Status = XVFrmbufRd_SetBufferAddr(&(Frmbuf->FrmbufRd), Frmbuf->FrmbufRdBufrAddr);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Unable to configure Frame Buffer Read buffer address\r\n");
		return(XST_FAILURE);
	}

	// Enable Interrupt
	XVFrmbufRd_InterruptEnable(&(Frmbuf->FrmbufRd), XVFRMBUFRD_IRQ_DONE_MASK);

	// Frame Buffers in Auto-restart mode
	XV_frmbufrd_EnableAutoRestart(&(Frmbuf->FrmbufRd.FrmbufRd));

	return(XST_SUCCESS);
}

/*
 * Start the Frame Buffers
 */
int FrmbufStart(Frmbuf *Frmbuf)
{
	XVFrmbufWr_Start(&(Frmbuf->FrmbufWr));
	XVFrmbufRd_Start(&(Frmbuf->FrmbufRd));
	return(XST_SUCCESS);
}

/*
 * BgFrmbufRdInit - Option A background-image reader. Configures a standalone
 * v_frmbuf_rd (bg_frmbuf_rd) to stream a full-screen static image from DDR into the
 * v_mix master/background layer, replacing the v_tpg cross-hatch. No interrupt is
 * used (the core free-runs via auto-restart). The row stride is returned via
 * *stride_out so the caller can lay out the image bytes. Does NOT start the core;
 * the caller fills DDR (and flushes cache) first, then calls XVFrmbufRd_Start().
 */
int BgFrmbufRdInit(XV_FrmbufRd_l2 *rd, UINTPTR BaseAddr, UINTPTR BufrAddr,
                   u16 width, u16 height, u32 *stride_out)
{
	XVidC_VideoStream s;
	XVidC_VideoTiming const *TimingPtr;
	XVidC_ColorFormat Cfmt;
	int stride, Status;

	Status = XVFrmbufRd_Initialize(rd, BaseAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR:: bg Frame Buffer Read init failed\r\n");
		return (XST_FAILURE);
	}

	s.PixPerClk     = rd->FrmbufRd.Config.PixPerClk;
	s.ColorDepth    = rd->FrmbufRd.Config.MaxDataWidth;
	Cfmt            = ColorFormats[COLOR_FORMAT_INDEX].MemFormat;
	s.ColorFormatId = ColorFormats[COLOR_FORMAT_INDEX].StreamFormat;
	s.VmId          = XVidC_GetVideoModeId(width, height, VMODE_FRAMERATE, FALSE);
	TimingPtr       = XVidC_GetTimingInfo(s.VmId);
	s.Timing        = *TimingPtr;
	s.FrameRate     = XVidC_GetFrameRate(s.VmId);
	stride          = CalcStride(Cfmt, rd->FrmbufRd.Config.AXIMMDataWidth, &s);

	Status = XVFrmbufRd_SetMemFormat(rd, stride, Cfmt, &s);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: bg Frame Buffer Read SetMemFormat 0x%X\r\n", Status);
		return (XST_FAILURE);
	}
	Status = XVFrmbufRd_SetBufferAddr(rd, BufrAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: bg Frame Buffer Read SetBufferAddr\r\n");
		return (XST_FAILURE);
	}

	XV_frmbufrd_EnableAutoRestart(&(rd->FrmbufRd));
	if (stride_out) *stride_out = (u32)stride;
	return (XST_SUCCESS);
}


void *FrmbufRdCallback(void *data)
{
	Frmbuf *frmbuf = (Frmbuf *)data;
	XV_FrmbufRd_l2 *frmbufrd = &(frmbuf->FrmbufRd);
	// Nothing to do - Frame Buffer Read is configured for auto-restart
	(void)frmbuf; (void)frmbufrd;
    return NULL;
}

#define FRAME_MAX 3

#ifdef FCAPZ_MARK_PRESENT
static u32 s_fmark_b = 0;   /* last ISR marker word (fcapz build only) */
#endif

void *FrmbufWrCallback(void *data)
{
	Frmbuf *frmbuf = (Frmbuf *)data;
	XV_FrmbufWr_l2 *frmbufwr = &(frmbuf->FrmbufWr);
	XV_FrmbufRd_l2 *frmbufrd = &(frmbuf->FrmbufRd);
	u32 Status;

    // The frame we just finished writing
    UINTPTR completed = frmbuf->FrmbufWrBufrAddr;
#ifdef FCAPZ_MARK_PRESENT
    FMARK_B(s_fmark_b | FMB_ISR);
#endif

    // Per-capture handoff: latch the just-completed buffer and raise the ready flag for the
    // thread-context worker (FrmbufTakeFrame). Three aligned 32-bit stores; ISR stays short.
    frmbuf->FrameDoneAddr = completed;
    frmbuf->FrameSeq++;
    frmbuf->FrameReady = 1;

    // Advance write pointer and wrap correctly
    UINTPTR end = frmbuf->FrmbufBufrBaseAddr + (frmbuf->FrameSizeBytes * FRAME_MAX);
    UINTPTR next_wr = frmbuf->FrmbufWrBufrAddr + frmbuf->FrameSizeBytes;
    if (next_wr >= end) next_wr = frmbuf->FrmbufBufrBaseAddr;
    // Skip the slot the app is holding on screen (trigger mode), so the write ring never
    // overwrites the displayed frame. One skip suffices for the 3-slot ring (one held).
    if (frmbuf->DisplayHeldAddr && next_wr == frmbuf->DisplayHeldAddr) {
        next_wr += frmbuf->FrameSizeBytes;
        if (next_wr >= end) next_wr = frmbuf->FrmbufBufrBaseAddr;
    }
    frmbuf->FrmbufWrBufrAddr = next_wr;

	// Set the buffer address to write the next frame
    Status = XVFrmbufWr_SetBufferAddr(frmbufwr, frmbuf->FrmbufWrBufrAddr);
    if(Status != XST_SUCCESS) {
        xil_printf("ERROR: Unable to set Frame Buffer Write address\r\n");
    }

	// Advance the display to the just-completed frame — UNLESS the app has taken over the
	// read address (AppDrivesRd), so it can skip bad frames instead of flashing them.
	if (!frmbuf->AppDrivesRd) {
		frmbuf->FrmbufRdBufrAddr = completed;
		Status = XVFrmbufRd_SetBufferAddr(frmbufrd, frmbuf->FrmbufRdBufrAddr);
		if(Status != XST_SUCCESS) {
			xil_printf("ERROR: Unable to set Frame Buffer Read address\r\n");
		}
	}

#ifdef FCAPZ_MARK_PRESENT
    {
        UINTPTR b = frmbuf->FrmbufBufrBaseAddr; u32 sz = frmbuf->FrameSizeBytes;
        s_fmark_b = FMB_DONE(fmark_slot(b, sz, completed)) |
                    FMB_NEXT(fmark_slot(b, sz, frmbuf->FrmbufWrBufrAddr)) |
                    (frmbuf->AppDrivesRd ? FMB_APPRD : 0u) |
                    FMB_HELD(fmark_slot(b, sz, frmbuf->DisplayHeldAddr));
        FMARK_B(s_fmark_b);
    }
#endif
    return NULL;
}

/* Consume a completed frame if one has landed since the last call (see frmbuf.h).
 * Brief interrupt lock makes the flag-clear + address-latch atomic vs. FrmbufWrCallback. */
int FrmbufTakeFrame(Frmbuf *Frmbuf, UINTPTR *done_addr)
{
    int got = 0;
    Xil_ExceptionDisable();
    if (Frmbuf->FrameReady) {
        Frmbuf->FrameReady = 0;
        if (done_addr) *done_addr = Frmbuf->FrameDoneAddr;
        got = 1;
    }
    Xil_ExceptionEnable();
    return got;
}

/*****************************************************************************/
/**
 * This function calculates the stride
 *
 * @returns stride in bytes
 *
 *****************************************************************************/
static u32 CalcStride(XVidC_ColorFormat Cfmt,
						u16 AXIMMDataWidth,
						XVidC_VideoStream *StreamPtr)
{
  u32 stride;
  int width = StreamPtr->Timing.HActive;
  u16 MMWidthBytes = AXIMMDataWidth/8;
  u8 bpp_numerator;
  u8 bpp_denominator = 1;

  switch (Cfmt) {
    case XVIDC_CSF_MEM_Y_UV10:
    case XVIDC_CSF_MEM_Y_UV10_420:
    case XVIDC_CSF_MEM_Y10:
      /* 4 bytes per 3 pixels (Y_UV10, Y_UV10_420, Y10) */
      bpp_numerator = 4;
      bpp_denominator = 3;
      break;
    case XVIDC_CSF_MEM_Y_UV8:
    case XVIDC_CSF_MEM_Y_UV8_420:
    case  XVIDC_CSF_MEM_Y8:
      /* 1 byte per pixel (Y_UV8, Y_UV8_420, Y8) */
      bpp_numerator = 1;
      break;
    case XVIDC_CSF_MEM_RGB8:
    case  XVIDC_CSF_MEM_YUV8:
    case XVIDC_CSF_MEM_BGR8:
      /* 3 bytes per pixel (RGB8, YUV8, BGR8) */
      bpp_numerator = 3;
      break;
    case XVIDC_CSF_MEM_RGBX12:
    case  XVIDC_CSF_MEM_YUVX12:
      /* 5 bytes per pixel (RGBX12, YUVX12) */
      bpp_numerator = 5;
      break;
    case XVIDC_CSF_MEM_Y_UV12:
    case  XVIDC_CSF_MEM_Y_UV12_420:
    case XVIDC_CSF_MEM_Y12:
      /* 3 bytes per 2 pixels (Y_UV12, Y_UV12_420, Y12) */
      bpp_numerator = 3;
      bpp_denominator = 2;
      break;
    case XVIDC_CSF_MEM_RGB16:
    case XVIDC_CSF_MEM_YUV16:
      /* 6 bytes per pixel (RGB16, YUV16) */
      bpp_numerator = 6;
      break;
    case XVIDC_CSF_MEM_YUYV8:
    case XVIDC_CSF_MEM_UYVY8:
    case XVIDC_CSF_MEM_Y_UV16:
    case XVIDC_CSF_MEM_Y_UV16_420:
    case XVIDC_CSF_MEM_Y16:
      /* 2 bytes per pixel (YUYV8, UYVY8, Y_UV16, Y_UV16_420, Y16) */
      bpp_numerator = 2;
      break;
    default:
      /* 4 bytes per pixel */
      bpp_numerator = 4;
  }
  /* The frame-buffer RD/WR drivers require the stride to be a multiple of 32
   * bytes on this build (the 4-ppc read side rejects anything else with
   * error 0x1001), so align up to 32. This is NOT the source of the ~3/4
   * wrap seam - a stride of 2160 was tried and rejected by the driver; 2176
   * (32-aligned) is the required value and the round trip is consistent. */
  u16 align = MMWidthBytes;
  if (align < FRMBUF_STRIDE_ALIGN)
    align = FRMBUF_STRIDE_ALIGN;

  stride = ((((width * bpp_numerator) / bpp_denominator) +
    align - 1) / align) * align;

  return(stride);
}

