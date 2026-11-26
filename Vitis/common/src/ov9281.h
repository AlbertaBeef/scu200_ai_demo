/*
 * ov9281.h — OmniVision OV9281 (Arducam B0165, native/raw mono GS) driver, SCU200 port.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Raw sensor (unlike the bridged AR0234 B0429): 16-bit register address, 8-bit data
 * (OmniVision style, same as ov5640). Chip ID 0x300A/0x300B = 0x9281. Mono global
 * shutter, 1280x800, 2-lane RAW10 — fits the SCU200 mono pipeline directly (no demosaic).
 * DETECT-first bring-up; full 2-lane RAW10 mode init added once detect confirms the sensor.
 */
#ifndef OV9281_H_
#define OV9281_H_

#include <stdint.h>
#include "xil_types.h"
#include "xgpio.h"

#define OV9281_I2C_SLAVE_ADDR  0x60    /* 7-bit; OV9281 default (verify via boot I2C scan) */
#define OV9281_REG_CHIP_ID_HI  0x300A  /* = 0x92 */
#define OV9281_REG_CHIP_ID_LO  0x300B  /* = 0x81 */
#define OV9281_CHIP_ID         0x9281

int ov9281_detect(uint8_t iic_id);
int ov9281_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask);
int ov9281_reset(XGpio *gpio, uint32_t gpio_mask);
int ov9281_read(uint8_t iic_id, uint16_t addr, uint8_t *data);
int ov9281_write(uint8_t iic_id, uint16_t addr, uint8_t data);

/* External-trigger (FSIN) mode: enable=1 hands frame timing to the FSIN pad (one frame
 * per rising pulse from the disc_strobe core); enable=0 restores continuous streaming.
 * Call ONLY after ov9281_config(). Regs verified against Arducam ov9281_external_trigger.c. */
int ov9281_set_trigger_mode(uint8_t iic_id, int enable);

/* Set the manual exposure, in row/line units (registers 0x3500/0x3501/0x3502 = lines<<4).
 * Note: in FSIN snapshot/trigger mode the integration is gated by the wake timing, so this
 * has little/no effect there — use gain instead. Typical range ~16..(VTS-4). */
int ov9281_set_exposure(uint8_t iic_id, uint32_t lines);

/* Set the analog gain, in 1/16x units (real-gain format: 16 = 1.0x, 40 = 2.5x, 248 = 15.5x).
 * Registers 0x3508[4:0]/0x3509[7:0]. The main brightness lever in FSIN trigger mode. */
int ov9281_set_gain(uint8_t iic_id, uint32_t gain_x16);

/* Frame length (VTS, lines) - registers 0x380E/0x380F. The streaming mode uses
 * OV9281_VTS_DEFAULT (1373 lines x ~18.2 us = 25 ms, 40 fps). In FSIN trigger mode the
 * good frame starts one VTS after the wake frame, and the sensor ignores a new FSIN until
 * it has finished both, so a shorter VTS raises the strobe rate the camera can follow.
 * Must stay >= the 496 read-out lines plus blanking. */
#define OV9281_VTS_DEFAULT 1373u
int ov9281_set_vts(uint8_t iic_id, uint32_t vts);

#endif /* OV9281_H_ */
