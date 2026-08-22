// motor.h — DC Motor 15 Click (DRV8874) speed/direction + quadrature-encoder read,
// for the SCU200 rotating-disc target. Compiled only in the `motor` variant, where
// the block design instantiates motor_pwm (axi_timer PWM) and motor_gpio (axi_gpio).
// SPDX-License-Identifier: Apache-2.0
//
// Wiring (see bd_scu200_ai_demo01.tcl / scu200_ai_demo01.xdc, all bank 66 3.3 V):
//   motor_pwm/pwm0 -> CLICK1_CS  = DRV8874 EN  (speed, PWM)
//   motor_gpio out[0] -> CLICK1_PWM = DRV8874 PH (direction)
//   motor_gpio out[1] -> CLICK1_RST = DRV8874 nSLEEP (1 = enable)
//   motor_gpio out[2] -> MCLICK1_3V3_ON, out[3] -> MCLICK2_3V3_ON (rail enables)
//   motor_gpio in[0]  <- CLICK1_INT = DRV8874 nFAULT (0 = fault)
//   motor_gpio in[1]  <- CLICK2_MISO = encoder A, in[2] <- CLICK2_MOSI = encoder B
#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>
#include "xparameters.h"

#if defined(XPAR_MOTOR_PWM_BASEADDR) || defined(XPAR_MOTOR_PWM_0_BASEADDR) \
 || defined(XPAR_MOTOR_GPIO_BASEADDR) || defined(XPAR_MOTOR_GPIO_0_BASEADDR)
#define MOTOR_PRESENT 1

void     motor_init(void);            // power click rails, PWM @0%, motor disabled (safe)
void     motor_enable(int on);        // nSLEEP: 1 = driver awake, 0 = coast/sleep
void     motor_set_dir(int reverse);  // PH bit: 0 = forward, 1 = reverse
void     motor_set_speed(int permille); // duty 0..1000 (0..100%)
int      motor_fault(void);           // 1 = nFAULT asserted (driver fault)
uint32_t motor_encoder_raw(void);     // live encoder: bit0 = A, bit1 = B
void     motor_start_default(void);   // auto-start 20% clockwise at init
void     motor_poll(void);            // non-blocking: one UART command/call, from the video loop

// Optional extra console handler: motor_poll routes keys it (and disc_strobe) don't consume
// to this callback. Returns 1 if it handled the key. Lets other modules (e.g. the mbpre crop
// control) add UART keys without a second UART reader. Register once at init.
typedef int (*motor_aux_key_cb_t)(int ch);
void     motor_set_aux_key_cb(motor_aux_key_cb_t cb);

#endif // MOTOR_PRESENT
#endif // MOTOR_H
