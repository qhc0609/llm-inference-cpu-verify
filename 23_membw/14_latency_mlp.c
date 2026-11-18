/* 14_latency_mlp.c - latency is not bandwidth.
 *
 * One dependent pointer chain can only have one miss in flight, so its
 * throughput is 64 B / latency (~0.5 GB/s) no matter how fast the DRAM is.
 * Run N independent chains at once and the misses overlap: per-chain latency
 * stays the same, aggregate bandwidth climbs until the bus is full.
 *
 * 128 MiB footprint, random permutation, N disjoint cycles.
 */
#include "common.h"

#define M (32u << 20) /* 32M ints = 128 MiB */
#define MAXCH 32

static int *nxt;
static int *ord;
static int p[MAXCH];

static uint32_t rs = 12345u;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

static void build(int nch)
{
    for (int i = 0; i < M; i++) ord[i] = i;
    for (int i = M - 1; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        int t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }
    /* chain c walks positions c, c+nch, c+2*nch, ... and loops back */
    int len = M / nch;
    for (int c = 0; c < nch; c++)
        for (int k = 0; k < len; k++) {
            int from = ord[c + k * nch];
            int to = ord[c + ((k + 1) % len) * nch];
            nxt[from] = to;
        }
}

static void run(int nch, long iters)
{
    uint64_t v[3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < nch; c++) p[c] = ord[c];
        uint64_t sum = 0;
        uint64_t t0 = now_ns();
        for (long it = 0; it < iters; it++)
            for (int c = 0; c < nch; c++) {
                int v2 = p[c];
                v2 = nxt[v2];
                p[c] = v2;
                sum += (uint64_t)v2;
            }
        uint64_t t1 = now_ns();
        keep(sum);
        v[r] = t1 - t0;
    }
    uint64_t t = median3(v[0], v[1], v[2]);
    double lat = (double)t / (double)iters;                     /* ns per chain hop */
    double gbs = (double)nch * 64.0 * (double)iters / (double)t; /* B/ns = GB/s */
    printf("RES|mlp|%d|%.1f|%.3f\n", nch, lat, gbs);
}

int main(void)
{
    nxt = xalloc((size_t)M * sizeof(int));
    ord = xalloc((size_t)M * sizeof(int));
    /* warm-up: touch all pages */
    build(1);
    { uint64_t s = 0; for (int i = 0; i < M; i++) s += nxt[i]; keep(s); }

    for (int nch = 1; nch <= MAXCH; nch *= 2) {
        build(nch);
        long iters = 2000000;
        if (nch >= 16) iters = 1000000;
        run(nch, iters);
    }
    return 0;
}
