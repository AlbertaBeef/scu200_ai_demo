/*
 * Copyright 2025 mariobergeron.com
 *
 * The functions in this module allow for initialization of the video pipe and starting of
 * the video pipe.
 */

#include "xstatus.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "i2c.h"
#include "xiic.h"
#include "xiic_l.h"
#include "sleep.h"
#include "xv_frmbufrd_l2.h"
#include "xv_frmbufwr_l2.h"
#include "pipe.h"   /* pulls in the ISP driver headers only when PIPE_HAS_ISP */
#include "math.h"
#include "config.h"

/* Stretch the camera I2C bus timing.
 *
 * The SCU200 has no external pull-up resistors on MIPIn_I2C_SCL/SDA (they run
 * straight from the FPGA (sch. p32) to the camera connector (sch. p22) with
 * nothing in between), and the RPi camera module relies on the host board to
 * supply them. So the only pull-up is the FPGA's internal one, which is weak
 * (~30-50 kohm). That holds the lines high at DC, but the RC rise time against
 * the ribbon capacitance is too slow for the default 100 kHz - SDA hasn't
 * settled by the time the master samples the ACK.
 *
 * The AXI IIC timing registers are runtime-writable, so stretch every interval
 * to drop the bus to ~5 kHz and give the edges time to settle.
 *
 * NOTE: XIic_Reset() - which IicAxiWrite/IicAxiRead call on any NACK or
 * timeout - restores the defaults, so this must be re-applied before every
 * transaction we care about, not just once at init.
 */
#define IIC_TIMING_STRETCH 20

static const struct { u32 off; u32 def; } iic_tmg[] = {
	{0x128, 570},	/* TSUSTA */
	{0x12C, 500},	/* TSUSTO */
	{0x130, 430},	/* THDSTA */
	{0x134,  55},	/* TSUDAT */
	{0x138, 500},	/* TBUF   */
	{0x13C, 493},	/* THIGH  */
	{0x140, 493},	/* TLOW   */
};

/* The timing registers are only 10 bits wide (verified by writing 0xFFFFFFFF
 * and reading back 1023), so a naive multiply silently truncates - x20 of 493
 * wraps to 644 and actually speeds the bus up slightly. Clamp instead. That
 * caps us at ~48 kHz, about 2x slower than the 100 kHz default. */
#define IIC_TMG_MAX 1023

static void iic_slow_down(UINTPTR base)
{
	for (unsigned i = 0; i < sizeof(iic_tmg)/sizeof(iic_tmg[0]); i++) {
		u32 v = iic_tmg[i].def * IIC_TIMING_STRETCH;
		if (v > IIC_TMG_MAX) v = IIC_TMG_MAX;
		Xil_Out32(base + iic_tmg[i].off, v);
	}
	/* TSUDAT must stay comfortably inside TLOW, so don't max it out. */
	Xil_Out32(base + 0x134, 200);
}

extern volatile int IicAxiTraceEvents;

/* Ask the core to become bus master, which drives a START (SDA falling while
 * SCL is high). The core reports that back as bus-busy in SR[2]. If busy never
 * appears, no START is possible because the lines are being held low - the
 * signature of a device clamping the bus (e.g. an unpowered slave pulling
 * SDA/SCL down through its ESD diodes) rather than simply not answering.
 * Bounded, unlike XIic_Send, so a stuck bus can't hang us. */
static int iic_bus_can_start(UINTPTR b)
{
	int busy = 0;

	/* Drive it the way XIic_Send does: address into the TX register first,
	 * then MSMS|TX. The core only emits the START once it has something to
	 * clock out, so asserting MSMS alone can under-report. */
	Xil_Out32(b + 0x100, 0x02);		/* hold TX FIFO in reset    */
	Xil_Out32(b + 0x100, 0x00);
	Xil_Out32(b + 0x108, 0x10 << 1);	/* slave 0x10, write        */
	Xil_Out32(b + 0x100, 0x01 | 0x04 | 0x08);	/* EN | MSMS | TX   */
	for (int i = 0; i < 1000; i++) {	/* up to ~100 ms            */
		if (Xil_In32(b + 0x104) & 0x04) { busy = 1; break; }
		usleep(100);
	}

	/* Release properly. Clearing MSMS while the core is still enabled emits
	 * a STOP; just writing CR=0 abandons the bus mid-transaction, which made
	 * the NEXT call report a false STUCK. */
	Xil_Out32(b + 0x100, 0x01 | 0x08);	/* EN | TX, MSMS low -> STOP */
	for (int i = 0; i < 200; i++) {		/* wait for the bus to idle  */
		if (!(Xil_In32(b + 0x104) & 0x04)) break;
		usleep(100);
	}
	Xil_Out32(b + 0x100, 0x02);		/* flush the TX FIFO         */
	Xil_Out32(b + 0x100, 0x00);		/* disable                   */
	usleep(2000);
	return busy;
}

/* Bounded, polled 1-byte read probe in dynamic mode - no interrupts, so it
 * cannot hang like the driver's ISR path. Returns 1 if the addressed device
 * ACKed (a byte landed in the RX FIFO), 0 otherwise. */
static int iic_poll_probe(UINTPTR b, u8 addr)
{
	int acked = 0;

	Xil_Out32(b + 0x100, 0x02);		/* TX FIFO reset            */
	Xil_Out32(b + 0x100, 0x00);
	Xil_Out32(b + 0x020, Xil_In32(b + 0x020));	/* clear ISR (w1c)  */
	Xil_Out32(b + 0x100, 0x01);		/* enable                   */
	/* dynamic-mode read: START+addr+R, then STOP + 1 byte */
	Xil_Out32(b + 0x108, 0x100 | ((u32)addr << 1) | 0x1);
	Xil_Out32(b + 0x108, 0x200 | 0x1);

	for (int i = 0; i < 500; i++) {		/* up to ~50 ms             */
		u32 sr = Xil_In32(b + 0x104);
		if (!(sr & 0x40)) { acked = 1; break; }	/* RX FIFO not empty */
		if (Xil_In32(b + 0x020) & 0x02) break;	/* TX_ERROR / NACK   */
		usleep(100);
	}

	Xil_Out32(b + 0x100, 0x02);		/* flush                    */
	Xil_Out32(b + 0x100, 0x00);		/* disable                  */
	usleep(2000);
	return acked;
}

/*
 * Initialize the video pipe
 */
int pipe_init(VideoPipe *pipe, VideoPipeBaseAddr *baseaddr)
{
#ifdef PIPE_HAS_ISP
	XVprocSs_Config *VprocSsConfigPtr;
#endif
	int Status;
    
	/*
	 * Initialize the GPIO driver
	 */
	Status = XGpio_Initialize(&(pipe->Gpio), baseaddr->Gpio);
	if (Status != XST_SUCCESS) {
		xil_printf("Gpio Initialization Failed\r\n");
		return XST_FAILURE;
	}

	// Set GPIO directions (1=input, 0=output)
	XGpio_SetDataDirection(&(pipe->Gpio), 1, 0);
	/* Clean power CYCLE of the camera. IO0 = <MIPIn>_PWR_EN (TPS22950 load
	 * switch for the 3V3 rail), IO1 = <MIPIn>_LED_EN_XCLK. On an FPGA
	 * reprogram without a board power-cycle the sensor can be left in an
	 * odd state, so force the rail OFF, let it discharge, then ON with a
	 * generous settle - the IMX477 will not ACK on I2C until its rail is
	 * stable. (Empirically the sensor only answered after such a cycle.) */
	XGpio_DiscreteWrite(&(pipe->Gpio), 1, 0);           /* all off      */
	usleep(150000);                                     /* 150 ms drain */
	XGpio_DiscreteWrite(&(pipe->Gpio), 1,
			    GPIO_CAM_IO0_MASK | GPIO_CAM_IO1_MASK); /* pwr + xclk on */
	usleep(300000);   /* 300 ms camera power-up settle */

	/*
	 * Initialize the IIC for communication with camera
	 */
	u8 iic_id;
	Status = IicAxiInit(&(pipe->Iic),baseaddr->Iic,&iic_id);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the I2C\n\r");
		return XST_FAILURE;
	}

	/* Slow the camera I2C bus down (the SCU200 has only weak internal
	 * pull-ups on the camera buses) and do a quick polled ACK sanity check.
	 * The sensor drivers use the polled path too - the axi_iic interrupt is
	 * unreliable on these buses, so the interrupt-driven scan/read would
	 * miss a device that is actually present. */
	iic_slow_down(baseaddr->Iic);
	xil_printf("  I2C(bus 0x%08x) polled ACK: 0x10(IMX219)=%s  0x1A(IMX477)=%s\r\n",
		   (unsigned)baseaddr->Iic,
		   iic_poll_probe(baseaddr->Iic, 0x10) ? "ACK" : "-",
		   iic_poll_probe(baseaddr->Iic, 0x1A) ? "ACK" : "-");

	/* Full-range 7-bit scan (0x08..0x77) to locate an unknown sensor / Arducam
	 * bridge address (e.g. AR0234 at a non-default SADDR strap, or a bridge ~0x0C).
	 * iic_poll_probe leaves the core mid-transaction, so RESET the core after every
	 * probe (else it locks up and false-ACKs a contiguous block). */
	{
		int n_found = 0;
		xil_printf("  I2C(bus 0x%08x) full scan:", (unsigned)baseaddr->Iic);
		for (u8 addr = 0x08; addr <= 0x77; addr++) {
			int ack = iic_poll_probe(baseaddr->Iic, addr);
			IicReset(iic_id);      /* clean bus before the next probe */
			usleep(500);
			if (ack) { xil_printf(" 0x%02x", addr); n_found++; }
		}
		xil_printf("%s\r\n", n_found ? "" : "  (nothing ACKed)");
	}

	/* The bounded polled probe above leaves the core mid-transaction (it
	 * exits as soon as it sees the ACK, without draining/STOPping). Reset
	 * the core so the sensor driver's XIic_Send finds an idle bus - it waits
	 * for bus-not-busy without a timeout and would otherwise hang. */
	IicReset(iic_id);
	usleep(2000);

	/*
	 * Initialize the camera
	 * This function will initialize the Camera container and try to communicate with the image sensor
	 * via the I2C bus so that we know what model it is and thus configure the Sensor Demosaic
	 * accordingly.
	 */
	iic_slow_down(baseaddr->Iic);
	Status = rpi_cam_init(&(pipe->Camera),iic_id,&(pipe->Gpio),GPIO_CAM_IO0_MASK);
	if(Status == XST_FAILURE) {
		pipe->IsConnected = FALSE;
		return(XST_FAILURE);
	}

	// Disable the camera while we setup the video pipe and until we use the pipe_start_camera function
	XGpio_DiscreteClear(&(pipe->Gpio), 1, GPIO_CAM_IO0_MASK);

	/*
	 * Frame Buffer Wr/Rd initialization and config
	 */
    pipe_reset_deassert(pipe,GPIO_CAM_FRMBUFWR_RST_N_MASK);
	Status = FrmbufWrInit(&(pipe->Frmbuf),baseaddr->FrmbufWr,baseaddr->FrmbufBufrBaseAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the Frame Buffer Write\n\r");
		return XST_FAILURE;
	}
    pipe_reset_deassert(pipe,GPIO_CAM_FRMBUFRD_RST_N_MASK);
	Status = FrmbufRdInit(&(pipe->Frmbuf),baseaddr->FrmbufRd,baseaddr->FrmbufBufrBaseAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the Frame Buffer Read\n\r");
		return XST_FAILURE;
	}

#ifdef PIPE_HAS_ISP
	/*
	 * Video Processor Subsystem initialization and config
	 */
    pipe_reset_deassert(pipe,GPIO_CAM_VPROC_RST_N_MASK);
	VprocSsConfigPtr = XVprocSs_LookupConfig(baseaddr->Vproc);
	if(VprocSsConfigPtr == NULL) {
		xil_printf("ERROR: Video Processor Subsystem device not found\r\n");
		return(XST_FAILURE);
	}
	// Start capturing event log
	XVprocSs_LogReset(&(pipe->Vproc));
	Status = XVprocSs_CfgInitialize(&(pipe->Vproc),
			                        VprocSsConfigPtr,
			                        VprocSsConfigPtr->BaseAddress);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Video Processing Subsystem Init. error\n\r");
		return(XST_FAILURE);
	}

	// Configure the Video Processing Subsystem INPUT stream parameters
	XVidC_VideoMode resIdIn = XVidC_GetVideoModeId(CAM_WIDTH,CAM_HEIGHT,VMODE_FRAMERATE,FALSE);
	XVidC_VideoTiming const *TimingInPtr = XVidC_GetTimingInfo(resIdIn);
	XVidC_VideoStream StreamIn;
	if (TimingInPtr == NULL) {
		/* Non-standard input resolution (e.g. 960x1080 - the bandwidth-limited
		 * 2-lane crop). Base the timing on 1920x1080 (its 1080-line vertical
		 * timing is exactly right) and override the horizontal active width.
		 * CRITICAL: HTotal must be made consistent with the new HActive
		 * (HActive + blanking). Leaving the 1920-wide HTotal (2200) with a
		 * 960 HActive gives the scaler an impossible line geometry and it
		 * wraps each line ~3/4 across. Recompute HTotal from the retained
		 * (1920p) blanking porches so active + blanking == total. */
		TimingInPtr = XVidC_GetTimingInfo(XVIDC_VM_1920x1080_60_P);
		StreamIn.Timing         = *TimingInPtr;
		StreamIn.Timing.HActive = CAM_WIDTH;
		StreamIn.Timing.VActive = CAM_HEIGHT;
		StreamIn.Timing.HTotal  = CAM_WIDTH
					+ StreamIn.Timing.HFrontPorch
					+ StreamIn.Timing.HSyncWidth
					+ StreamIn.Timing.HBackPorch;
		StreamIn.VmId           = XVIDC_VM_CUSTOM;
	} else {
		StreamIn.VmId           = resIdIn;
		StreamIn.Timing         = *TimingInPtr;
	}
	StreamIn.ColorFormatId  = COLOR_FORMAT_ID;
	StreamIn.ColorDepth     = pipe->Vproc.Config.ColorDepth;
	StreamIn.PixPerClk      = pipe->Vproc.Config.PixPerClock;
	StreamIn.FrameRate      = (StreamIn.VmId == XVIDC_VM_CUSTOM) ?
					VMODE_FRAMERATE : XVidC_GetFrameRate(resIdIn);
	StreamIn.IsInterlaced   = (StreamIn.VmId == XVIDC_VM_CUSTOM) ?
					FALSE : XVidC_IsInterlaced(resIdIn);
	XVprocSs_SetVidStreamIn(&(pipe->Vproc), &StreamIn);

	// Configure the Video Processing Subsystem OUTPUT stream parameters
	XVidC_VideoMode resIdOut = XVidC_GetVideoModeId(VPROC_WIDTH_OUT,VPROC_HEIGHT_OUT,VPROC_FRAMERATE_OUT,FALSE);
	XVidC_VideoTiming const *TimingOutPtr = XVidC_GetTimingInfo(resIdOut);
	XVidC_VideoStream StreamOut;
	StreamOut.VmId           = resIdOut;
	StreamOut.Timing         = *TimingOutPtr;
	StreamOut.ColorFormatId  = COLOR_FORMAT_ID;
	StreamOut.ColorDepth     = pipe->Vproc.Config.ColorDepth;
	StreamOut.PixPerClk      = pipe->Vproc.Config.PixPerClock;
	StreamOut.FrameRate      = XVidC_GetFrameRate(resIdOut);
	StreamOut.IsInterlaced   = XVidC_IsInterlaced(resIdOut);
	XVprocSs_SetVidStreamOut(&(pipe->Vproc), &StreamOut);

	// Start the Video Processor Subsystem
	Status = XVprocSs_SetSubsystemConfig(&(pipe->Vproc));
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: Failed to start the Video Processing SS\n\r");
		return XST_FAILURE;
	}
    XVprocSs_Start(&(pipe->Vproc));

	/*
	 * Demosaic initialization and config
	 */
    pipe_reset_deassert(pipe,GPIO_CAM_DEMOSAIC_RST_N_MASK);
	Status = XV_demosaic_Initialize(&(pipe->Demosaic), baseaddr->Demosaic);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: Failed to initialize the Demosaic\n\r");
		return XST_FAILURE;
	}
	XV_demosaic_Set_HwReg_width(&(pipe->Demosaic), CAM_WIDTH);
	XV_demosaic_Set_HwReg_height(&(pipe->Demosaic), CAM_HEIGHT);
	XV_demosaic_Set_HwReg_bayer_phase(&(pipe->Demosaic), rpi_cam_bayer_phase(&(pipe->Camera)));
	XV_demosaic_EnableAutoRestart(&(pipe->Demosaic));
	XV_demosaic_Start(&(pipe->Demosaic));
    
	/*
	 * Gamma LUT initialization and config
	 */
    pipe_reset_deassert(pipe,GPIO_CAM_GAMMA_RST_N_MASK);
	Status = XV_gamma_lut_Initialize(&(pipe->GammaLut), baseaddr->GammaLut);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: Failed to initialize the Gamma LUT\n\r");
		return XST_FAILURE;
	}
	XV_gamma_lut_Set_HwReg_width(&(pipe->GammaLut), CAM_WIDTH);
	XV_gamma_lut_Set_HwReg_height(&(pipe->GammaLut), CAM_HEIGHT);
	XV_gamma_lut_Set_HwReg_video_format(&(pipe->GammaLut), 0);
	/* Per-channel white balance via the three gamma LUTs (ch0=R @0x800,
	 * ch1=G @0x1000, ch2=B @0x1800). The RGGB Bayer array has twice as many
	 * green photosites, so the demosaiced image is heavily green-tinted.
	 * Attenuate green (and lift R/B slightly) to neutralise it. Tune these
	 * three gains to taste. */
	/* White balance measured against a white target: demosaic white response
	 * was R=230, G=253, B=253, so gains R=1.0, G=B=0.909 neutralise it
	 * (white -> R=G=B~230). Tuned live over JTAG then baked in here. */
	const double wb_gain[3] = { 1.000, 0.909, 0.909 };  /* R, G, B */
	const uint32_t lut_off[3] = { 0x800, 0x1000, 0x1800 };
	for(uint32_t i = 0; i < GAMMA_TABLE_SIZE; i++)
	{
		double base = pow((i / (double)GAMMA_TABLE_SIZE), GAMMA) * (double)GAMMA_TABLE_SIZE;
		for(int ch = 0; ch < 3; ch++) {
			double v = base * wb_gain[ch];
			if (v > (GAMMA_TABLE_SIZE - 1)) v = GAMMA_TABLE_SIZE - 1;
			Xil_Out16((pipe->GammaLut.Config.BaseAddress + lut_off[ch] + i*2), (uint16_t)v);
		}
	}
	XV_gamma_lut_Start(&(pipe->GammaLut));
	XV_gamma_lut_EnableAutoRestart(&(pipe->GammaLut));
#endif /* PIPE_HAS_ISP */

	pipe->IsConnected = TRUE;

	return(XST_SUCCESS);
}

int pipe_start_camera(VideoPipe *pipe)
{
	int Status;

	// Start the RPi camera
	Status = rpi_cam_config(&(pipe->Camera));
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: Failed to configure the camera\n\r");
		return XST_FAILURE;
	}
	// Start the Frame buffers
	Status = FrmbufStart(&(pipe->Frmbuf));
	return(Status);
}

