/* 08_gemm.c - 1024x1024 fp32 matrix multiply, naive ijk vs blocked ijk.
 *
 * Same data, same compiler (-O2), only the loop blocking changes.
 * flops = 2 * N^3 = 2.147 G
 *
 * output: RES|gemm|<label>|<block>|<ms>|<GFLOPS>
 */
#include "common.h"

#define N 1024

static float *A, *B, *C;

static void naive(void)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < N; k++)
                s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

static void blocked(int bs)
{
    /* i-k-j order with the j loop innermost: B is walked along a row, and the
     * C tile stays in cache while every k of the block is applied to it. */
    for (int ii = 0; ii < N; ii += bs)
        for (int kk = 0; kk < N; kk += bs)
            for (int jj = 0; jj < N; jj += bs)
                for (int i = ii; i < ii + bs && i < N; i++)
                    for (int k = kk; k < kk + bs && k < N; k++) {
                        float aik = A[i * N + k];
                        const float *brow = &B[k * N];
                        float *crow = &C[i * N];
                        for (int j = jj; j < jj + bs && j < N; j++)
                            crow[j] += aik * brow[j];
                    }
}

static double time_it(void (*fn)(void), int bs, double *gflops)
{
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        uint64_t t0 = now_ns();
        if (fn)
            fn();
        else
            blocked(bs);
        uint64_t dt = now_ns() - t0;
        if (r > 0)
            best[r - 1] = dt;
    }
    uint64_t dt = median3(best[0], best[1], best[2]);
    double ms = (double)dt / 1e6;
    *gflops = (2.0 * (double)N * N * N) / ((double)dt / 1e9) / 1e9;
    return ms;
}

int main(void)
{
    A = (float *)xalloc((size_t)N * N * sizeof(float));
    B = (float *)xalloc((size_t)N * N * sizeof(float));
    C = (float *)xalloc((size_t)N * N * sizeof(float));
    for (int i = 0; i < N * N; i++) {
        A[i] = (float)((i % 17) - 8) / 8.0f;
        B[i] = (float)((i % 13) - 6) / 8.0f;
    }

    double g;
    double ms = time_it(naive, 0, &g);
    printf("RES|gemm|naive|0|%.2f|%.2f\n", ms, g);
    fflush(stdout);

    int blocks[] = {16, 32, 64, 128};
    for (int i = 0; i < 4; i++) {
        ms = time_it(NULL, blocks[i], &g);
        printf("RES|gemm|blocked|%d|%.2f|%.2f\n", blocks[i], ms, g);
        fflush(stdout);
    }
    keep((uint64_t)C[0]);
    return 0;
}
