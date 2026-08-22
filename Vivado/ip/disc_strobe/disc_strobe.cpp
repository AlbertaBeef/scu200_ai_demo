// disc_strobe.cpp — motor-encoder quadrature decode + angle-triggered camera strobe.
// SPDX-License-Identifier: Apache-2.0
//
// Free-running fabric core (ap_ctrl_none, II=1): the function body is the per-clock
// logic and the static locals are registers. Decodes the Pololu 25D encoder with FULL
// 4x quadrature (count on every A/B edge, direction from the transition), and tracks the
// disc angle in a FIXED-POINT (Q16: counts x65536) phase accumulator that CARRIES the
// fractional remainder across each revolution wrap. That kills the integer-rounding drift
// of the old 1x/integer scheme: with a 20.4:1 gearbox the true count/rev is 48*20.4 = 979.2,
// not an integer, so wrapping at an integer cpr walked the fire grid ~0.2 count/rev. The
// accumulator wraps at the Q16 cpr and keeps the leftover, so the fire points stay phase-locked.
//
// Register UNITS (AXI-Lite, offsets unchanged from the 1x version):
//   cpr / target_angle / fire_step  are now Q16 fixed-point (value = counts x 65536).
//   num_fires / pulse_width / enable are plain integers.
//   position (RO) is the integer 4x count (acc>>16); rev_count (RO) is revolutions.
#include <ap_int.h>
#include <stdint.h>

void disc_strobe(
    ap_uint<1>  enc_a,          // encoder A (raw; synced internally)
    ap_uint<1>  enc_b,          // encoder B
    ap_uint<1> &xvs,            // strobe/trigger output -> OV9281 FSIN
    uint32_t    cpr,            // Q16: counts per disc revolution x65536 (4x; ~979.2*65536)
    uint32_t    target_angle,   // Q16: angle of the FIRST fire point this rev
    uint32_t    pulse_width,    // XVS high time, in 100 MHz clocks (>=100 = 1us)
    uint32_t    enable,         // 1 = strobe active
    uint32_t    fire_step,      // Q16: counts between consecutive fire points (= cpr/N; 0 -> cpr)
    uint32_t    num_fires,      // fire EXACTLY this many pulses per rev (= N; 0 -> 1)
    uint32_t   &position,       // RO: current integer 4x count (acc>>16)
    uint32_t   &rev_count)      // RO: revolutions since reset
{
#pragma HLS INTERFACE ap_ctrl_none port=return
#pragma HLS INTERFACE ap_none port=enc_a
#pragma HLS INTERFACE ap_none port=enc_b
#pragma HLS INTERFACE ap_none port=xvs
#pragma HLS INTERFACE s_axilite port=cpr          bundle=ctrl
#pragma HLS INTERFACE s_axilite port=target_angle bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pulse_width  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=enable       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=fire_step    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=num_fires    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=position     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=rev_count    bundle=ctrl
#pragma HLS PIPELINE II=1

    // 2-FF metastability sync on A/B. qcurr = {A,B} (bit1=A, bit0=B); qprev is last sample.
    static ap_uint<1> a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    a2 = a1; a1 = enc_a;
    b2 = b1; b1 = enc_b;
    ap_uint<2> qcurr = (a2, b2);
    static ap_uint<2> qprev = 0;

    static int32_t  acc       = 0;   // Q16 phase accumulator (0..cpr), carries the remainder
    static uint32_t revs      = 0;
    static uint32_t pcnt      = 0;   // XVS pulse-remaining counter
    static uint32_t next_fire = 0;   // Q16: position of the next strobe pulse this rev
    static uint32_t fire_idx  = 0;   // pulses already fired this rev (capped at num_fires)

    const uint32_t step  = (fire_step == 0) ? cpr : fire_step;   // Q16
    const uint32_t nfire = (num_fires == 0) ? 1u : num_fires;

    // 4x quadrature decode: +1/-1 per edge from the (qprev -> qcurr) transition.
    // Forward sequence 00->10->11->01->00 (rising A with B low = +1, matching the old 1x sign).
    const unsigned tr = ((unsigned)qprev.to_uint() << 2) | (unsigned)qcurr.to_uint();
    int d = 0;
    switch (tr) {
        case 0x2: case 0xB: case 0xD: case 0x4: d = +1; break;   // forward edges
        case 0x1: case 0x7: case 0xE: case 0x8: d = -1; break;   // reverse edges
        default:                                d =  0; break;   // no change / illegal double-step
    }
    qprev = qcurr;

    if (d != 0) {
        acc += d * 65536;                                        // one 4x count = 65536 in Q16
        bool wrapped = false;
        if (acc >= (int32_t)cpr) { acc -= (int32_t)cpr; revs++; wrapped = true; }  // carry remainder
        else if (acc < 0)        { acc += (int32_t)cpr;        wrapped = true; }

        if (wrapped) { next_fire = target_angle; fire_idx = 0; } // re-anchor grid + fire count

        // Fire when this edge's phase just crossed next_fire (forward), within one count. The
        // carried remainder means the crossing lands at the correct sub-count angle -> no drift.
        if (enable && pulse_width != 0 && fire_idx < nfire &&
            (uint32_t)acc >= next_fire && ((uint32_t)acc - next_fire) < 65536u) {
            pcnt = pulse_width;                                  // arm the strobe pulse
            next_fire += step;                                  // advance to the next fire point
            fire_idx++;                                         // ... but never more than N per rev
        }
    }

    xvs = (pcnt > 0) ? 1 : 0;
    if (pcnt > 0) pcnt--;

    position  = (uint32_t)acc >> 16;  // integer 4x count for the software fine-count RPM
    rev_count = revs;
}

#ifndef __SYNTHESIS__
#include <cstdio>
#include <cstdlib>

// Drive a full run at (cpr_frac, N) and check the steady-state (rev>=2) invariants:
//  (a) EXACTLY N pulses per rev (over MANY revs -> validates the fractional carry never
//      drops/adds a fire at a wrap), and
//  (b) the minimum gap between consecutive pulses (in 4x counts) is >= floor(cpr/N)
//      -> no extra tightly-spaced pulse near the wrap.
static int run_frac(double cpr_frac, uint32_t N)
{
    // Forward quadrature 00->10->11->01 (one edge per phase = one 4x count).
    const int sa[4] = {0, 1, 1, 0};
    const int sb[4] = {0, 0, 1, 1};

    const uint32_t cpr_q16 = (uint32_t)(cpr_frac * 65536.0 + 0.5);
    uint32_t step_q16 = cpr_q16 / N; if (step_q16 == 0) step_q16 = 1;
    const uint32_t step_cnt = step_q16 >> 16;

    ap_uint<1> xvs, xvs_prev = 0; uint32_t pos = 0, revs = 0, prevpos = 0;
    long abs_cnt = 0, prev_fire_abs = -1, min_gap = 1L << 30;
    uint32_t per_rev = 0, last_rev = 0; int bad_rev = 0, steady_revs = 0;

    const int REVS = 50;                                   // many revs -> exercise the carry
    long total = (long)(cpr_frac * (double)REVS) * 4 * 10; // 4 phases/count, one phase per 10 clks
    for (long clk = 0; clk < total; clk++) {
        int ph = (int)((clk / 10) % 4);
        disc_strobe(sa[ph], sb[ph], xvs, cpr_q16, /*target*/0, /*pw*/3, /*en*/1,
                    step_q16, N, pos, revs);
        if (pos != prevpos) { abs_cnt++; prevpos = pos; }
        if (revs != last_rev) {                            // revolution wrap
            if (revs >= 3) { if (per_rev != N) bad_rev = 1; steady_revs++; }
            per_rev = 0; last_rev = revs;
        }
        if (xvs && !xvs_prev) {                            // rising edge = one capture
            if (revs >= 2) {
                per_rev++;
                if (prev_fire_abs >= 0) { long g = abs_cnt - prev_fire_abs; if (g < min_gap) min_gap = g; }
            }
            prev_fire_abs = abs_cnt;
        }
        xvs_prev = xvs;
    }
    bool ok = (!bad_rev) && (steady_revs >= 3) && (min_gap >= (long)step_cnt);
    printf("  cpr=%.1f (q16=%u) N=%2u step=%u.%02u cnt: per-rev %s over %d revs, "
           "min_gap=%ld (need >= %u) -> %s\n",
           cpr_frac, cpr_q16, N, step_q16 >> 16, ((step_q16 & 0xFFFF) * 100) >> 16,
           bad_rev ? "BAD" : "= N", steady_revs, min_gap, step_cnt, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// One config per process (the core's static state must start fresh each time).
int main(int argc, char **argv)
{
    if (argc >= 3) return run_frac(atof(argv[1]), (uint32_t)atoi(argv[2]));
    return run_frac(979.2, 8);   // default: 4x, 20.4:1 gearbox, 8 captures/rev
}
#endif
