/* 21b_loads.c - 三种典型负载：依赖链 / 四路并行 / 指针追逐
 * 目的：用真实 PMU 计数（perf stat）对照程序内 RDTSC 计数，
 *      看「同一颗核、不同写法」下 IPC 到底差多少。
 * 输出为 KEY=VALUE 行，便于脚本解析。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t rdtsc(void) {
    unsigned lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

#define ITERS 60000000L

__attribute__((noinline)) double load_dep(long n) {
    double x = 1.0;
    for (long i = 0; i < n; i++) {
        x = x * 1.0000001 + 1.0;      /* 每条都等上一条的结果 */
        __asm__ __volatile__("" : "+x"(x));
    }
    return x;
}

__attribute__((noinline)) double load_par4(long n) {
    double a = 1.0, b = 2.0, c = 3.0, d = 4.0;
    for (long i = 0; i < n; i++) {
        a = a * 1.0000001 + 1.0;
        b = b * 1.0000001 + 2.0;
        c = c * 1.0000001 + 3.0;
        d = d * 1.0000001 + 4.0;
        __asm__ __volatile__("" : "+x"(a), "+x"(b), "+x"(c), "+x"(d));
    }
    return a + b + c + d;
}

/* 指针追逐：下一次地址藏在这次读出的值里，必须排队等 */
__attribute__((noinline)) uint64_t load_chase(uint64_t *buf, long n, long stride_elems) {
    uint64_t idx = 0, sum = 0;
    for (long i = 0; i < n; i++) {
        idx = buf[idx];
        sum += idx;
    }
    return sum;
}

/* 随机置换环：每个节点指向一个随机位置，硬件预取器无从下手，
 * 每次访问都必须真等上一次的结果（这才是「等数据」的样子）。 */
static uint64_t *make_ring(size_t bytes, long stride_elems) {
    (void)stride_elems;
    size_t n = bytes / sizeof(uint64_t);
    uint64_t *buf = aligned_alloc(64, bytes);
    uint64_t *perm = malloc(n * sizeof(uint64_t));
    if (!buf || !perm) { perror("alloc"); exit(1); }
    for (size_t i = 0; i < n; i++) perm[i] = i;
    uint64_t s = 88172645463325252ull;
    for (size_t i = n - 1; i > 0; i--) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t j = s % (i + 1);
        uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (size_t i = 0; i + 1 < n; i++) buf[perm[i]] = perm[i + 1];
    buf[perm[n - 1]] = perm[0];
    free(perm);
    return buf;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "dep";
    long n = argc > 2 ? atol(argv[2]) : ITERS;
    volatile double sink;

    if (!strcmp(mode, "dep") || !strcmp(mode, "par4")) {
        /* 热身 */
        if (!strcmp(mode, "dep")) sink = load_dep(n / 20);
        else sink = load_par4(n / 20);
        uint64_t c0 = rdtsc(); double t0 = now_ns();
        double r = !strcmp(mode, "dep") ? load_dep(n) : load_par4(n);
        double t1 = now_ns(); uint64_t c1 = rdtsc();
        sink = r;
        printf("MODE=%s\nITERS=%ld\nNS=%.0f\nCYCLES=%llu\nCYC_PER_ITER=%.4f\nNS_PER_ITER=%.4f\n",
               mode, n, t1 - t0, (unsigned long long)(c1 - c0),
               (double)(c1 - c0) / n, (t1 - t0) / n);
    } else if (!strcmp(mode, "chase")) {
        size_t bytes = argc > 3 ? (size_t)atol(argv[3]) : (32u << 20);
        long stride = argc > 4 ? atol(argv[4]) : 8;   /* 8 elems = 64 B = 一行 */
        uint64_t *buf = make_ring(bytes, stride);
        /* 热身：走 1/20 */
        load_chase(buf, n / 20, stride);
        uint64_t c0 = rdtsc(); double t0 = now_ns();
        uint64_t s = load_chase(buf, n, stride);
        double t1 = now_ns(); uint64_t c1 = rdtsc();
        sink = (double)s;
        printf("MODE=chase\nITERS=%ld\nBYTES=%zu\nSTRIDE=%ld\nNS=%.0f\nCYCLES=%llu\nCYC_PER_ITER=%.4f\nNS_PER_ITER=%.4f\n",
               n, bytes, stride, t1 - t0, (unsigned long long)(c1 - c0),
               (double)(c1 - c0) / n, (t1 - t0) / n);
        free(buf);
    } else {
        fprintf(stderr, "usage: %s dep|par4|chase [iters] [bytes] [stride_elems]\n", argv[0]);
        return 1;
    }
    return (int)(sink == 12345.0);
}
