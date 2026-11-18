/* 17_batch.c - the same weights, used by 1 token or by 256 tokens.
 *
 * C[B][N] = A[B][K] * W[K][N], K = N = 4096 -> W is 64 MiB, well above L3.
 * B = 1 is decode (one token per pass). Raising B reuses each weight line
 * B times, which raises arithmetic intensity linearly and pushes the kernel
 * from the sloped part of the roofline onto the flat roof.
 *
 * Two kernels, because the two ends of the sweep are limited by different
 * things: B < 8 runs a plain axpy per token (memory bound, no reuse to
 * exploit); B >= 8 runs an 8x8 register-blocked kernel so the flat roof is
 * measured with a kernel that can actually reach it.
 */
#include "common.h"
#include <immintrin.h>

#define K 4096
#define N 4096
#define MAXB 256
#define KC 128
#define NC 256
#define MR 8
#define NR 8

static float *W, *A, *C;

static void gemv_axpy(int B)
{
    for (int jb = 0; jb < N; jb += NC)
        for (int kb = 0; kb < K; kb += KC)
            for (int b = 0; b < B; b++)
                for (int k = kb; k < kb + KC; k++) {
                    float ak = A[b * K + k];
                    const float *w = &W[k * N + jb];
                    float *cr = &C[b * N + jb];
                    for (int j = 0; j < NC; j++) cr[j] += ak * w[j];
                }
}

static void gemm_blocked(int B)
{
    for (int jb = 0; jb < N; jb += NC)
        for (int kb = 0; kb < K; kb += KC)
            for (int b0 = 0; b0 + MR <= B; b0 += MR)
                for (int j0 = jb; j0 < jb + NC; j0 += NR) {
                    __m256 c0 = _mm256_load_ps(&C[(b0 + 0) * N + j0]);
                    __m256 c1 = _mm256_load_ps(&C[(b0 + 1) * N + j0]);
                    __m256 c2 = _mm256_load_ps(&C[(b0 + 2) * N + j0]);
                    __m256 c3 = _mm256_load_ps(&C[(b0 + 3) * N + j0]);
                    __m256 c4 = _mm256_load_ps(&C[(b0 + 4) * N + j0]);
                    __m256 c5 = _mm256_load_ps(&C[(b0 + 5) * N + j0]);
                    __m256 c6 = _mm256_load_ps(&C[(b0 + 6) * N + j0]);
                    __m256 c7 = _mm256_load_ps(&C[(b0 + 7) * N + j0]);
                    for (int k = kb; k < kb + KC; k++) {
                        __m256 w = _mm256_load_ps(&W[k * N + j0]);
                        c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 0) * K + k]), w, c0);
                        c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 1) * K + k]), w, c1);
                        c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 2) * K + k]), w, c2);
                        c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 3) * K + k]), w, c3);
                        c4 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 4) * K + k]), w, c4);
                        c5 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 5) * K + k]), w, c5);
                        c6 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 6) * K + k]), w, c6);
                        c7 = _mm256_fmadd_ps(_mm256_set1_ps(A[(b0 + 7) * K + k]), w, c7);
                    }
                    _mm256_store_ps(&C[(b0 + 0) * N + j0], c0);
                    _mm256_store_ps(&C[(b0 + 1) * N + j0], c1);
                    _mm256_store_ps(&C[(b0 + 2) * N + j0], c2);
                    _mm256_store_ps(&C[(b0 + 3) * N + j0], c3);
                    _mm256_store_ps(&C[(b0 + 4) * N + j0], c4);
                    _mm256_store_ps(&C[(b0 + 5) * N + j0], c5);
                    _mm256_store_ps(&C[(b0 + 6) * N + j0], c6);
                    _mm256_store_ps(&C[(b0 + 7) * N + j0], c7);
                }
}

static void bench(int B)
{
    uint64_t v[3];
    for (int r = 0; r < 3; r++) {
        memset(C, 0, (size_t)B * N * sizeof(float));
        uint64_t t0 = now_ns();
        if (B >= MR) gemm_blocked(B); else gemv_axpy(B);
        uint64_t t1 = now_ns();
        keep((uint64_t)(C[r] * 1e6));
        v[r] = t1 - t0;
    }
    uint64_t t = median3(v[0], v[1], v[2]);
    double flops = 2.0 * (double)B * (double)K * (double)N;
    /* DRAM traffic model: weights once, activations and output once each.
     * The C tile is re-read from L1/L2 per k-block, not from DRAM. */
    double bytes = (double)K * N * 4.0 + (double)B * K * 4.0 + (double)B * N * 4.0;
    printf("RES|batch|%d|%.2f|%.2f|%.2f|%.3f\n", B, (double)t / 1e6,
           flops / (double)t, bytes / (double)t, flops / bytes);
}

/* spot-check one element against a naive dot product */
static double check(int B, int b, int j)
{
    double s = 0;
    for (int k = 0; k < K; k++) s += (double)A[b * K + k] * (double)W[k * N + j];
    return s;
}

int main(void)
{
    W = xalloc((size_t)K * N * 4);
    A = xalloc((size_t)MAXB * K * 4);
    C = xalloc((size_t)MAXB * N * 4);
    for (size_t i = 0; i < (size_t)K * N; i++) W[i] = (float)(i & 255) * 0.001f;
    for (size_t i = 0; i < (size_t)MAXB * K; i++) A[i] = (float)(i & 63) * 0.01f;

    { uint64_t s = 0; for (size_t i = 0; i < (size_t)K * N; i += 16) s += (uint64_t)W[i]; keep(s); }

    bench(1);
    bench(2);
    bench(4);
    bench(8);
    bench(16);
    bench(32);
    bench(64);
    bench(256);

    /* correctness: recompute two elements of the B=256 result */
    double e0 = check(256, 0, 0), a0 = C[0];
    double e1 = check(256, 255, 4095), a1 = C[255 * N + 4095];
    double rel0 = (a0 - e0) / (e0 ? e0 : 1.0), rel1 = (a1 - e1) / (e1 ? e1 : 1.0);
    printf("RES|check|c00|%.6f|%.6f|%.2e\n", a0, e0, rel0);
    printf("RES|check|c255_4095|%.6f|%.6f|%.2e\n", a1, e1, rel1);
    return 0;
}
