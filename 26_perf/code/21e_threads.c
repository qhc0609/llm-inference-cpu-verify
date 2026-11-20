/* 21e_threads.c - 固定总工作量，线程数从 1 加到 2×核数
 * 观察：时间不再变短、上下文切换暴涨 —— 这就是「算力够但被调度拖住」。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

static inline uint64_t rdtsc(void) {
    unsigned lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}
static double now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
/* 整个进程累计占用 CPU 的时间：墙钟被挂起时它不涨，
 * 用它除以墙钟就是"实际有幾個核在干活"（有效并行度）。 */
static double now_cpu(void) {
    struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

typedef struct { double *w; long n; long iters; double out; } arg_t;

static void *worker(void *p) {
    arg_t *a = (arg_t *)p;
    double s = 0;
    for (long it = 0; it < a->iters; it++) {
        double acc = 0;
        for (long i = 0; i < a->n; i++) acc += a->w[i] * (double)(i & 7);
        s += acc;
    }
    a->out = s;
    return NULL;
}

static long ctxt(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; long v = 0, nv = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "voluntary_ctxt_switches:", 24)) v = atol(line + 24);
        if (!strncmp(line, "nonvoluntary_ctxt_switches:", 27)) nv = atol(line + 27);
    }
    if (f) fclose(f);
    return v + nv;
}

int main(int argc, char **argv) {
    int nt = argc > 1 ? atoi(argv[1]) : 1;
    long n = argc > 2 ? atol(argv[2]) : 262144;      /* 2 MiB 工作集 */
    long total_iters = argc > 3 ? atol(argv[3]) : 400;

    double *w = aligned_alloc(64, n * sizeof(double));
    for (long i = 0; i < n; i++) w[i] = 1.0 / (double)(i + 1);

    pthread_t th[64];
    arg_t a[64];
    long per = total_iters / nt; if (per < 1) per = 1;

    long c0 = ctxt();
    uint64_t t0c = rdtsc(); double t0 = now_ns(); double u0 = now_cpu();
    for (int i = 0; i < nt; i++) {
        a[i].w = w; a[i].n = n; a[i].iters = per; a[i].out = 0;
        pthread_create(&th[i], NULL, worker, &a[i]);
    }
    double sum = 0;
    for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); sum += a[i].out; }
    double t1 = now_ns(); uint64_t t1c = rdtsc(); double u1 = now_cpu();
    long c1 = ctxt();

    double wall = t1 - t0, cpu = u1 - u0;
    printf("THREADS=%d\nWORK=%.0f_MAC\nNS=%.0f\nCYCLES=%llu\nCPUS=%.0f\nPARALLEL=%.2f\n"
           "CTXT_SWITCHES=%ld\nPER_THREAD_SW=%.1f\nSINK=%.0f\n",
           nt, (double)per * nt * n, wall, (unsigned long long)(t1c - t0c), cpu,
           wall > 0 ? cpu / wall : 0.0,
           c1 - c0, (double)(c1 - c0) / nt, sum);
    free(w);
    return 0;
}
