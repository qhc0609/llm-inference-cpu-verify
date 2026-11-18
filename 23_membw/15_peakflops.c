/* 15_peakflops.c - the other edge of the roofline: peak FP32 FMA throughput.
 *
 * 8 independent accumulators keep both FMA pipes fed; 1 accumulator shows what
 * happens when a dependency chain is the only source of work (the FP analogue
 * of a single pointer chain).
 */
#include "common.h"
#include <immintrin.h>
#include <pthread.h>

#define ITERS 100000000L

typedef struct { int tid; float r; } job_t;

static float body(int accs)
{
    __m256 c = _mm256_set1_ps(1e-7f);
    __m256 x = _mm256_set1_ps(1.0000001f);
    __m256 s0 = _mm256_set1_ps(0.5f), s1 = s0, s2 = s0, s3 = s0;
    __m256 s4 = s0, s5 = s0, s6 = s0, s7 = s0;

    if (accs == 1) {
        for (long i = 0; i < ITERS; i++) s0 = _mm256_fmadd_ps(s0, x, c);
    } else {
        for (long i = 0; i < ITERS; i++) {
            s0 = _mm256_fmadd_ps(s0, x, c);
            s1 = _mm256_fmadd_ps(s1, x, c);
            s2 = _mm256_fmadd_ps(s2, x, c);
            s3 = _mm256_fmadd_ps(s3, x, c);
            s4 = _mm256_fmadd_ps(s4, x, c);
            s5 = _mm256_fmadd_ps(s5, x, c);
            s6 = _mm256_fmadd_ps(s6, x, c);
            s7 = _mm256_fmadd_ps(s7, x, c);
        }
    }
    s0 = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    s4 = _mm256_add_ps(_mm256_add_ps(s4, s5), _mm256_add_ps(s6, s7));
    s0 = _mm256_add_ps(s0, s4);
    float out[8];
    _mm256_storeu_ps(out, s0);
    return out[0] + out[1] + out[2] + out[3] + out[4] + out[5] + out[6] + out[7];
}

static void *th_main(void *p)
{
    job_t *j = (job_t *)p;
    j->r = body(j->tid); /* tid doubles as "which variant", see main */
    return NULL;
}

/* flops per iteration: 8 lanes * 2 (mul+add) * accumulator count */
static double bench(int accs, int nt)
{
    uint64_t v[3];
    double flops = (double)ITERS * 16.0 * (double)accs * (double)nt;
    for (int r = 0; r < 3; r++) {
        pthread_t th[8];
        job_t j[8];
        uint64_t t0 = now_ns();
        for (int k = 0; k < nt; k++) { j[k].tid = accs; j[k].r = 0; pthread_create(&th[k], NULL, th_main, &j[k]); }
        double s = 0;
        for (int k = 0; k < nt; k++) { pthread_join(th[k], NULL); s += j[k].r; }
        uint64_t t1 = now_ns();
        keep((uint64_t)(s * 1e6));
        v[r] = t1 - t0;
    }
    uint64_t t = median3(v[0], v[1], v[2]);
    return flops / (double)t; /* flops/ns = GFLOPS */
}

int main(void)
{
    double one = bench(1, 1);
    double eight = bench(8, 1);
    double four = bench(8, 4);
    printf("RES|peak|fma1|1|%.2f\n", one);
    printf("RES|peak|fma8|1|%.2f\n", eight);
    printf("RES|peak|fma8|4|%.2f\n", four);
    return 0;
}
