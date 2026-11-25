// mbdpu03_tb.cpp — C-sim testbench for mbdpu03: bit-exact against a plain C golden model.
// SPDX-License-Identifier: Apache-2.0
//
//  1. random weights, input from img (CTRL_SRC_IMG), RELOAD on the first call only (checks the
//     on-chip stores persist between calls);
//  2. camera path: synthetic RGB8 mono frame, crops at blk 1/2/4 (incl. a non-8-aligned x0) and
//     invert -> the img write-back must equal the golden crop and the outputs the golden net on it;
//  3. when $MBDPU03_TESTDIR holds the trainer's blobs + vectors (train/mbdpu03/), every test
//     image must reproduce mnist_tests_expect.bin exactly (the Python INT8 model).
#include "mbdpu03.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static i8  g_w[WBLOB_N];
static i32 g_p[PBLOB_N];
static i8  g_img[IMG_N];
static i32 g_out[OUT_N];
static std::vector<w64> g_cam(CAM_W * CAM_H * 3 / 8);
static uint8_t g_frame[CAM_W * CAM_H * 3];

// ---------------------------------------------------------------- golden model
static i32 rq(int64_t acc, i32 bias, i32 M0, int sh)
{
    int64_t x = (acc + bias) * (int64_t)M0;
    int64_t r = (x + (sh > 0 ? (int64_t)1 << (sh - 1) : 0)) >> sh;
    if (r < 0) r = 0;
    if (r > 127) r = 127;
    return (i32)r;
}

// conv3x3 pad1 + bias + ReLU + requant, then 2x2 maxpool. in/out are [h][w][c].
static void ref_layer(const std::vector<i32>& in, int H, int IC, int OC, const i8* w,
                      const i32* b, i32 M0, int sh, std::vector<i32>& out)
{
    std::vector<i32> a(H * H * OC);
    for (int oc = 0; oc < OC; oc++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < H; x++) {
                int64_t acc = 0;
                for (int ic = 0; ic < IC; ic++)
                    for (int kh = 0; kh < 3; kh++)
                        for (int kw = 0; kw < 3; kw++) {
                            int yy = y + kh - 1, xx = x + kw - 1;
                            if (yy < 0 || yy >= H || xx < 0 || xx >= H) continue;
                            acc += (int64_t)in[(yy * H + xx) * IC + ic] *
                                   w[((oc * IC + ic) * 3 + kh) * 3 + kw];
                        }
                a[(y * H + x) * OC + oc] = rq(acc, b[oc], M0, sh);
            }
    const int P = H / 2;
    out.assign(P * P * OC, 0);
    for (int y = 0; y < P; y++)
        for (int x = 0; x < P; x++)
            for (int oc = 0; oc < OC; oc++) {
                i32 m = -1000;
                for (int d = 0; d < 4; d++) {
                    i32 v = a[((2 * y + d / 2) * H + 2 * x + d % 2) * OC + oc];
                    if (v > m) m = v;
                }
                out[(y * P + x) * OC + oc] = m;
            }
}

static void ref_net(const i8* w, const i32* p, const i8* img, i32* out)
{
    std::vector<i32> a(img, img + IMG_N), b;
    const int IC[4] = {1, C1, C2, C3}, OC[4] = {C1, C2, C3, C4};
    const int WO[4] = {OFF_C1W, OFF_C2W, OFF_C3W, OFF_C4W}, BO[4] = {OFF_C1B, OFF_C2B, OFF_C3B, OFF_C4B};
    int H = IN_DIM;
    for (int l = 0; l < 4; l++) {
        ref_layer(a, H, IC[l], OC[l], w + WO[l], p + BO[l], p[OFF_M + 2 * l], p[OFF_M + 2 * l + 1], b);
        a.swap(b); H /= 2;
    }
    int best = 0; int64_t bestv = INT64_MIN;
    for (int o = 0; o < FC_OUT; o++) {
        int64_t acc = (o < FC_CLS) ? p[OFF_FCB + o] : p[OFF_POSB + o - FC_CLS];
        for (int i = 0; i < FC_IN; i++) acc += (int64_t)a[i] * w[OFF_FCW + o * FC_IN + i];
        if (o < FC_CLS) { out[o] = (i32)acc; if (acc > bestv) { bestv = acc; best = o; } }
        else out[o + 1] = (i32)acc;
    }
    out[FC_CLS] = best;
}

static void ref_crop(int x0, int y0, int bs, bool inv, i8* img)
{
    const int blk = 1 << bs;
    for (int oy = 0; oy < IN_DIM; oy++)
        for (int ox = 0; ox < IN_DIM; ox++) {
            uint32_t s = 0;
            for (int dy = 0; dy < blk; dy++)
                for (int dx = 0; dx < blk; dx++)
                    s += g_frame[((y0 + oy * blk + dy) * CAM_W + x0 + ox * blk + dx) * 3 + 1];
            int q = (int)((s >> (2 * bs)) >> 1);
            if (inv) q = 127 - q;
            img[oy * IN_DIM + ox] = (i8)q;
        }
}

// ---------------------------------------------------------------- helpers
static int cmp_out(const char* tag, const i32* hw, const i32* ref)
{
    int e = 0;
    for (int i = 0; i < OUT_N; i++)
        if (hw[i] != ref[i]) {
            if (e < 6) printf("  %s: out[%d] hw=%d ref=%d\n", tag, i, hw[i], ref[i]);
            e++;
        }
    return e;
}

static void rand_model(unsigned seed)
{
    srand(seed);
    for (int i = 0; i < WBLOB_N; i++) g_w[i] = (i8)(rand() % 41 - 20);
    for (int i = 0; i < PBLOB_N; i++) g_p[i] = rand() % 2001 - 1000;
    // requant scales that keep activations in range (roughly 64 / typical |acc|)
    const int sh[4] = {22, 23, 23, 24};
    for (int l = 0; l < 4; l++) { g_p[OFF_M + 2 * l] = 30000 + rand() % 20000; g_p[OFF_M + 2 * l + 1] = sh[l]; }
}

static void run_hw(uint32_t ctrl, int x0 = 0, int y0 = 0, int bs = 0)
{
    mbdpu03(g_w, g_p, g_img, g_out, g_cam.data(), CAM_W * 3, x0, y0, bs, ctrl);
}

static bool read_file(const std::string& path, void* dst, size_t n)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    size_t r = fread(dst, 1, n, f);
    fclose(f);
    return r == n;
}

int main()
{
    int errs = 0;
    i32 ref[OUT_N];

    // ---- 1. random model, img input, weights persist after the first RELOAD ----
    rand_model(1);
    for (int t = 0; t < 3; t++) {
        srand(100 + t);
        for (int i = 0; i < IMG_N; i++) g_img[i] = (i8)(rand() % 128);
        // a dark blob so conv layers see structure, not just noise
        for (int y = 30 + 10 * t; y < 70 + 10 * t; y++)
            for (int x = 40; x < 60; x++) g_img[y * IN_DIM + x] = (i8)(rand() % 20);
        run_hw(CTRL_SRC_IMG | (t == 0 ? CTRL_RELOAD : 0));
        ref_net(g_w, g_p, g_img, ref);
        int e = cmp_out("random", g_out, ref);
        printf("random model img %d: %s (argmax %d, logit0 %d, dx %d)\n", t, e ? "FAIL" : "ok",
               g_out[FC_CLS], g_out[0], g_out[11]);
        errs += e;
    }

    // ---- 1b. extremes for the packed (2 MAC/DSP) arithmetic: full-range +-127 weights, inputs
    //      0/127, small requant scales so every layer still sees large activations ----
    srand(7);
    for (int i = 0; i < WBLOB_N; i++) { int r = rand() % 3; g_w[i] = (i8)(r == 0 ? -127 : r == 1 ? 127 : rand() % 255 - 127); }
    for (int i = 0; i < PBLOB_N; i++) g_p[i] = rand() % 2001 - 1000;
    for (int l = 0; l < 4; l++) { g_p[OFF_M + 2 * l] = 40000 + rand() % 20000; g_p[OFF_M + 2 * l + 1] = 26 + 2 * l; }
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < IMG_N; i++) g_img[i] = (i8)(t == 0 ? 127 : ((i / 7 + i / 113) & 1) * 127);
        run_hw(CTRL_SRC_IMG | (t == 0 ? CTRL_RELOAD : 0));
        ref_net(g_w, g_p, g_img, ref);
        int e = cmp_out("extreme", g_out, ref);
        printf("extreme weights img %d: %s (argmax %d, logit0 %d)\n", t, e ? "FAIL" : "ok", g_out[FC_CLS], g_out[0]);
        errs += e;
    }
    rand_model(1);
    run_hw(CTRL_SRC_IMG | CTRL_RELOAD);   // restore the random model for the camera tests

    // ---- 2. camera path: synthetic mono frame, blk 1/2/4, invert, unaligned x0 ----
    for (int y = 0; y < CAM_H; y++)
        for (int x = 0; x < CAM_W; x++) {
            int v = (x * 7 + y * 3) & 255;
            if ((x / 40 + y / 40) & 1) v = 255 - v / 3;
            for (int c = 0; c < 3; c++) g_frame[(y * CAM_W + x) * 3 + c] = (uint8_t)v;
        }
    for (size_t i = 0; i < g_cam.size(); i++) {
        w64 w = 0;
        for (int k = 0; k < 8; k++) w.range(8 * k + 7, 8 * k) = g_frame[i * 8 + k];
        g_cam[i] = w;
    }
    struct { int x0, y0, bs; bool inv; } cases[] = {
        {264, 184, 0, false}, {101, 57, 0, true}, {209, 133, 1, false}, {96, 16, 2, false},
    };
    for (auto& c : cases) {
        i8 gold[IMG_N];
        ref_crop(c.x0, c.y0, c.bs, c.inv, gold);
        memset(g_img, 0x55, sizeof g_img);
        run_hw(c.inv ? CTRL_INVERT : 0, c.x0, c.y0, c.bs);
        int ce = 0;
        for (int i = 0; i < IMG_N; i++) ce += (g_img[i] != gold[i]);
        ref_net(g_w, g_p, gold, ref);
        int e = cmp_out("camera", g_out, ref);
        printf("camera crop x0=%d y0=%d blk=%d inv=%d: img %s (%d diff), net %s\n", c.x0, c.y0,
               1 << c.bs, (int)c.inv, ce ? "FAIL" : "ok", ce, e ? "FAIL" : "ok");
        errs += e + (ce != 0);
    }

    // ---- 3. trainer vectors (bit-exact vs the Python INT8 model) ----
    const char* dir = getenv("MBDPU03_TESTDIR");
    if (dir) {
        std::string d(dir);
        std::vector<uint8_t> tests;
        FILE* f = fopen((d + "/mnist_tests.bin").c_str(), "rb");
        if (f && read_file(d + "/mnist_wblob.bin", g_w, WBLOB_N) &&
            read_file(d + "/mnist_pblob.bin", g_p, PBLOB_N * 4)) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            tests.resize(n); fread(tests.data(), 1, n, f); fclose(f);
            const int N = (int)(n / (IMG_N + 1));
            std::vector<i32> expect(N * OUT_N);
            bool have_exp = read_file(d + "/mnist_tests_expect.bin", expect.data(), expect.size() * 4);
            int ok = 0, lab = 0;
            for (int t = 0; t < N; t++) {
                memcpy(g_img, &tests[t * (IMG_N + 1)], IMG_N);
                const int label = (int8_t)tests[t * (IMG_N + 1) + IMG_N];
                run_hw(CTRL_SRC_IMG | (t == 0 ? CTRL_RELOAD : 0));
                ref_net(g_w, g_p, g_img, ref);
                int e = cmp_out("trained/ref", g_out, ref);
                if (have_exp) e += cmp_out("trained/py", g_out, &expect[t * OUT_N]);
                ok += (e == 0); lab += (g_out[FC_CLS] == label); errs += e;
            }
            printf("trained vectors (%s): %d/%d bit-exact%s, %d/%d correct labels\n", d.c_str(), ok, N,
                   have_exp ? " vs Python" : " (no expect file)", lab, N);
        } else {
            if (f) fclose(f);
            printf("trained vectors: none in %s (skipped)\n", d.c_str());
        }
    }

    printf(errs ? "*** mbdpu03 C-sim FAILED (%d mismatches) ***\n" : "*** mbdpu03 C-sim PASSED ***\n", errs);
    return errs ? 1 : 0;
}
