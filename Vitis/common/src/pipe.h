/*
 * Copyright 2025 mariobergeron.com
 *
 */

#ifndef PIPE_H_
#define PIPE_H_

#include "xil_types.h"
#include "xgpio.h"
#include "xiic.h"
#include "xparameters.h"
#include "rpi_cam.h"
/* Monochrome variants (a monochrome build) omit the demosaic /
 * gamma / v_proc ISP cores. Detect their absence from the generated platform and
 * compile out all ISP code so the same app source serves colour and mono builds. */
#if defined(XPAR_MIPI_0_DEMOSAIC_0_BASEADDR) || defined(XPAR_MIPI_1_DEMOSAIC_0_BASEADDR)
#define PIPE_HAS_ISP 1
#include "xv_demosaic.h"
#include "xv_gamma_lut.h"
#include "xvprocss_vdma.h"
#endif
#include "frmbuf.h"

// Camera GPIO masks
#define GPIO_CAM_IO0_MASK	0x01
#define GPIO_CAM_IO1_MASK	0x02
// Reset GPIO masks
#define GPIO_CAM_DEMOSAIC_RST_N_MASK (0x1 << 2)  // CAM Demosaic IP reset
#define GPIO_CAM_VPROC_RST_N_MASK    (0x1 << 3)  // CAM Vproc IP reset
#define GPIO_CAM_GAMMA_RST_N_MASK    (0x1 << 4)  // CAM Gamma LUT IP reset
#define GPIO_CAM_FRMBUFRD_RST_N_MASK (0x1 << 5)  // CAM Frame Buffer Read IP reset
#define GPIO_CAM_FRMBUFWR_RST_N_MASK (0x1 << 6)  // CAM Frame Buffer Write IP reset

// VideoPipe struct
typedef struct {
	uint8_t IsConnected;
	XIic Iic;
	XGpio Gpio;
#ifdef PIPE_HAS_ISP
	XV_demosaic Demosaic;
	XV_gamma_lut GammaLut;
	XVprocSs Vproc;
#endif
	RpiCamera Camera;
	Frmbuf Frmbuf;
} VideoPipe;

// VideoPipe Base Addresses
typedef struct {
	UINTPTR Iic;
	UINTPTR Gpio;
	UINTPTR FrmbufWr;
	UINTPTR FrmbufRd;
	UINTPTR FrmbufBufrBaseAddr;
#ifdef PIPE_HAS_ISP
	UINTPTR Demosaic;
	UINTPTR GammaLut;
	UINTPTR Vproc;
#endif
} VideoPipeBaseAddr;

static inline uint32_t pipe_gpio_read(VideoPipe *pipe)   { return XGpio_DiscreteRead(&(pipe->Gpio), 1); }
static inline void     pipe_gpio_write(VideoPipe *pipe, uint32_t v) { XGpio_DiscreteWrite(&(pipe->Gpio), 1, v); }

/* ==== ACTIVE-LOW SEMANTICS: assert=0, deassert=1 ==== */
static inline void pipe_reset_assert(VideoPipe *pipe,uint32_t mask)
{
    uint32_t v = pipe_gpio_read(pipe);
    pipe_gpio_write(pipe,v & ~mask);
}

static inline void pipe_reset_deassert(VideoPipe *pipe,uint32_t mask)
{
    uint32_t v = pipe_gpio_read(pipe);
    pipe_gpio_write(pipe,v | mask);
}

int pipe_init(VideoPipe *pipe, VideoPipeBaseAddr *baseaddr);
int pipe_start_camera(VideoPipe *pipe);

#endif /* PIPE_H_ */
