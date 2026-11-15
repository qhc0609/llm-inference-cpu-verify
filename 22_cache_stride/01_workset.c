/* 01_workset.c - sequential sum over a working set, only the array size changes.
 *
 * Every row reads about 256 MB in total (size x repeats), so the byte count is
 * held roughly constant while the footprint walks from L1 to DRAM.
 *
 * The accumulation is a plain 32-bit sum so the compiler can vectorise it
 * (wrapping is defined for unsigned); otherwise the adder, not the cache,
 * becomes the bottleneck and the steps disappear.
 *
 * output: RES|workset|<bytes>|<repeats>|<ms>|<GB/s>|<ns per element>
 */
#include "common.h"

static void run_case(size_t bytes, int reps, int rounds, double *out_ms,
                     double *out_bw, double *out_ns)
{
    size_t n = bytes / sizeof(int);
    int *a = (int *)xalloc(bytes);
    fill_ints(a, n, 7);

    uint64_t best[3];
    for (int r = 0; r < rounds + 1; r++) {          /* r == 0 is the warm-up */
        uint64_t t0 = now_ns();
        uint32_t s = 0;
        for (int k = 0; k < reps; k++)
            for (size_t i = 0; i < n; i++)
                s += (uint32_t)a[i];
        uint64_t dt = now_ns() - t0;
        keep(s);
        if (r > 0)
            best[r - 1] = dt;
    }
    uint64_t dt = median3(best[0], best[1], best[2]);

    double total_bytes = (double)bytes * (double)reps;
    *out_ms = (double)dt / 1e6;
    *out_bw = total_bytes / ((double)dt / 1e9) / 1e9;
    *out_ns = (double)dt / ((double)reps * (double)n);

    free(a);
}

int main(void)
{
    struct { size_t bytes; int reps; } cases[] = {
        {16u << 10, 16384}, {16u << 10, 8192}, {16u << 10, 4096},
        {16u << 10, 2048},  {16u << 10, 1024}, {16u << 10, 512},
        {1u << 20, 256},    {2u << 20, 128},   {4u << 20, 64},
        {8u << 20, 32},     {16u << 20, 16},   {32u << 20, 8},
        {64u << 20, 4},     {128u << 20, 3},   {256u << 20, 3},
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        double ms, bw, ns;
        run_case(cases[i].bytes, cases[i].reps, 3, &ms, &bw, &ns);
        RES("workset", "%zu|%d|%.2f|%.2f|%.4f", cases[i].bytes, cases[i].reps,
            ms, bw, ns);
        fflush(stdout);
    }
    return 0;
}
