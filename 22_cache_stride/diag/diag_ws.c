/* diag_ws.c - which accumulation shape gives a realistic sequential-read
 * bandwidth on this machine: 4 scalar accumulators, a plain uint32 sum
 * (vectorisable), or a float sum.
 */
#include "common.h"

static double bw_4acc(int *a, size_t n, int reps)
{
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        uint64_t t0 = now_ns();
        for (int k = 0; k < reps; k++) {
            size_t i = 0;
            for (; i + 4 <= n; i += 4) {
                s0 += (uint64_t)a[i];
                s1 += (uint64_t)a[i + 1];
                s2 += (uint64_t)a[i + 2];
                s3 += (uint64_t)a[i + 3];
            }
            for (; i < n; i++)
                s0 += (uint64_t)a[i];
        }
        uint64_t dt = now_ns() - t0;
        keep(s0 + s1 + s2 + s3);
        if (r > 0)
            best[r - 1] = dt;
    }
    uint64_t dt = median3(best[0], best[1], best[2]);
    return (double)((double)n * 4.0 * reps) / ((double)dt / 1e9) / 1e9;
}

static double bw_u32(int *a, size_t n, int reps)
{
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        uint32_t s = 0;
        uint64_t t0 = now_ns();
        for (int k = 0; k < reps; k++)
            for (size_t i = 0; i < n; i++)
                s += (uint32_t)a[i];
        uint64_t dt = now_ns() - t0;
        keep(s);
        if (r > 0)
            best[r - 1] = dt;
    }
    uint64_t dt = median3(best[0], best[1], best[2]);
    return (double)((double)n * 4.0 * reps) / ((double)dt / 1e9) / 1e9;
}

static double bw_f32(int *a, size_t n, int reps)
{
    float *f = (float *)a;
    uint64_t best[3];
    for (int r = 0; r < 4; r++) {
        float s = 0;
        uint64_t t0 = now_ns();
        for (int k = 0; k < reps; k++)
            for (size_t i = 0; i < n; i++)
                s += f[i];
        uint64_t dt = now_ns() - t0;
        keep((uint64_t)s);
        if (r > 0)
            best[r - 1] = dt;
    }
    uint64_t dt = median3(best[0], best[1], best[2]);
    return (double)((double)n * 4.0 * reps) / ((double)dt / 1e9) / 1e9;
}

int main(void)
{
    size_t sizes[] = {16u << 10, 256u << 10, 1u << 20, 4u << 20, 16u << 20, 256u << 20};
    for (int s = 0; s < 6; s++) {
        size_t bytes = sizes[s];
        int *a = (int *)xalloc(bytes);
        fill_ints(a, bytes / 4, 9);
        int reps = (int)((256u << 20) / bytes);
        if (reps < 1) reps = 1;
        printf("WS|%zuKB|4acc=%.2f|u32=%.2f|f32=%.2f\n", bytes >> 10,
               bw_4acc(a, bytes / 4, reps), bw_u32(a, bytes / 4, reps),
               bw_f32(a, bytes / 4, reps));
        free(a);
    }
    return 0;
}
