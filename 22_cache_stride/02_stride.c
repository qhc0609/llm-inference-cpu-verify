/* 02_stride.c - fixed 8,000,000 accesses, only the stride changes.
 *
 * 64 MB array, index walks by stride/4 ints with a power-of-two mask, so every
 * stride keeps hammering the same 64 MB but touches a different number of
 * distinct 64-byte lines. The inner loop is unrolled four ways with four
 * independent accumulators so the measurement reflects memory, not a serial
 * add chain.
 *
 * output: RES|stride|<bytes>|<ns per access>
 */
#include "common.h"

#define ACCESSES 8000000ull
#define ARRAY_BYTES (64u << 20)

int main(void)
{
    int n = (int)(ARRAY_BYTES / sizeof(int));       /* 16 Mi ints, power of two */
    uint64_t mask = (uint64_t)(n - 1);
    int *a = (int *)xalloc(ARRAY_BYTES);
    fill_ints(a, (size_t)n, 11);

    int strides[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int ns_count = (int)(sizeof(strides) / sizeof(strides[0]));

    for (int s = 0; s < ns_count; s++) {
        uint64_t step = (uint64_t)strides[s] / sizeof(int);
        uint64_t best[3];
        for (int r = 0; r < 4; r++) {               /* r == 0: warm-up */
            uint64_t idx = 0;
            uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            uint64_t i = 0;
            uint64_t t0 = now_ns();
            for (; i + 4 <= ACCESSES; i += 4) {
                s0 += (uint64_t)a[idx]; idx = (idx + step) & mask;
                s1 += (uint64_t)a[idx]; idx = (idx + step) & mask;
                s2 += (uint64_t)a[idx]; idx = (idx + step) & mask;
                s3 += (uint64_t)a[idx]; idx = (idx + step) & mask;
            }
            for (; i < ACCESSES; i++) {
                s0 += (uint64_t)a[idx];
                idx = (idx + step) & mask;
            }
            uint64_t dt = now_ns() - t0;
            keep(s0 + s1 + s2 + s3);
            if (r > 0)
                best[r - 1] = dt;
        }
        double ns = (double)median3(best[0], best[1], best[2]) / (double)ACCESSES;
        RES("stride", "%d|%.3f", strides[s], ns);
        fflush(stdout);
    }
    return 0;
}
