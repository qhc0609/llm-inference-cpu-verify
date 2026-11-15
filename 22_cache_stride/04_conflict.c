/* 04_conflict.c - conflict misses: 256 KB footprint, 12,000,000 accesses,
 * only the stride changes. 4096 B walks the same L1 set over and over;
 * 4160 B (= 4096 + 64) shifts one line per access and spreads the same
 * 512 lines over all 64 sets, which is exactly 8 lines per set.
 *
 * The loop is deliberately serial (one load per iteration): this case measures
 * latency, not throughput, so the accesses must not overlap.
 *
 * output: RES|conflict|<stride>|<ns per access>
 */
#include "common.h"

#define ACCESSES 12000000ull
#define FOOTPRINT (256u << 10)

int main(void)
{
    int n = (int)(FOOTPRINT / sizeof(int));
    uint64_t mask = (uint64_t)(n - 1);
    int *a = (int *)xalloc(FOOTPRINT);
    fill_ints(a, (size_t)n, 17);

    int strides[] = {1024, 2048, 3968, 4096, 4160, 4224, 8192};
    int cn = (int)(sizeof(strides) / sizeof(strides[0]));

    for (int s = 0; s < cn; s++) {
        uint64_t step = (uint64_t)strides[s] / sizeof(int);
        uint64_t best[3];
        for (int r = 0; r < 4; r++) {
            uint64_t idx = 0, sum = 0;
            uint64_t t0 = now_ns();
            for (uint64_t i = 0; i < ACCESSES; i++) {
                sum += (uint64_t)a[idx];
                idx = (idx + step) & mask;
            }
            uint64_t dt = now_ns() - t0;
            keep(sum);
            if (r > 0)
                best[r - 1] = dt;
        }
        double ns = (double)median3(best[0], best[1], best[2]) / (double)ACCESSES;
        RES("conflict", "%d|%.3f", strides[s], ns);
        fflush(stdout);
    }
    return 0;
}
