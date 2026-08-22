/*
 * Copyright 2025 mariobergeron.com
 *
 * This driver provides an interface to the image sensor. It currently supports:
 *
 *  - Digilent Pcam (OV5640)
 *  - Raspberry Pi camera v2 (IMX219)
 *  - Raspberry Pi HQ / Arducam B0242 (IMX477)
 *
 */

#include "rpi_cam.h"
#include "ov5640.h"
#include "imx219.h"
#include "imx477.h"
#include "ar0234.h"
#include "ov9281.h"

u8 NumRpiCameras = 0;
RpiCamera RpiCameraArray[4];

/* IMX477 is probed first: it is the camera validated on SCU200, and it lives
 * at I2C 0x1A, so probing it before the IMX219 (0x10) avoids a spurious no-ACK
 * on 0x10 when an IMX477 is attached. */
static const Sensor SupportedSensors[5] =
{
	/* OV9281 (Arducam B0165) probed first — the native/raw mono GS sensor, at I2C
	 * 0x60. Detect confirms chip ID 0x9281; config is a stub until the mode init lands. */
	{
		RPI_CAM_TYPE_OV9281,
		RPI_CAM_BAYER_PAT_OV9281,
		&ov9281_config,
		&ov9281_detect
	},
	/* AR0234 (Arducam B0429) — bridged module (answers only at 0x0C bridge, not raw);
	 * detect will no-ACK at 0x10 and fall through. Kept for a future raw AR0234. */
	{
		RPI_CAM_TYPE_AR0234,
		RPI_CAM_BAYER_PAT_AR0234,
		&ar0234_config,
		&ar0234_detect
	},
	{
		RPI_CAM_TYPE_IMX477,
		RPI_CAM_BAYER_PAT_IMX477,
		&imx477_config,
		&imx477_detect
	},
	{
		RPI_CAM_TYPE_OV5640,
		RPI_CAM_BAYER_PAT_OV5640,
		&ov5640_config,
		&ov5640_detect
	},
	{
		RPI_CAM_TYPE_IMX219,
		RPI_CAM_BAYER_PAT_IMX219,
		&imx219_config,
		&imx219_detect
	}
};

#define NUM_SUPPORTED_SENSORS (sizeof(SupportedSensors)/sizeof(SupportedSensors[0]))

// Initialize the RPi Camera driver
// Returns XST_SUCCESS if a supported camera was found on the I2C bus else returns XST_FAILURE
int rpi_cam_init(RpiCamera *camera,uint8_t iic_id,XGpio *gpio_inst,uint32_t gpio_mask)
{
    if (!camera || !gpio_inst) return XST_INVALID_PARAM;
	// Copy the IIC instance pointer that connects to the RPi Camera
	camera->iic_id = iic_id;
	// Copy the GPIO instance for the reset signal
	camera->gpio = gpio_inst;
	camera->gpio_mask = gpio_mask;

	// Determine the type of camera (if any) is connected
	// Read the detect register and check the returned value
	int Status;
	for(uint8_t i = 0; i < NUM_SUPPORTED_SENSORS; i++) {
		camera->sensor = SupportedSensors[i];
		Status = rpi_cam_detect(camera);
		if (Status == XST_SUCCESS) {
			return(XST_SUCCESS);
		}
	}
	// Otherwise no supported camera was found - FAILURE
	return XST_FAILURE;
}

// Configure the camera, using the appropriate driver
int rpi_cam_config(RpiCamera *camera)
{
	int Status;
	Status = camera->sensor.config(camera->iic_id,camera->gpio,camera->gpio_mask);
	return(Status);
}

// Detect the camera, using the appropriate driver
int rpi_cam_detect(RpiCamera *camera)
{
	int Status;
	Status = camera->sensor.detect(camera->iic_id);
	return(Status);
}

// Returns the bayer pattern as a value for Demosaic bayer phase
uint8_t rpi_cam_bayer_phase(RpiCamera *camera)
{
	return(camera->sensor.bayer_pattern);
}

