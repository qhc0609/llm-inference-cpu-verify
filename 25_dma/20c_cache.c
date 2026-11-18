/* 20c_cache.c - the price of a cache maintenance operation.
 *
 * On a non-coherent link (ARM + a device that is not in the coherency domain)
 * the CPU must flush dirty lines before handing a buffer over, and invalidate
 * them before reading what the device wrote. x86 has no invalidate and a
 * coherent fabric, but it does have CLFLUSH, so the *cost per line* of the
 * maintenance itself is measurable right here.                              */
#include "common.h"
#include <immintrin.h>

#define MB 64
#define LINE 64

static void flush_all(uint8_t *p, size_t n, int opt) {
    if (opt) {
        for (size_t i = 0; i < n; i += LINE) _mm_clflushopt(p + i);
        _mm_sfence();
    } else {
        for (size_t i = 0; i < n; i += LINE) _mm_clflush(p + i);
    }
}

static double read_bw(uint8_t *p, size_t n) {
    uint64_t t0 = now_ns(), s = 0;
    for (size_t i = 0; i < n; i += LINE) s += p[i];
    uint64_t t1 = now_ns();
    g_sink += s;
    return (double)n / ((double)(t1 - t0) / 1e9) / 1e9;
}

int main(int argc, char **argv) {
    pin_cpu(argc > 1 ? atoi(argv[1]) : 0);
    size_t n = (size_t)MB << 20;
    uint8_t *p = xalloc(n);
    memset(p, 0x11, n);

    /* --- 1. flush a dirty 64 MiB buffer, byte-accurate timing ------------ */
    uint64_t v[3];
    for (int r = 0; r < 3; r++) {
        memset(p, r, n);                       /* make every line dirty */
        uint64_t t0 = now_ns();
        flush_all(p, n, 0);
        v[r] = now_ns() - t0;
    }
    uint64_t t = med3(v[0], v[1], v[2]);
    double ns_line = (double)t / (double)(n / LINE);
    RES("cache|clflush|%d|%.1f|%.2f|%.1f", MB, ns_line, (double)n / ((double)t / 1e9) / 1e9,
        (double)t / 1e6);

    uint64_t v2[3];
    for (int r = 0; r < 3; r++) {
        memset(p, r, n);
        uint64_t t0 = now_ns();
        flush_all(p, n, 1);
        v2[r] = now_ns() - t0;
    }
    uint64_t t2 = med3(v2[0], v2[1], v2[2]);
    RES("cache|clflushopt|%d|%.1f|%.2f|%.1f", MB, (double)t2 / (double)(n / LINE),
        (double)n / ((double)t2 / 1e9) / 1e9, (double)t2 / 1e6);

    /* --- 2. clear the same volume with a plain store pass ---------------- */
    uint64_t v3[3];
    for (int r = 0; r < 3; r++) {
        uint64_t t0 = now_ns();
        memset(p, 0x22, n);
        v3[r] = now_ns() - t0;
    }
    uint64_t t3 = med3(v3[0], v3[1], v3[2]);
    RES("cache|memset_same_volume|%d|%.2f", MB, (double)n / ((double)t3 / 1e9) / 1e9);

    /* --- 3. is the line actually gone? read after flush vs read warm ----- */
    double warm = 0, cold = 0;
    for (int r = 0; r < 3; r++) {
        double w = read_bw(p, n);
        if (w > warm) warm = w;
        flush_all(p, n, 0);
        uint64_t t0 = now_ns(), s = 0;
        for (size_t i = 0; i < n; i += LINE) s += p[i];
        uint64_t t1 = now_ns(); g_sink += s;
        double c = (double)n / ((double)(t1 - t0) / 1e9) / 1e9;
        if (c > cold) cold = c;
    }
    RES("cache|read_warm|%d|%.2f", MB, warm);
    RES("cache|read_after_flush|%d|%.2f", MB, cold);

    /* --- 3b. the same test at a size that actually fits in L3 ------------- */
    /* 64 MiB never fits in a 6 MiB L3, so "warm" and "flushed" look identical
     * above. The maintenance cost only shows up on data that was resident.    */
    {
        size_t sn = (size_t)4 << 20;
        uint8_t *q = xalloc(sn);
        double w = 0, c = 0;
        for (int r = 0; r < 5; r++) {
            double a = read_bw(q, sn);
            if (a > w) w = a;
            flush_all(q, sn, 0);
            uint64_t t0 = now_ns(), s2 = 0;
            for (size_t i = 0; i < sn; i += LINE) s2 += q[i];
            uint64_t t1 = now_ns(); g_sink += s2;
            double b = (double)sn / ((double)(t1 - t0) / 1e9) / 1e9;
            if (b > c) c = b;
        }
        RES("cache|small_warm|4|%.2f", w);
        RES("cache|small_after_flush|4|%.2f", c);
        free(q);
    }

    /* --- 4. per-line round trip: flush one line, then read it ------------- */
    /* A dependent chain over flushed lines measures the refill from DRAM,
     * i.e. what an invalidate costs the consumer on the other side.        */
    {
        size_t m = 4096;
        uint8_t *q = xalloc(m * LINE);
        for (size_t i = 0; i < m * LINE; i += LINE) q[i] = 1;
        uint64_t v4[5];
        for (int r = 0; r < 5; r++) {
            uint64_t t0 = now_ns(); uint64_t s = 0;
            for (size_t i = 0; i < m; i++) {
                _mm_clflush(q + i * LINE);
                s += q[i * LINE];
            }
            uint64_t t1 = now_ns(); g_sink += s;
            v4[r] = t1 - t0;
        }
        uint64_t best = v4[0];
        for (int r = 1; r < 5; r++) if (v4[r] < best) best = v4[r];
        RES("cache|flush_read_roundtrip_ns|%.1f", (double)best / (double)m);
        free(q);
    }
    free(p);
    return 0;
}
