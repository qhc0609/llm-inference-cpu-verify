/* 05_chase.c - pointer chasing through a random permutation.
 * The next address lives inside the current element, so the prefetcher has
 * nothing to lock on to. Sequential scan over the same footprint is the control.
 *
 * output: RES|chase|<MB>|random_ns|seq_ns|ratio
 */
#include "common.h"

#define STEPS 8000000ull

int main(void)
{
    int sizes_mb[] = {2, 16, 64, 256};
    int sn = (int)(sizeof(sizes_mb) / sizeof(sizes_mb[0]));

    for (int s = 0; s < sn; s++) {
        size_t bytes = (size_t)sizes_mb[s] << 20;
        int n = (int)(bytes / sizeof(int));
        int *next = (int *)xalloc(bytes);

        /* build a random cyclic permutation: next[i] = element visited after i */
        int *perm = (int *)xalloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++)
            perm[i] = i;
        unsigned seed = 12345u + (unsigned)s;
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(rand_r(&seed) % (unsigned)(i + 1));
            int t = perm[i];
            perm[i] = perm[j];
            perm[j] = t;
        }
        for (int i = 0; i < n - 1; i++)
            next[perm[i]] = perm[i + 1];
        next[perm[n - 1]] = perm[0];
        free(perm);

        uint64_t best[3];
        for (int r = 0; r < 4; r++) {
            int idx = 0;
            uint64_t sum = 0;
            uint64_t t0 = now_ns();
            for (uint64_t i = 0; i < STEPS; i++) {
                idx = next[idx];
                sum += (uint64_t)idx;
            }
            uint64_t dt = now_ns() - t0;
            keep(sum);
            if (r > 0)
                best[r - 1] = dt;
        }
        double rnd_ns = (double)median3(best[0], best[1], best[2]) / (double)STEPS;

        /* sequential control over the same footprint */
        for (int r = 0; r < 4; r++) {
            uint64_t sum = 0;
            uint64_t t0 = now_ns();
            for (int i = 0; i < n; i++)
                sum += (uint64_t)next[i];
            uint64_t dt = now_ns() - t0;
            keep(sum);
            if (r > 0)
                best[r - 1] = dt;
        }
        double seq_ns = (double)median3(best[0], best[1], best[2]) / (double)n;

        printf("RES|chase|%d|%.2f|%.3f|%.1f\n", sizes_mb[s], rnd_ns, seq_ns,
               rnd_ns / seq_ns);
        fflush(stdout);
        free(next);
    }
    return 0;
}
