// mbdpu02_tb.cpp — C testbench for the mbdpu02 HLS core (csim only; see run_hls.tcl).
// SPDX-License-Identifier: Apache-2.0
#include "mbdpu02.h"
// =====================================================================
//  Self-checking C testbench (host only; excluded from synthesis).
//  1) Pseudo-random weights/image vs an independent golden model (datapath, bit-exact).
//  2) If $MBDPU02_TESTDIR holds the trainer's mnist_{w,p}blob.bin + mnist_tests.bin +
//     mnist_tests_expect.bin, replay those vectors and compare all 13 outputs with the
//     Python INT8 model's expected output (trained-model, bit-exact).
// =====================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int8_t  g_w[WBLOB_N];
static int32_t g_p[PBLOB_N];
static int8_t  g_img[IN_H*IN_W];
static int32_t g_out[OUT_N];
static int32_t g_ref[OUT_N];

static int8_t sat8(int v){ if(v>127)v=127; if(v<-127)v=-127; return (int8_t)v; }

// plain-C golden (independent re-implementation of the forward pass)
static void golden()
{
    int8_t a1[IN_H][IN_W][C1], p1[P1_H][P1_W][C1];
    int8_t a2[P1_H][P1_W][C2], p2[P2_H][P2_W][C2];
    int32_t c1M=g_p[OFF_C1M], c1S=g_p[OFF_C1S], c2M=g_p[OFF_C2M], c2S=g_p[OFF_C2S];
    #define REQ(acc,bias,M,S) sat8((int)(((( (int64_t)((acc)+(bias)) )*(M)) + ((int64_t)1<<((S)-1))) >> (S)) < 0 ? 0 : \
                                   (int)(((( (int64_t)((acc)+(bias)) )*(M)) + ((int64_t)1<<((S)-1))) >> (S)))
    for(int oc=0;oc<C1;oc++)for(int oh=0;oh<IN_H;oh++)for(int ow=0;ow<IN_W;ow++){
        int32_t acc=0;
        for(int kh=0;kh<KS;kh++)for(int kw=0;kw<KS;kw++){int ih=oh+kh-1,iw=ow+kw-1;
            if(ih>=0&&ih<IN_H&&iw>=0&&iw<IN_W) acc+=(int32_t)g_img[ih*IN_W+iw]*g_w[OFF_C1W+((oc*1+0)*KS+kh)*KS+kw];}
        a1[oh][ow][oc]=REQ(acc,g_p[OFF_C1B+oc],c1M,c1S);
    }
    for(int oc=0;oc<C1;oc++)for(int oh=0;oh<P1_H;oh++)for(int ow=0;ow<P1_W;ow++){int8_t m=-128;
        for(int dh=0;dh<2;dh++)for(int dw=0;dw<2;dw++){int8_t v=a1[oh*2+dh][ow*2+dw][oc]; if(v>m)m=v;} p1[oh][ow][oc]=m;}
    for(int oc=0;oc<C2;oc++)for(int oh=0;oh<P1_H;oh++)for(int ow=0;ow<P1_W;ow++){
        int32_t acc=0;
        for(int ic=0;ic<C1;ic++)for(int kh=0;kh<KS;kh++)for(int kw=0;kw<KS;kw++){int ih=oh+kh-1,iw=ow+kw-1;
            if(ih>=0&&ih<P1_H&&iw>=0&&iw<P1_W) acc+=(int32_t)p1[ih][iw][ic]*g_w[OFF_C2W+(((oc*C1+ic)*KS)+kh)*KS+kw];}
        a2[oh][ow][oc]=REQ(acc,g_p[OFF_C2B+oc],c2M,c2S);
    }
    for(int oc=0;oc<C2;oc++)for(int oh=0;oh<P2_H;oh++)for(int ow=0;ow<P2_W;ow++){int8_t m=-128;
        for(int dh=0;dh<2;dh++)for(int dw=0;dw<2;dw++){int8_t v=a2[oh*2+dh][ow*2+dw][oc]; if(v>m)m=v;} p2[oh][ow][oc]=m;}
    int best=0; int32_t bv=-2147483647;
    for(int o=0;o<FC_OUT;o++){
        int32_t acc = (o<FC_CLS) ? g_p[OFF_FCB+o] : g_p[OFF_POSB+(o-FC_CLS)]; int idx=0;
        for(int oh=0;oh<P2_H;oh++)for(int ow=0;ow<P2_W;ow++)for(int oc=0;oc<C2;oc++){
            acc+=(int32_t)p2[oh][ow][oc]*g_w[OFF_FCW+o*FC_IN+idx]; idx++;}
        if(o<FC_CLS){ g_ref[o]=acc; if(acc>bv){bv=acc;best=o;} }
        else g_ref[o+1]=acc;
    }
    g_ref[FC_CLS]=best;
}

static int compare(const char *tag)
{
    int errs=0;
    for(int o=0;o<OUT_N;o++) if(g_out[o]!=g_ref[o]){
        printf("  %s MISMATCH out[%d] hw=%d ref=%d\n", tag, o, g_out[o], g_ref[o]); errs++;
    }
    return errs;
}

static bool load(const std::string &path, void *dst, size_t n)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    size_t r = fread(dst, 1, n, f); fclose(f);
    return r == n;
}

int main()
{
    // ---- 1) datapath test: deterministic pseudo-random weights + image (LCG) ----
    uint32_t s=12345;
    #define RND ((s = s*1103515245u+12345u), (int)((s>>16)&0xff))
    for(int i=0;i<WBLOB_N;i++) g_w[i]=(int8_t)((RND%15)-7);      // weights in [-7,7]
    for(int i=0;i<IN_H*IN_W;i++) g_img[i]=(int8_t)(RND%50);      // image in [0,49]
    for(int i=0;i<C1+C2+FC_CLS;i++) g_p[i]=(RND%64)-32;          // biases
    g_p[OFF_C1M]=1<<14; g_p[OFF_C1S]=15;                         // requant ~ *0.5
    g_p[OFF_C2M]=1<<14; g_p[OFF_C2S]=16;                         // requant ~ *0.25
    g_p[OFF_POSB]=(RND%64)-32; g_p[OFF_POSB+1]=(RND%64)-32;      // position biases
    g_p[OFF_POSK]=0; g_p[OFF_POSH]=0;                            // firmware-only (unused)

    golden();
    mbdpu02(g_w, g_p, g_img, g_out);
    int errs = compare("lcg");
    printf("mbdpu02 C-sim (lcg): pred=%d dx=%d dy=%d (ref %d %d %d) -> %s\n",
           g_out[FC_CLS], g_out[FC_CLS+1], g_out[FC_CLS+2],
           g_ref[FC_CLS], g_ref[FC_CLS+1], g_ref[FC_CLS+2], errs==0 ? "PASS" : "FAIL");

    // ---- 2) trained-model vectors from the Python INT8 model (optional) ----
    const char *dir = getenv("MBDPU02_TESTDIR");
    if (dir) {
        std::string d(dir);
        static int8_t tests[64][IN_H*IN_W + 1];
        static int32_t expect[64][OUT_N];
        FILE *f = fopen((d + "/mnist_tests.bin").c_str(), "rb");
        size_t nt = 0;
        if (f) { nt = fread(tests, IN_H*IN_W + 1, 64, f); fclose(f); }
        bool ok = nt > 0 && load(d + "/mnist_wblob.bin", g_w, sizeof g_w)
                         && load(d + "/mnist_pblob.bin", g_p, sizeof g_p)
                         && load(d + "/mnist_tests_expect.bin", expect, nt * OUT_N * 4);
        if (!ok) {
            printf("mbdpu02 C-sim (trained): vectors not found in %s - skipped\n", dir);
        } else {
            int terrs = 0, correct = 0;
            for (size_t t = 0; t < nt; t++) {
                memcpy(g_img, tests[t], IN_H*IN_W);
                mbdpu02(g_w, g_p, g_img, g_out);
                memcpy(g_ref, expect[t], sizeof g_ref);
                char tag[16]; snprintf(tag, sizeof tag, "vec%zu", t);
                terrs += compare(tag);
                golden();                                // also cross-check the C golden model
                terrs += compare("golden");
                correct += (g_out[FC_CLS] == tests[t][IN_H*IN_W]);
            }
            printf("mbdpu02 C-sim (trained): %zu vectors, %d/%zu correct labels -> %s\n",
                   nt, correct, nt, terrs==0 ? "PASS (bit-exact to Python INT8 model)" : "FAIL");
            errs += terrs;
        }
    }
    return errs ? 1 : 0;
}
