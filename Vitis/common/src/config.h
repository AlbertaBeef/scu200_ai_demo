/*
 * Copyright 2025 mariobergeron.com
 *
 * This file should hold the major configuration parameters for the demo so that they can be set in
 * a single place.
 */

// Video mode and frame size defines
//#define DEMO_VMODE_720P60     1
#define DEMO_VMODE_1080P30     1

#if defined (DEMO_VMODE_720P60)
#define VMODE_WIDTH			1280
#define VMODE_HEIGHT		720
#define VMODE_CAM			MODE_720P_1280_720_60fps
#define GAMMA               1/1.0
#define PIXEL_SIZE			8	// In bits
#define GAMMA_TABLE_SIZE	256  // 2^PIXEL_SIZE
#define FRAME_HORI_LEN	  	VMODE_WIDTH*3  // Each pixel is 3 bytes
#define FRAME_VERT_LEN		VMODE_HEIGHT
#define SUBFRAME_START_OFFSET    (FRAME_HORI_LEN * 5 + 64)
#endif
#if defined (DEMO_VMODE_1080P30)
/* VMODE_* is the HDMI OUTPUT canvas (v_mix master layer + TPG background).
 * It must stay a standard 1080p mode - the mixer/HDMI TX are hardwired to
 * 1920x1080 elsewhere. Do NOT set this to the camera crop width. */
#define VMODE_WIDTH			1920
#define VMODE_HEIGHT		1080
#define VMODE_FRAMERATE		60
#define VMODE_CAM			MODE_1080P_1920_1080_30fps
/* CAM_* is the CAMERA capture path (demosaic + gamma + vproc INPUT). Use a
 * STANDARD 1920x1080 so the vproc gets a real XVidC timing (no fabricated
 * CUSTOM timing, which caused a ~3/4 vertical wrap seam). The sensor is
 * cropped to 1920 wide and its line rate is slowed (wider LINE_LENGTH) so
 * each 1920-px line still fits the 2-lane MIPI bandwidth without shear. */
/* CONFIRMATION TEST: crop the capture width to 640 (< the ~704-px capture
 * line-buffer wrap boundary) so no line exceeds the buffer -> no wrap seam.
 * Height left at 1080 (the wrap is horizontal only). vproc runs 1:1 (unity),
 * so the image is a clean 640x1080 strip on the canvas. If this is seam-free
 * it confirms the fixed capture-buffer wrap; the full-res fix is in the BD. */
/* Camera capture = 640x480 (4:3), matching the sensor's 640x480 crop and the
 * vproc output 1:1 (no scaling, correct aspect ratio). 640x480 is a standard
 * XVidC mode so both StreamIn and StreamOut use real timing (no custom). */
#define CAM_WIDTH			640
#define CAM_HEIGHT			480
#define VPROC_WIDTH_OUT		640
#define VPROC_HEIGHT_OUT	480
#define VPROC_FRAMERATE_OUT	60
#define COLOR_FORMAT_ID		XVIDC_CSF_RGB
#define GAMMA               1/1.0
#define PIXEL_SIZE			8	// In bits
#define GAMMA_TABLE_SIZE	256  // 2^PIXEL_SIZE
#define FRAME_HORI_LEN	  	VMODE_WIDTH*3  // Each pixel is 3 bytes
#define FRAME_VERT_LEN		VMODE_HEIGHT
#define SUBFRAME_START_OFFSET    (FRAME_HORI_LEN * 5 + 64)
#endif

