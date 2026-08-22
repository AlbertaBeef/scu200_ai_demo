/*
 * ar0234.c — onsemi AR0234 (Arducam B0429) driver, SCU200 port. See ar0234.h.
 * DETECT-ONLY: confirms raw-register access + cabling before we invest in the
 * full mode init. Uses the POLLED I2C path (SCU200 camera-bus interrupt is
 * unreliable — same rationale as imx477.c).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sleep.h"
#include "ar0234.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "i2c.h"

/* AR0234: 16-bit register address, 16-bit register data. */
int ar0234_write16(uint8_t iic_id, uint16_t addr, uint16_t data)
{
    uint8_t buf[4];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    buf[2] = data >> 8;
    buf[3] = data & 0xFF;
    return IicPolledSend(iic_id, AR0234_I2C_SLAVE_ADDR, buf, 4, XIIC_STOP);
}

int ar0234_read16(uint8_t iic_id, uint16_t addr, uint16_t *data)
{
    int Status;
    uint8_t abuf[2], dbuf[2] = {0, 0};
    abuf[0] = addr >> 8;
    abuf[1] = addr & 0xFF;
    /* Repeated-start: write the 16-bit register pointer, then read 2 data bytes. */
    Status = IicPolledSend(iic_id, AR0234_I2C_SLAVE_ADDR, abuf, 2, XIIC_REPEATED_START);
    if (Status != XST_SUCCESS) return Status;
    Status = IicPolledRecv(iic_id, AR0234_I2C_SLAVE_ADDR, dbuf, 2, XIIC_STOP);
    if (Status != XST_SUCCESS) return Status;
    *data = ((uint16_t)dbuf[0] << 8) | dbuf[1];
    return XST_SUCCESS;
}

/* Look for the AR0234 by reading its 16-bit chip ID. A clean 0x0A56 means the
 * module exposes the raw sensor AND the MIPI1 I2C/power is wired right. */
int ar0234_detect(uint8_t iic_id)
{
    uint16_t id = 0;
    int Status = ar0234_read16(iic_id, AR0234_REG_CHIP_ID, &id);
    if (Status != XST_SUCCESS) {
        xil_printf("  ar0234_detect: I2C read FAILED at addr 0x%02x "
                   "(no ACK — bridged module, wrong address, or cabling)\r\n",
                   AR0234_I2C_SLAVE_ADDR);
        return XST_FAILURE;
    }
    if (id != AR0234_CHIP_ID) {
        xil_printf("  ar0234_detect: I2C ACK at 0x%02x but chip ID=0x%04x "
                   "(expected 0x%04x) — likely an Arducam bridge, not raw AR0234\r\n",
                   AR0234_I2C_SLAVE_ADDR, id, AR0234_CHIP_ID);
        return XST_FAILURE;
    }
    xil_printf("  ar0234_detect: AR0234 FOUND (raw sensor, chip ID=0x0A56)\r\n");
    return XST_SUCCESS;
}

/* STUB: full 2-lane RAW10 mode init (NXP/Lattice tables, PLL into the ~420 Mbps
 * D-PHY window) is deferred until detect confirms raw access. This just soft-resets
 * and reports, so the pipe init doesn't attempt to stream garbage. */
int ar0234_config(uint8_t iic_id, XGpio *gpio, uint32_t gpio_mask)
{
    (void)iic_id;
    ar0234_reset(gpio, gpio_mask);
    xil_printf("  ar0234_config: STUB — register init not yet written "
               "(detect-only bring-up). No streaming configured yet.\r\n");
    return XST_SUCCESS;
}

/* Reset by toggling the camera power-enable GPIO (same scheme as IMX219/IMX477). */
int ar0234_reset(XGpio *gpio, uint32_t gpio_mask)
{
    XGpio_DiscreteClear(gpio, 1, gpio_mask);
    usleep(100000);
    XGpio_DiscreteSet(gpio, 1, gpio_mask);
    usleep(100000);
    return XST_SUCCESS;
}
