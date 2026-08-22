/*
 * Copyright 2025 mariobergeron.com
 *
 * IMX477 (Raspberry Pi HQ / Arducam B0242) sensor driver for the SCU200 port.
 *
 * Register mode tables are derived from the Raspberry Pi Linux kernel driver
 * drivers/media/i2c/imx477.c (mode_common_regs + mode_2028x1080_regs), reduced
 * to a fixed 2-lane 1920x1080 configuration (the 2028x1080 2x2-binned "1080p
 * cropped" mode, horizontally windowed from 2028 to 1920).
 */
#ifndef IMX477_H_
#define IMX477_H_

#include <stdint.h>
#include "xgpio.h"

/* Confirmed by on-board I2C ACK during bring-up: the Arducam IMX477 sits at
 * 0x1A, not 0x10 (which is the IMX219). */
#define IMX477_I2C_SLAVE_ADDR	0x1A

/* Chip ID: register 0x0016 (hi) / 0x0017 (lo) reads 0x0477 */
#define IMX477_REG_CHIP_ID_HI	0x0016
#define IMX477_REG_CHIP_ID_LO	0x0017
#define IMX477_CHIP_ID		0x0477

/* Register address (16-bit) and value (8-bit) */
typedef struct {
	uint16_t addr;
	uint8_t data;
} imx477_config_word_t;

int imx477_detect(uint8_t iic_id);
int imx477_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask);
int imx477_reset(XGpio *gpio, uint32_t gpio_mask);
int imx477_write(uint8_t iic_id, uint16_t addr, uint8_t data);
int imx477_read(uint8_t iic_id, uint16_t addr, uint8_t *data);

#endif /* IMX477_H_ */
