// disc_strobe.h — control/read for the disc_strobe fabric core (quadrature decode +
// angle-triggered XVS/FSIN strobe to the OV9281). Compiled only in the `motor` variant,
// where the block design instantiates xilinx.com:hls:disc_strobe:1.0 on s_axi_ctrl.
// SPDX-License-Identifier: Apache-2.0
//
// The core reads the same encoder A/B the motor GPIO sees (forked in fabric), keeps its
// own position counter that wraps at `cpr`, and pulses xvs -> MOTOR_XVS (D25) -> the
// OV9281 FSIN pad each time position == target_angle. See Vivado/ip/disc_strobe.
#ifndef DISC_STROBE_H
#define DISC_STROBE_H

#include <stdint.h>
#include "xparameters.h"

#if   defined(XPAR_DISC_STROBE_S_AXI_CTRL_BASEADDR)
#define DISC_STROBE_BASE  XPAR_DISC_STROBE_S_AXI_CTRL_BASEADDR
#define DISC_STROBE_PRESENT 1
#elif defined(XPAR_DISC_STROBE_BASEADDR)
#define DISC_STROBE_BASE  XPAR_DISC_STROBE_BASEADDR
#define DISC_STROBE_PRESENT 1
#elif defined(XPAR_DISC_STROBE_0_S_AXI_CTRL_BASEADDR)
#define DISC_STROBE_BASE  XPAR_DISC_STROBE_0_S_AXI_CTRL_BASEADDR
#define DISC_STROBE_PRESENT 1
#endif

#ifdef DISC_STROBE_PRESENT

// Encoder resolution reaching the core: Pololu 48-CPR encoder on a 20.4:1 gearbox, decoded
// with FULL 4x quadrature. Nominal = 48*20.4 = 979.2 counts/disc-rev, but EMPIRICALLY CALIBRATED
// on this unit to 979.6 (the real gear ratio runs a touch high of 20.4:1). Non-integer, so the
// core tracks the angle in Q16 fixed-point (counts x65536) and carries the fractional remainder
// across each wrap -> no per-rev rounding drift. cpr/target/fire_step registers are Q16.
// C/V still trim cpr live (fine, 0.1-count steps) to re-null if the mechanics change.
#define DISC_STROBE_Q16             65536u        // Q16 scale (1.0 count)
#define DISC_STROBE_CPR_DEFAULT     979u          // integer counts/rev (display + fine-count RPM)
#define DISC_STROBE_CPR_Q16_DEFAULT 64199066u     // 979.6 * 65536 (calibrated fractional value)
#define DISC_STROBE_PULSE_DEFAULT   1000u         // XVS high time in 100 MHz clocks (10 us)

// Optional hook: the 'F' console key toggles the camera's FSIN trigger mode via this
// callback (main registers one wrapping ov9281_set_trigger_mode). Kept separate from the
// strobe-arm 'S' key so the encoder/strobe can be exercised with a live free-running image
// before the FSIN flylead is connected.
typedef void (*disc_strobe_cam_cb_t)(int enable);
void     disc_strobe_set_cam_cb(disc_strobe_cam_cb_t cb);

void     disc_strobe_init(void);              // program cpr/target/pulse, strobe OFF
void     disc_strobe_enable(int on);          // 1 = fire XVS at target_angle
void     disc_strobe_set_cpr(uint32_t cpr);
void     disc_strobe_set_target(uint32_t angle);
void     disc_strobe_set_pulse(uint32_t clks);
void     disc_strobe_set_captures(uint32_t n);  // N evenly-spaced captures/rev (fire_step=cpr/N)
uint32_t disc_strobe_position(void);          // live position count (0..cpr-1)
uint32_t disc_strobe_revs(void);              // revolutions since reset
uint32_t disc_strobe_cpr(void);               // current counts-per-revolution, integer (for fine-count rate)
uint32_t disc_strobe_cpr_x10(void);           // cpr x10 (one-decimal display, e.g. 9792 = 979.2)
uint32_t disc_strobe_target(void);            // current target angle (integer counts)
uint32_t disc_strobe_captures(void);          // current captures/rev (N)
int      disc_strobe_enabled(void);           // 1 = strobe armed, 0 = off
void     disc_strobe_status(void);            // print state over UART
int      disc_strobe_poll_key(int ch);        // handle one console key; 1 if consumed

#endif // DISC_STROBE_PRESENT
#endif // DISC_STROBE_H
