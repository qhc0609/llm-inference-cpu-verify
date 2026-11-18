/* 16_ai_sweep.c - walk the arithmetic-intensity axis with one loop.
 *
 * Each 4-byte float is read once, then used R times in FMA. Bytes stay the
 * same, flops scale with R, so AI = 2*R/4 = R/2 runs 0.5 .. 64.
 * Scanning R from 1 to 128 is how you find the knee without trusting a spec
 * sheet: the point where GFLOPS stops climbing is the roofline corner.
 */
#include "common.h"
#include <immintrin.h>

#define NF (32u << 20) /* 32M floats = 128 MiB */

static float *a;

static void bench(int R)
{
    uint64_t v[3];
    int reps = (R >= 8) ? R / 8 : 1;   /* FMAs per accumulator per vector */
    double flops = 2.0 * (double)R * (double)NF;
    double bytes = 4.0 * (double)NF;

    for (int r = 0; r < 3; r++) {
        __m256 c = _mm256_set1_ps(1e-7f);
        __m256 s0 = _mm256_set1_ps(0.5f), s1 = s0, s2 = s0, s3 = s0;
        __m256 s4 = s0, s5 = s0, s6 = s0, s7 = s0;
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < NF; i += 8) {
            __m256 x = _mm256_load_ps(&a[i]);
            if (R == 1) {
                s0 = _mm256_fmadd_ps(x, s0, c);
            } else if (R == 2) {
                s0 = _mm256_fmadd_ps(x, s0, c);
                s1 = _mm256_fmadd_ps(x, s1, c);
            } else if (R == 4) {
                s0 = _mm256_fmadd_ps(x, s0, c);
                s1 = _mm256_fmadd_ps(x, s1, c);
                s2 = _mm256_fmadd_ps(x, s2, c);
                s3 = _mm256_fmadd_ps(x, s3, c);
            } else {
                for (int k = 0; k < reps; k++) {
                    s0 = _mm256_fmadd_ps(x, s0, c);
                    s1 = _mm256_fmadd_ps(x, s1, c);
                    s2 = _mm256_fmadd_ps(x, s2, c);
                    s3 = _mm256_fmadd_ps(x, s3, c);
                    s4 = _mm256_fmadd_ps(x, s4, c);
                    s5 = _mm256_fmadd_ps(x, s5, c);
                    s6 = _mm256_fmadd_ps(x, s6, c);
                    s7 = _mm256_fmadd_ps(x, s7, c);
                }
            }
        }
        uint64_t t1 = now_ns();
        s0 = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
        s4 = _mm256_add_ps(_mm256_add_ps(s4, s5), _mm256_add_ps(s6, s7));
        float out[8];
        _mm256_storeu_ps(out, _mm256_add_ps(s0, s4));
        keep((uint64_t)(out[0] * 1e6));
        v[r] = t1 - t0;
    }
    uint64_t t = median3(v[0], v[1], v[2]);
    printf("RES|ai|%.2f|%.2f|%.2f|%.2f\n", flops / bytes, flops / (double)t,
           bytes / (double)t, (double)t / 1e6);
}

int main(void)
{
    a = xalloc((size_t)NF * 4);
    for (size_t i = 0; i < NF; i++) a[i] = (float)(i & 1023) * 0.001f;

    bench(1);    /* AI 0.5 */
    bench(2);    /* AI 1   */
    bench(4);    /* AI 2   */
    bench(8);    /* AI 4   */
    bench(16);   /* AI 8   */
    bench(32);   /* AI 16  */
    bench(64);   /* AI 32  */
    bench(128);  /* AI 64  */
    return 0;
}
