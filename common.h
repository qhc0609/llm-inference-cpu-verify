/* common.h —— 全系列验证程序共用的计时与输出工具 */
#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* 单调时钟，纳秒。CLOCK_MONOTONIC 不受系统时间调整影响 */
static inline double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* 统一的结果输出格式：RES|key=value|unit
 * 好处是后面用 grep 就能把所有结果抓成表格，不用解析人话 */
#define RES(...) do { printf("RES|"); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static inline void *xmalloc(size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 64, n ? n : 64) != 0 || !p) {
        fprintf(stderr, "alloc failed: %zu bytes\n", n);
        exit(1);
    }
    memset(p, 0, n);
    return p;
}

/* 固定种子的伪随机，保证任何人重跑结果一致 */
static inline void seed_rng(uint64_t s) { srand48((long)s); }
static inline float frand(void) { return (float)drand48(); }

/* 已用内存（KB），从 /proc/self/status 的 VmRSS 读 */
static inline long vm_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; }
    }
    fclose(f);
    return kb;
}

#endif
