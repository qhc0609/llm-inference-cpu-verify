/* 19f_decode.c - one decode-shaped pass: y = W x, W is the whole weight matrix.
 *
 * 16384 x 4096 floats = 256 MiB, read once per "token", 2 flops per float, so
 * the arithmetic intensity is exactly 0.5 flop/B no matter how many threads run.
 * Threads are pinned to the cpus this process is allowed to use, in order.
 *
 * Usage: ./19f_decode <threads> [tokens_per_round] [rounds]
 * Wrap with numactl to move the weights and the workers around:
 *   numactl --membind=0 --cpunodebind=0 ./19f_decode 4      all local
 *   numactl --membind=0 --physcpubind=0,2 ./19f_decode 2    half remote
 *
 * Reports ms per token, the tokens/s it implies, the GFLOPS, and the weight
 * stream in GB/s. That last column is the point: 256 MiB per token means the
 * "bandwidth" a decode step asks for is just 256 MiB / ms.
 */
#include "common.h"
#include "numa_util.h"
#include <pthread.h>

static size_t ROWS = 16384, K = 4096;   /* 256 MiB of weights */
static float *W, *x, *y;

typedef struct { size_t lo, hi; int cpu; } job_t;

static void *gemv(void *p)
{
    job_t *j = (job_t *)p;
    pin_cpu(j->cpu);
    for (size_t r = j->lo; r < j->hi; r++) {
        const float *w = W + r * K;
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        size_t i = 0;
        for (; i + 4 <= K; i += 4) {
            a0 += w[i] * x[i];
            a1 += w[i + 1] * x[i + 1];
            a2 += w[i + 2] * x[i + 2];
            a3 += w[i + 3] * x[i + 3];
        }
        for (; i < K; i++) a0 += w[i] * x[i];
        y[r] = a0 + a1 + a2 + a3;
    }
    return NULL;
}

/* one "token": all threads sweep the whole W once; returns ns */
static uint64_t one_token(int nt, const int *cpu)
{
    size_t per = ROWS / (size_t)nt;
    pthread_t th[8];
    job_t j[8];
    uint64_t t0 = now_ns();
    for (int k = 0; k < nt; k++) {
        j[k].lo = (size_t)k * per;
        j[k].hi = (k == nt - 1) ? ROWS : (size_t)(k + 1) * per;
        j[k].cpu = cpu[k];
        pthread_create(&th[k], NULL, gemv, &j[k]);
    }
    for (int k = 0; k < nt; k++) pthread_join(th[k], NULL);
    uint64_t t1 = now_ns();
    keep((uint64_t)y[0]);
    return t1 - t0;
}

int main(int argc, char **argv)
{
    int nt = (argc > 1) ? atoi(argv[1]) : 1;
    int tpr = (argc > 2) ? atoi(argv[2]) : 4;      /* tokens per timed round */
    int rounds = (argc > 3) ? atoi(argv[3]) : 5;
    if (nt < 1) nt = 1;
    if (nt > 8) nt = 8;
    if (tpr < 1) tpr = 1;
    if (rounds < 1) rounds = 1;
    if (rounds > 16) rounds = 16;

    W = xalloc(ROWS * K * sizeof(float));
    x = xalloc(K * sizeof(float));
    y = xalloc(ROWS * sizeof(float));
    for (size_t i = 0; i < K; i++) x[i] = 1.0f;
    for (size_t i = 0; i < ROWS * K; i++) W[i] = 1.0f;  /* fault in, 256 MiB */

    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    int cpu[16], nc = 0;
    for (int c = 0; c < 64 && nc < 16; c++)
        if (CPU_ISSET(c, &set)) cpu[nc++] = c;
    if (nt > nc) nt = nc;

    one_token(nt, cpu);                    /* warm-up, thrown away */

    /* Every token is timed on its own and kept. On a shared host the slow ones
     * are host interference (schedule jitter, stolen vCPU time), not NUMA, so
     * the headline is the fastest token and the median is printed next to it
     * to show how much the box was kicking. */
    enum { MAXS = 256 };
    uint64_t all[MAXS];
    int ns_ = 0;
    for (int r = 0; r < rounds; r++)
        for (int t = 0; t < tpr && ns_ < MAXS; t++)
            all[ns_++] = one_token(nt, cpu);

    for (int a = 0; a < ns_; a++)
        for (int b = a + 1; b < ns_; b++)
            if (all[b] < all[a]) { uint64_t z = all[a]; all[a] = all[b]; all[b] = z; }
    uint64_t tmin = all[0];
    uint64_t tmed = all[ns_ / 2];
    uint64_t t = tmin;

    double ms = (double)t / 1e6;
    double flops = 2.0 * (double)ROWS * (double)K;
    double bytes = 4.0 * (double)ROWS * (double)K;
    /* flops per ns is already GFLOPS; bytes per ns is GB/s */
    printf("RES|dec|%d|%.2f|%.1f|%.2f|%.2f|%.2f|%.2f|%d\n",
           nt, ms, 1000.0 / ms, flops / (double)t, bytes / (double)t,
           (double)tmed / 1e6, (double)all[ns_ - 1] / 1e6, ns_);
    return 0;
}
