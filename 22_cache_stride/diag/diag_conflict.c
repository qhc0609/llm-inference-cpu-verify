/* diag_conflict.c - find the footprint where the 4096 B stride really does
 * thrash one L1 set while 4160 B stays clean, on a machine whose L2 is 256 KB.
 * serial loop: this measures latency, not throughput.
 */
#include "common.h"

#define ACC 12000000ull

static double run(int *a, uint64_t n, uint64_t step)
{
    uint64_t mask = n - 1, best[3];
    for (int r = 0; r < 4; r++) {
        uint64_t idx = 0, sum = 0;
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < ACC; i++) {
            sum += (uint64_t)a[idx];
            idx = (idx + step) & mask;
        }
        uint64_t dt = now_ns() - t0;
        keep(sum);
        if (r > 0)
            best[r - 1] = dt;
    }
    return (double)median3(best[0], best[1], best[2]) / (double)ACC;
}

int main(void)
{
    size_t sizes[] = {32u << 10, 48u << 10, 64u << 10, 96u << 10, 128u << 10, 256u << 10};
    int strides[] = {1024, 4096, 4160, 4224};
    for (int s = 0; s < 6; s++) {
        size_t bytes = sizes[s];
        int *a = (int *)xalloc(bytes);
        fill_ints(a, bytes / 4, 17);
        printf("FOOT|%zu", bytes >> 10);
        for (int k = 0; k < 4; k++)
            printf("|%d=%.3f", strides[k], run(a, bytes / 4, strides[k] / 4));
        printf("\n");
        free(a);
    }
    return 0;
}
