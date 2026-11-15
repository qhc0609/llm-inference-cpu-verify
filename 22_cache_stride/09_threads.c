/* 09_threads.c - two multi-core effects.
 *
 * A) false sharing: N threads each bump their own counter 12,000,000 times.
 *    adjacent = counters 8 B apart (same 64 B line)
 *    padded   = counters 128 B apart (one line each)
 *
 * B) aggregate bandwidth: 1..4 threads, each scans a private 128 MB array once.
 *
 * output:
 *   RES|false_share|<threads>|<adjacent ms>|<padded ms>|<ratio>
 *   RES|agg_bw|<threads>|<GB/s>
 */
#include "common.h"
#include <pthread.h>

#define BUMPS 12000000
#define ARR_BYTES (128u << 20)
#define ROUNDS 5

typedef struct {
    volatile long *ctr;
    int bumps;
} Job;

static void *bump(void *arg)
{
    Job *j = (Job *)arg;
    volatile long *c = j->ctr;
    /* plain volatile increment: not atomic, but every iteration does a
     * load + add + store against the shared line */
    for (int i = 0; i < j->bumps; i++)
        (*c)++;
    return NULL;
}

static uint64_t median_n(uint64_t *v, int n)
{
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    return v[n / 2];
}

static double run_share(int threads, int padded)
{
    size_t stride = padded ? 128 : 8;
    char *base = (char *)xalloc((size_t)threads * stride + 256);
    pthread_t th[8];
    Job jobs[8];
    uint64_t t[ROUNDS];

    for (int r = 0; r < ROUNDS + 1; r++) {
        uint64_t t0 = now_ns();
        for (int i = 0; i < threads; i++) {
            jobs[i].ctr = (volatile long *)(base + (size_t)i * stride);
            jobs[i].bumps = BUMPS;
            pthread_create(&th[i], NULL, bump, &jobs[i]);
        }
        for (int i = 0; i < threads; i++)
            pthread_join(th[i], NULL);
        uint64_t dt = now_ns() - t0;
        if (r > 0)
            t[r - 1] = dt;
    }
    free(base);
    return (double)median_n(t, ROUNDS) / 1e6;
}

typedef struct {
    int *a;
    size_t n;
    uint64_t sum;
} Scan;

static void *scan(void *arg)
{
    Scan *s = (Scan *)arg;
    const int *a = s->a;
    size_t n = s->n, i = 0;
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += (uint64_t)a[i];
        s1 += (uint64_t)a[i + 1];
        s2 += (uint64_t)a[i + 2];
        s3 += (uint64_t)a[i + 3];
    }
    for (; i < n; i++)
        s0 += (uint64_t)a[i];
    s->sum = s0 + s1 + s2 + s3;
    return NULL;
}

int main(void)
{
    int tcounts[] = {1, 2, 4};
    for (int i = 0; i < 3; i++) {
        int t = tcounts[i];
        double adj = run_share(t, 0);
        double pad = run_share(t, 1);
        printf("RES|false_share|%d|%.2f|%.2f|%.2f\n", t, adj, pad, adj / pad);
        fflush(stdout);
    }

    int *bufs[4];
    for (int t = 0; t < 4; t++) {
        bufs[t] = (int *)xalloc(ARR_BYTES);
        fill_ints(bufs[t], ARR_BYTES / sizeof(int), 31 + t);
    }

    for (int threads = 1; threads <= 4; threads++) {
        Scan sc[4];
        pthread_t th[4];
        uint64_t t[ROUNDS];
        for (int r = 0; r < ROUNDS + 1; r++) {
            uint64_t t0 = now_ns();
            for (int i = 0; i < threads; i++) {
                sc[i].a = bufs[i];
                sc[i].n = ARR_BYTES / sizeof(int);
                sc[i].sum = 0;
                pthread_create(&th[i], NULL, scan, &sc[i]);
            }
            uint64_t total = 0;
            for (int i = 0; i < threads; i++) {
                pthread_join(th[i], NULL);
                total += sc[i].sum;
            }
            uint64_t dt = now_ns() - t0;
            keep(total);
            if (r > 0)
                t[r - 1] = dt;
        }
        uint64_t dt = median_n(t, ROUNDS);
        double gbs = (double)threads * (double)ARR_BYTES / ((double)dt / 1e9) / 1e9;
        printf("RES|agg_bw|%d|%.2f\n", threads, gbs);
        fflush(stdout);
    }
    for (int t = 0; t < 4; t++)
        free(bufs[t]);
    return 0;
}
