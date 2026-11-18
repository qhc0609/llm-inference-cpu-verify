/* common.h - shared helpers for the DMA / shared-memory lab */
#ifndef COMMON_H
#define COMMON_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline void pin_cpu(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof s, &s);
}

static volatile uint64_t g_sink;

static inline uint64_t med3(uint64_t a, uint64_t b, uint64_t c) {
    if (a > b) { uint64_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

static inline void *xalloc(size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 64, n)) { perror("posix_memalign"); exit(1); }
    memset(p, 1, n);
    return p;
}

#define RES(...) do { printf("RES|" __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#endif
