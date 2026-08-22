/*
 * ar0234.h — onsemi AR0234 (Arducam B0429 GS module) driver, SCU200 port.
 * SPDX-License-Identifier: Apache-2.0
 *
 * DETECT-ONLY bring-up: reads the 16-bit chip-ID register to confirm whether the
 * module exposes the raw AR0234 on I2C (vs. an Arducam bridge) and that the MIPI1
 * cabling is right. The full 2-lane RAW10 register init is deferred until detect
 * confirms raw-register access (chip ID 0x3000 = 0x0A56). AR0234 uses 16-bit
 * register addresses AND 16-bit register data (unlike the IMX 16-bit/8-bit).
 */
#ifndef AR0234_H_
#define AR0234_H_

#include <stdint.h>
#include "xil_types.h"
#include "xgpio.h"

/* 7-bit I2C address. onsemi AR0234 SADDR-low default = 0x10 (Arducam typically 0x10).
 * If detect no-ACKs, 0x36 is the other common option — verify on the bus. */
#define AR0234_I2C_SLAVE_ADDR   0x10
#define AR0234_REG_CHIP_ID      0x3000   /* chip_version_reg, 16-bit data */
#define AR0234_CHIP_ID          0x0A56

int ar0234_detect(uint8_t iic_id);
int ar0234_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask);
int ar0234_reset(XGpio *gpio, uint32_t gpio_mask);
int ar0234_read16(uint8_t iic_id, uint16_t addr, uint16_t *data);
int ar0234_write16(uint8_t iic_id, uint16_t addr, uint16_t data);

#endif /* AR0234_H_ */
