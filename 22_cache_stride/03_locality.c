/* 03_locality.c - spatial vs temporal locality, 8,000,000 accesses each.
 *
 * A: 32 KB buffer read again and again          -> fits in L1d
 * B: 128 MB scanned in order, same access count -> far beyond L3, but predictable
 * C: 128 MB scanned with a prime stride (32 KB)  -> far beyond L3, unpredictable
 *
 * B and C are the reworked version: the first attempt compared "32 KB repeated"
 * with "128 MB sequential" and got two nearly identical numbers, because the
 * hardware prefetcher hid the DRAM latency. C defeats the prefetcher.
 *
 * output: RES|locality|<case>|<ns per access>
 */
#include "common.h"

#define ACCESSES 8000000ull

static double run_fixed(int *a, uint64_t n, uint64_t accesses)
{
    uint64_t mask = n - 1;
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        uint64_t idx = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0, i = 0;
        uint64_t t0 = now_ns();
        for (; i + 4 <= accesses; i += 4) {
            s0 += (uint64_t)a[idx]; idx = (idx + 1) & mask;
            s1 += (uint64_t)a[idx]; idx = (idx + 1) & mask;
            s2 += (uint64_t)a[idx]; idx = (idx + 1) & mask;
            s3 += (uint64_t)a[idx]; idx = (idx + 1) & mask;
        }
        for (; i < accesses; i++) {
            s0 += (uint64_t)a[idx];
            idx = (idx + 1) & mask;
        }
        uint64_t dt = now_ns() - t0;
        keep(s0 + s1 + s2 + s3);
        if (r > 0)
            best[r - 1] = dt;
    }
    return (double)median3(best[0], best[1], best[2]) / (double)accesses;
}

static double run_prime(int *a, uint64_t n, uint64_t accesses, uint64_t step)
{
    uint64_t mask = n - 1;
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        uint64_t idx = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0, i = 0;
        uint64_t t0 = now_ns();
        for (; i + 4 <= accesses; i += 4) {
            s0 += (uint64_t)a[idx]; idx = (idx + step) & mask;
            s1 += (uint64_t)a[idx]; idx = (idx + step) & mask;
            s2 += (uint64_t)a[idx]; idx = (idx + step) & mask;
            s3 += (uint64_t)a[idx]; idx = (idx + step) & mask;
        }
        for (; i < accesses; i++) {
            s0 += (uint64_t)a[idx];
            idx = (idx + step) & mask;
        }
        uint64_t dt = now_ns() - t0;
        keep(s0 + s1 + s2 + s3);
        if (r > 0)
            best[r - 1] = dt;
    }
    return (double)median3(best[0], best[1], best[2]) / (double)accesses;
}

int main(void)
{
    int *small = (int *)xalloc(32u << 10);
    fill_ints(small, (32u << 10) / sizeof(int), 3);

    int *big = (int *)xalloc(128u << 20);
    fill_ints(big, (128u << 20) / sizeof(int), 5);

    printf("RES|locality|small_32k|%.3f\n", run_fixed(small, (32u << 10) / 4, ACCESSES));
    printf("RES|locality|big_seq_128m|%.3f\n",
           run_fixed(big, (128u << 20) / 4, ACCESSES));
    /* 8191 ints = 32764 B, an odd prime step: co-prime with the power-of-two mask */
    printf("RES|locality|big_prime_128m|%.3f\n",
           run_prime(big, (128u << 20) / 4, ACCESSES, 8191));
    fflush(stdout);
    return 0;
}
