// mnist_pre.h — camera frame -> mbdpu01 MNIST input (28x28x1 INT8 [0,127]).
// SPDX-License-Identifier: Apache-2.0
//
// The camera delivers 640x480 RGB8; mbdpu01 wants a 28x28 grayscale INT8 image
// (values 0..127, matching the training input scale s_in = 1/127). This module
// center-crops a square, box-averages down to 28x28, converts RGB->luma, and
// quantizes. `invert` flips intensity (MNIST is white-digit-on-black, so a printed
// digit seen by the camera as black-on-white needs invert=1).
#ifndef MNIST_PRE_H
#define MNIST_PRE_H

#include <stdint.h>

#define MNIST_DIM      28
#define MNIST_N        (MNIST_DIM*MNIST_DIM)   // 784
#define MNIST_FRAME_W  640                     // camera VPROC_WIDTH_OUT
#define MNIST_FRAME_H  480                     // camera VPROC_HEIGHT_OUT
#define MNIST_CROP     112                     // 4*28 centered square crop (was 448)
#define MNIST_BLK      (MNIST_CROP/MNIST_DIM)  // 4x4 box average per output pixel

// Pure resampler (host-testable, no DDR/cache): RGB8 frame with `stride` bytes/row
// -> 28x28 INT8 in img28 (784 elems, 0..127; inverted if invert!=0).
void mnist_downscale_rgb8(const uint8_t *rgb, uint32_t stride, int invert, int8_t *img28);

// MicroBlaze wrapper: invalidate the DMA-written camera buffer, resample, then flush
// img_out so mbdpu01's AXI master reads fresh data from DDR.
void mnist_preprocess_rgb8(uintptr_t cam_frmbuf, uint32_t stride, int invert, uintptr_t img_out);

// On-screen feedback (drawn into the RGB8 background buffer; no extra video layer needed).
// mnist_draw_static: the fixed 0-9 digit labels for the bar chart — call ONCE at startup.
// mnist_draw_live: the ROI preview + a per-digit confidence bar (min-max normalized logits,
//   winner highlighted) — call each classification. Flushes the drawn region for the DMA.
void mnist_draw_static(uintptr_t fb, uint32_t stride);
void mnist_draw_live(uintptr_t fb, uint32_t stride, const int8_t *img28, const int32_t *logits);
// Model input side for the preview + position gauges: 28 (default; mbdpu01/02, preview x4, dx/dy
// in 28-px units) or 112 (mbdpu03: 112x112 input drawn 1:1, dx/dy in 112-px units). Call once.
void mnist_set_input_dim(int dim);

// Rotating-disc HUD state. Rate/latency fields carry a value x10 (one decimal) or -1 for "--"
// (no data yet); trig_on/strobe_armed render OFF/ON and OFF/ARM; expo is exposure lines;
// gain_x10 is analog gain x10 (e.g. 80 -> 8.0x).
typedef struct {
    int rpm;                         // whole revolutions/min (-1 = n/a)
    int caps_x10;                    // captures/second x10
    int pre_x10, dpu_x10, ovl_x10;   // mbpre / mbdpu01 / overlay latency, ms x10
    int trig_on;                     // FSIN external-trigger mode:  0=OFF 1=ON
    int strobe_armed;                // angle strobe:                0=OFF 1=ARM
    int expo;                        // OV9281 exposure (lines)
    int gain_x10;                    // OV9281 analog gain x10
    int target;                      // strobe target angle (counts)
    int cpr_x10;                     // strobe counts-per-rev x10 (e.g. 9792 = 979.2)
} mnist_stats_t;

// Rotating-disc HUD panel: draws the RPM / CAP / PRE / DPU / OVL / TRIG / STRB / EXPO / GAIN
// readout into the RGB8 background buffer, top-left at (x0,y0). The whole panel is repainted
// each call (fixed-width, cleared cells) so values never ghost; the region is cache-flushed for
// the DMA. Call it into BOTH background buffers so it survives the double-buffer page-flip.
void mnist_stats_panel(uintptr_t fb, uint32_t stride, int x0, int y0, const mnist_stats_t *s);

// strobe target gauge (compact 38 px box, scale-2 text): "TGT value/max" + a track 0..target_max with a
// white tick at the mid-point (the boot default) and a teal fill from it to the marker. target_max follows cpr/N, so redraw when either changes.
// auto_on relabels the gauge "AUTO" while the dx -> target loop (key 'A') is running.
void mnist_target_gauge(uintptr_t fb, uint32_t stride, int x0, int y0, int w,
                        uint32_t target, uint32_t target_max, int auto_on);

// mbdpu02 digit-position gauges framing the ROI preview: DX (horizontal, under it) and DY
// (vertical, left of it), +-14 model px = the preview's own extent, plus "DX +1.2"/"DY -0.4"
// value lines. Values in 1/100 model px (dx: left -, right +; dy: top +, bottom -). Repaints
// its area and cache-flushes it; call on the back buffer each capture (like mnist_draw_live).
void mnist_draw_pos(uintptr_t fb, uint32_t stride, int dx_x100, int dy_x100);

// Overlay drawing mode and profiling. Delta mode (off by default) repaints only what changed
// since the last draw into the same buffer; mnist_ovl_invalidate forces the next draw of each
// buffer to be a full repaint. mnist_set_tick supplies a free-running tick counter;
// mnist_ovl_prof copies + clears the per-stage tick sums and returns the number of draws.
#define MNIST_OVL_PROF_N 5   // 0 preview, 1 bars, 2 live flush, 3 pos draw, 4 pos flush
// Numeric DX/DY and TGT values on/off (default off; follows the HUD 'h' key). Gauges and their
// labels are always drawn. Forces a full DX/DY repaint; redraw the target gauge after calling.
void     mnist_set_show_values(int on);
/* 1: the background image carries the DX/DY, 0-9 and target-gauge labels (firmware skips them). */
void     mnist_set_bg_labels(int on);
void     mnist_ovl_set_delta(int on);
int      mnist_ovl_get_delta(void);
void     mnist_ovl_invalidate(void);
void     mnist_set_tick(uint32_t (*fn)(void));
uint32_t mnist_ovl_prof(uint32_t out[MNIST_OVL_PROF_N]);

#define ROI_CAP_DIM 112   // captured hi-res crop side (for training data)

// Capture the current crop region (side x side, from the live camera frame) resampled by
// nearest-neighbour to ROI_CAP_DIM x ROI_CAP_DIM 8-bit grayscale, for training-data export.
// Invalidates the crop rows first; writes ROI_CAP_DIM^2 bytes to out (caller flushes).
void mnist_capture_crop112(uintptr_t cam_frmbuf, uint32_t stride, int x0, int y0, int side,
                           uint8_t *out);

// ROI box: draw a `thick`-px rectangle outline (side x side, 0xRRGGBB) into a camera frame
// buffer at (x,y), clamped to the 640x480 frame; flushes the box rows for the display DMA.
// Drawn into the live camera buffer (fresh each frame, so no erase of the previous box needed).
void mnist_draw_roi_box(uintptr_t fb, uint32_t stride, int x, int y, int side,
                        uint32_t color, int thick);

#endif // MNIST_PRE_H
