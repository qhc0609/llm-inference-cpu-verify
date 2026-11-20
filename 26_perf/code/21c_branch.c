/* 21c_branch.c - 分支预测失败的代价：同一批数据，排序 vs 随机
 * noinline + 真分支，防止编译器改成无分支写法（cmov/算术技巧）。
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
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

#define N 1048576

__attribute__((noinline)) long branch_sum(const int *a, long n, int threshold) {
    long s = 0;
    for (long i = 0; i < n; i++) {
        if (__builtin_expect(a[i] >= threshold, 0)) s += a[i];
        else s -= 1;
    }
    return s;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(int argc, char **argv) {
    const char *order = argc > 1 ? argv[1] : "sorted";   /* sorted | random */
    int *a = aligned_alloc(64, N * sizeof(int));
    unsigned seed = 12345u;
    for (long i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        a[i] = (int)((seed >> 16) % 256);
    }
    if (!strcmp(order, "sorted")) qsort(a, N, sizeof(int), cmp_int);

    int threshold = 128;
    volatile long sink = branch_sum(a, N / 8, threshold);   /* 热身 */
    uint64_t c0 = rdtsc(); double t0 = now_ns();
    long s = branch_sum(a, N, threshold);
    double t1 = now_ns(); uint64_t c1 = rdtsc();
    sink = s;

    printf("ORDER=%s\nN=%d\nNS=%.0f\nCYCLES=%llu\nCYC_PER_ELEM=%.4f\nNS_PER_ELEM=%.4f\n",
           order, N, t1 - t0, (unsigned long long)(c1 - c0),
           (double)(c1 - c0) / N, (t1 - t0) / N);
    free(a);
    return 0;
}
