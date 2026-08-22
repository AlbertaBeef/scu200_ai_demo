// motor.c — DC Motor 15 Click (DRV8874) control + encoder read. See motor.h.
// SPDX-License-Identifier: Apache-2.0
#include "motor.h"
#ifdef MOTOR_PRESENT

#include "xil_io.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xuartlite_l.h"   /* blocking UART read, independent of the BSP STDIN config */
#include "disc_strobe.h"   /* sibling core in the motor variant; keys routed below */

#if   defined(XPAR_AXI_UARTLITE_BASEADDR)
#define UART_BASE  XPAR_AXI_UARTLITE_BASEADDR
#elif defined(XPAR_XUARTLITE_0_BASEADDR)
#define UART_BASE  XPAR_XUARTLITE_0_BASEADDR
#endif

// ---- base addresses (from the generated platform; verify names in xparameters.h) ----
#if   defined(XPAR_MOTOR_PWM_BASEADDR)
#define PWM_BASE   XPAR_MOTOR_PWM_BASEADDR
#elif defined(XPAR_MOTOR_PWM_0_BASEADDR)
#define PWM_BASE   XPAR_MOTOR_PWM_0_BASEADDR
#endif
#if   defined(XPAR_MOTOR_GPIO_BASEADDR)
#define GPIO_BASE  XPAR_MOTOR_GPIO_BASEADDR
#elif defined(XPAR_MOTOR_GPIO_0_BASEADDR)
#define GPIO_BASE  XPAR_MOTOR_GPIO_0_BASEADDR
#endif

// ---- axi_timer registers (PWM mode uses both timers) ----
#define TCSR0  0x00
#define TLR0   0x04
#define TCSR1  0x10
#define TLR1   0x14
#define T_ENT  (1u<<7)   // enable timer
#define T_LOAD (1u<<5)   // load TLR -> counter
#define T_ARHT (1u<<4)   // auto-reload / hold
#define T_GENT (1u<<2)   // enable generate output
#define T_UDT  (1u<<1)   // count down
#define T_PWMA (1u<<9)   // PWM mode

// ---- axi_gpio registers ----
#define GPIO_DATA   0x00   // channel 1 (outputs)
#define GPIO2_DATA  0x08   // channel 2 (inputs)
#define O_DIR         (1u<<0)   // CLICK1_PWM  = PH  (direction)
#define O_NSLEEP      (1u<<1)   // CLICK1_RST  = nSLEEP (1 = enable)
#define O_MCLICK1_ON  (1u<<2)   // MCLICK1_3V3_ON (motor click rail)
#define O_MCLICK2_ON  (1u<<3)   // MCLICK2_3V3_ON (encoder rail)
#define I_NFAULT      (1u<<0)   // CLICK1_INT  = nFAULT (0 = fault)

#define PWM_CLK_HZ   100000000u
#define PWM_FREQ_HZ  20000u
#define PWM_PERIOD   (PWM_CLK_HZ / PWM_FREQ_HZ)   // 5000 cycles/period @ 20 kHz

static uint32_t g_out;   // shadow of the output data register

static void gpio_write(uint32_t bits, int on)
{
    if (on) g_out |= bits; else g_out &= ~bits;
    Xil_Out32(GPIO_BASE + GPIO_DATA, g_out);
}

static void pwm_setup(void)
{
    // Timer0 holds the PWM period, Timer1 the high time (duty). Load both, then
    // enable in PWM/generate/auto-reload/down-count mode.
    Xil_Out32(PWM_BASE + TLR0, PWM_PERIOD - 2);
    Xil_Out32(PWM_BASE + TLR1, 0);            // start at 0 % duty
    Xil_Out32(PWM_BASE + TCSR0, T_LOAD);
    Xil_Out32(PWM_BASE + TCSR1, T_LOAD);
    uint32_t cfg = T_PWMA | T_GENT | T_ARHT | T_UDT | T_ENT;
    Xil_Out32(PWM_BASE + TCSR0, cfg);
    Xil_Out32(PWM_BASE + TCSR1, cfg);
}

void motor_set_speed(int permille)
{
    if (permille < 0)    permille = 0;
    if (permille > 1000) permille = 1000;
    uint32_t high = (uint32_t)(((uint64_t)PWM_PERIOD * (uint32_t)permille) / 1000u);
    Xil_Out32(PWM_BASE + TLR1, high >= 2u ? high - 2u : 0u);   // updates next period (ARHT)
}

void     motor_set_dir(int reverse) { gpio_write(O_DIR, reverse ? 1 : 0); }
void     motor_enable(int on)       { gpio_write(O_NSLEEP, on ? 1 : 0); }
int      motor_fault(void)          { return (Xil_In32(GPIO_BASE + GPIO2_DATA) & I_NFAULT) ? 0 : 1; }
uint32_t motor_encoder_raw(void)    { return (Xil_In32(GPIO_BASE + GPIO2_DATA) >> 1) & 0x3u; }

void motor_init(void)
{
    g_out = 0;
    Xil_Out32(GPIO_BASE + GPIO_DATA, 0);            // all low: disabled + rails off (safe)
    gpio_write(O_MCLICK1_ON | O_MCLICK2_ON, 1);     // power the motor-click + encoder rails
    usleep(50000);                                  // let the 3.3 V load switches settle
    pwm_setup();
    motor_set_speed(0);
    motor_set_dir(0);
    motor_enable(0);                                // stay asleep until commanded
    xil_printf("motor: init (PWM %u Hz, disabled, click rails ON)\r\n", PWM_FREQ_HZ);
}

/* --- Concurrent control: state persists across non-blocking poll calls --- */
static int g_speed = 0, g_dir = 0, g_en = 0;

/* Optional extra console handler (see motor.h) — e.g. the mbpre crop pan/zoom keys. */
static motor_aux_key_cb_t g_aux_key_cb = 0;
void motor_set_aux_key_cb(motor_aux_key_cb_t cb) { g_aux_key_cb = cb; }

/* Auto-start at boot: 20% speed, CLOCKWISE (dir=REV; FWD=CCW / REV=CW). */
void motor_start_default(void)
{
    g_dir = 1;          /* REV = clockwise */
    g_speed = 200;      /* 20% */
    g_en = 1;
    motor_set_dir(g_dir);
    motor_set_speed(g_speed);
    motor_enable(1);
    xil_printf("motor: auto-start 20 pct CW.  keys: e d r 0-9 +/- f c p (via UART)\r\n");
}

/* Non-blocking: process one motor command key if the UART has one. Call once per
 * main-loop iteration so the motor runs concurrently with the live video pipeline. */
void motor_poll(void)
{
    if (XUartLite_IsReceiveEmpty(UART_BASE)) return;
    int ch = XUartLite_RecvByte(UART_BASE);
    switch (ch) {
    case 'e': g_en = 1; motor_enable(1); xil_printf("motor: enabled\r\n"); break;
    case 'd': g_en = 0; motor_enable(0); xil_printf("motor: disabled\r\n"); break;
    case 'r': g_dir = !g_dir; motor_set_dir(g_dir);
              xil_printf("motor: dir=%s\r\n", g_dir ? "CW" : "CCW"); break;
    case '+': g_speed += 50; if (g_speed > 1000) g_speed = 1000; motor_set_speed(g_speed);
              xil_printf("motor: speed=%d pct\r\n", g_speed/10); break;
    case '-': g_speed -= 50; if (g_speed < 0) g_speed = 0; motor_set_speed(g_speed);
              xil_printf("motor: speed=%d pct\r\n", g_speed/10); break;
    case '0': g_speed = 0; motor_set_speed(0); xil_printf("motor: stop\r\n"); break;
    case 'f': xil_printf("motor: nFAULT %s\r\n", motor_fault() ? "FAULT" : "ok"); break;
    case 'p': xil_printf("motor: en=%d dir=%s speed=%d pct fault=%d encAB=%lu\r\n",
                         g_en, g_dir ? "CW" : "CCW", g_speed/10, motor_fault(),
                         (unsigned long)motor_encoder_raw()); break;
    case 'c': {   /* brief (~ms) encoder toggle-check; occasional, tolerable in the loop */
        uint32_t seenA = 0, seenB = 0, prev = 0xFFu; int trans = 0;
        for (int i = 0; i < 200000; i++) {
            uint32_t v = motor_encoder_raw();
            seenA |= (1u << (v & 1u));
            seenB |= (1u << ((v >> 1) & 1u));
            if (v != prev) { trans++; prev = v; }
        }
        xil_printf("motor: enc A_tog=%d B_tog=%d trans=%d\r\n", (seenA==3),(seenB==3),trans);
        break;
    }
    default:
#ifdef DISC_STROBE_PRESENT
        if (disc_strobe_poll_key(ch)) break;   /* S F N T G C V Y -> strobe control */
#endif
        if (g_aux_key_cb && g_aux_key_cb(ch)) break;   /* e.g. mbpre crop pan/zoom keys */
        if (ch >= '1' && ch <= '9') { g_speed = (ch - '0') * 100; motor_set_speed(g_speed);
            xil_printf("motor: speed=%d pct\r\n", g_speed/10); }
        break;
    }
}

#endif // MOTOR_PRESENT
