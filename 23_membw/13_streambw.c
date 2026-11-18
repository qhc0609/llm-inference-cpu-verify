/* 13_streambw.c - how many bytes per second can this machine actually move?
 *
 * Four kernels (read / copy / scale / triad) single-threaded, plus the read
 * kernel scaled to 1..4 threads. Every array is 256 MiB, far above the 6 MB
 * shared L3, so what is measured is DRAM, not cache.
 */
#include "common.h"
#include <pthread.h>

#define N (32u << 20) /* 32M u64 = 256 MiB per array */

static uint64_t *A, *B, *C;

typedef struct { const uint64_t *a; size_t lo, hi; uint64_t s; } job_t;

static void *th_sum(void *p)
{
    job_t *j = (job_t *)p;
    const uint64_t *a = j->a;
    size_t i = j->lo, hi = j->hi;
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (; i + 4 <= hi; i += 4) {
        s0 += a[i]; s1 += a[i + 1]; s2 += a[i + 2]; s3 += a[i + 3];
    }
    for (; i < hi; i++) s0 += a[i];
    j->s = s0 + s1 + s2 + s3;
    return NULL;
}

/* read, nt threads, three rounds -> median GB/s */
static void bench_read(int nt)
{
    uint64_t v[3];
    size_t per = N / (size_t)nt;
    for (int r = 0; r < 3; r++) {
        pthread_t th[8];
        job_t j[8];
        uint64_t t0 = now_ns();
        for (int k = 0; k < nt; k++) {
            j[k].a = A;
            j[k].lo = (size_t)k * per;
            j[k].hi = (k == nt - 1) ? N : (size_t)(k + 1) * per;
            j[k].s = 0;
            pthread_create(&th[k], NULL, th_sum, &j[k]);
        }
        uint64_t tot = 0;
        for (int k = 0; k < nt; k++) { pthread_join(th[k], NULL); tot += j[k].s; }
        uint64_t t1 = now_ns();
        keep(tot);
        v[r] = (uint64_t)((double)N * 8.0 / (double)(t1 - t0) * 1e9);
    }
    uint64_t m = median3(v[0], v[1], v[2]);
    printf("RES|bw|read|%d|%.2f\n", nt, (double)m / 1e9);
}

int main(void)
{
    A = xalloc((size_t)N * 8);
    B = xalloc((size_t)N * 8);
    C = xalloc((size_t)N * 8);
    for (size_t i = 0; i < N; i++) { A[i] = i * 3 + 1; B[i] = i * 5 + 2; }

    /* warm up: touch every page of all three arrays */
    { job_t j = {A, 0, N, 0}; th_sum(&j); keep(j.s); }

    bench_read(1);
    bench_read(2);
    bench_read(4);

    /* copy / scale / triad, single thread */
    {
        uint64_t v[3];
        for (int r = 0; r < 3; r++) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < N; i++) B[i] = A[i];
            uint64_t t1 = now_ns();
            keep(B[r]);
            v[r] = (uint64_t)((double)N * 16.0 / (double)(t1 - t0) * 1e9);
        }
        printf("RES|bw|copy|1|%.2f\n", (double)median3(v[0], v[1], v[2]) / 1e9);
    }
    {
        uint64_t v[3];
        for (int r = 0; r < 3; r++) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < N; i++) B[i] = A[i] * 3;
            uint64_t t1 = now_ns();
            keep(B[r]);
            v[r] = (uint64_t)((double)N * 16.0 / (double)(t1 - t0) * 1e9);
        }
        printf("RES|bw|scale|1|%.2f\n", (double)median3(v[0], v[1], v[2]) / 1e9);
    }
    {
        uint64_t v[3];
        for (int r = 0; r < 3; r++) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < N; i++) C[i] = A[i] + 3 * B[i];
            uint64_t t1 = now_ns();
            keep(C[r]);
            v[r] = (uint64_t)((double)N * 24.0 / (double)(t1 - t0) * 1e9);
        }
        printf("RES|bw|triad|1|%.2f\n", (double)median3(v[0], v[1], v[2]) / 1e9);
    }
    return 0;
}
