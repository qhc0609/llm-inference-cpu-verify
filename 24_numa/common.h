/* common.h - shared helpers for the CPU cache / memory access microbenchmarks.
 *
 * All benchmarks follow one discipline:
 *   - one benchmark at a time, no competing load
 *   - one warm-up pass, then three timed passes -> median
 *   - every result accumulated into a volatile global so the loop cannot be removed
 */
#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* sink: keeps the compiler from deleting the measured work */
static volatile uint64_t g_sink;

static inline void keep(uint64_t v) { g_sink += v; }

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static inline uint64_t median3(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t v[3] = {a, b, c};
    qsort(v, 3, sizeof(uint64_t), cmp_u64);
    return v[1];
}

static inline void *xalloc(size_t bytes)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, bytes) != 0 || !p) {
        fprintf(stderr, "alloc failed: %zu bytes\n", bytes);
        exit(1);
    }
    memset(p, 0, bytes);
    return p;
}

/* fill an int array so the data is touched (page faults happen in warm-up) */
static inline void fill_ints(int *a, size_t n, int seed)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (int)((i * 2654435761u + (unsigned)seed) & 0x7fffffffu);
}

#define RES(key, fmt, ...) printf("RES|%s|" fmt "\n", key, __VA_ARGS__)

#endif /* COMMON_H */
