/* 21d_wss.c - 工作集扫描：从 L1 到 DRAM，看每次访问的代价怎么跳台阶
 * 两个模式：
 *   seq  - 顺序读（硬件预取有效，测带宽）
 *   rnd  - 随机依赖链读（预取无效，测真实访存延迟）
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
static inline uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

__attribute__((noinline)) double seq_sum(const double *p, size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) { s0 += p[i]; s1 += p[i+1]; s2 += p[i+2]; s3 += p[i+3]; }
    for (; i < n; i++) s0 += p[i];
    return s0 + s1 + s2 + s3;
}

/* 随机依赖链：i = idx[i]，每次都等上一次的地址 */
__attribute__((noinline)) uint64_t rnd_chase(const uint64_t *idx, uint64_t start, long steps) {
    uint64_t k = start, sum = 0;
    for (long i = 0; i < steps; i++) { k = idx[k]; sum += k; }
    return sum;
}

static uint64_t *make_perm(size_t n, uint64_t seed) {
    uint64_t *p = malloc(n * sizeof(uint64_t));
    for (size_t i = 0; i < n; i++) p[i] = i;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = xs(&seed) % (i + 1);
        uint64_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
    return p;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    size_t bytes = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : (32u << 20);
    size_t n = bytes / 8;
    volatile double fsink;

    if (!strcmp(mode, "seq")) {
        double *p = aligned_alloc(64, n * 8);
        for (size_t i = 0; i < n; i++) p[i] = 1.0;
        double warm = seq_sum(p, n / 8); fsink = warm;
        /* 读三遍取总和，摊掉启动开销 */
        double best = 1e30;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            double s = seq_sum(p, n);
            double t1 = now_ns();
            fsink = s;
            if (t1 - t0 < best) best = t1 - t0;
        }
        double gb = (double)bytes * 3 / best;   /* 3 遍 */
        printf("MODE=seq\nBYTES=%zu\nNS=%.0f\nGBPS=%.2f\nNS_PER_LINE=%.3f\n",
               bytes, best, gb, best / (double)(n / 8) * 3);
        free(p);
    } else {
        uint64_t *perm = make_perm(n, 88172645463325252ull);
        uint64_t *idx = malloc(n * sizeof(uint64_t));
        for (size_t i = 0; i < n; i++) idx[i] = perm[i];
        free(perm);
        long steps = (long)(n < 2000000 ? 2000000 : n);
        uint64_t w = rnd_chase(idx, 0, steps / 10); fsink = (double)w;
        uint64_t best_c = ~0ull; double best_ns = 1e30;
        for (int r = 0; r < 5; r++) {
            uint64_t c0 = rdtsc(); double t0 = now_ns();
            uint64_t s = rnd_chase(idx, (uint64_t)r * 7919 % n, steps);
            double t1 = now_ns(); uint64_t c1 = rdtsc();
            fsink = (double)s;
            if (c1 - c0 < best_c) { best_c = c1 - c0; best_ns = t1 - t0; }
        }
        printf("MODE=rnd\nBYTES=%zu\nSTEPS=%ld\nNS=%.0f\nCYCLES=%llu\nNS_PER_ACCESS=%.2f\nCYC_PER_ACCESS=%.2f\n",
               bytes, steps, best_ns, (unsigned long long)best_c,
               best_ns / steps, (double)best_c / steps);
        free(idx);
    }
    return 0;
}
