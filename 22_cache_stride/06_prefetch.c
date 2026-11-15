/* 06_prefetch.c - manual __builtin_prefetch at several distances, over a
 * 256 MB array walked with a prime stride (prefetcher-hostile).
 *
 * distance 0 = no prefetch. Every other case issues one prefetch for the
 * address that will be needed `distance` steps later.
 *
 * output: RES|prefetch|<distance>|<ns per access>
 */
#include "common.h"

#define ACCESSES 8000000ull
#define ARRAY_BYTES (256u << 20)
#define STEP_INTS 8191ull        /* 32764 B, co-prime with the power-of-two mask */

int main(void)
{
    uint64_t n = (uint64_t)(ARRAY_BYTES / sizeof(int));
    uint64_t mask = n - 1;
    int *a = (int *)xalloc(ARRAY_BYTES);
    fill_ints(a, (size_t)n, 23);

    int dists[] = {0, 1, 2, 4, 8, 16, 32, 64};
    int dn = (int)(sizeof(dists) / sizeof(dists[0]));

    for (int d = 0; d < dn; d++) {
        uint64_t dist = (uint64_t)dists[d];
        uint64_t best[3];
        for (int r = 0; r < 4; r++) {
            uint64_t idx = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0, i = 0;
            uint64_t t0 = now_ns();
            for (; i + 4 <= ACCESSES; i += 4) {
                if (dist)
                    __builtin_prefetch(&a[(idx + dist * STEP_INTS) & mask], 0, 0);
                s0 += (uint64_t)a[idx]; idx = (idx + STEP_INTS) & mask;
                if (dist)
                    __builtin_prefetch(&a[(idx + dist * STEP_INTS) & mask], 0, 0);
                s1 += (uint64_t)a[idx]; idx = (idx + STEP_INTS) & mask;
                if (dist)
                    __builtin_prefetch(&a[(idx + dist * STEP_INTS) & mask], 0, 0);
                s2 += (uint64_t)a[idx]; idx = (idx + STEP_INTS) & mask;
                if (dist)
                    __builtin_prefetch(&a[(idx + dist * STEP_INTS) & mask], 0, 0);
                s3 += (uint64_t)a[idx]; idx = (idx + STEP_INTS) & mask;
            }
            for (; i < ACCESSES; i++) {
                s0 += (uint64_t)a[idx];
                idx = (idx + STEP_INTS) & mask;
            }
            uint64_t dt = now_ns() - t0;
            keep(s0 + s1 + s2 + s3);
            if (r > 0)
                best[r - 1] = dt;
        }
        double ns = (double)median3(best[0], best[1], best[2]) / (double)ACCESSES;
        RES("prefetch", "%d|%.3f", dists[d], ns);
        fflush(stdout);
    }
    return 0;
}
