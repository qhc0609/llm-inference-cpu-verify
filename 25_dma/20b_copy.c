/* 20b_copy.c - what it really costs the CPU to move bytes.
 *
 * Sections 1-3 of the article need one number: bytes per second that a core
 * can push, and the CPU time it burns doing it. memcpy is the "no DMA" case:
 * every byte passes through a register, the store buffer and the cache.     */
#include "common.h"
#include <pthread.h>

#define TOTAL_MB 256
#define CHUNK    (8u << 20)

typedef struct { uint8_t *src, *dst; size_t lo, hi; int op; int cpu; uint64_t ns; } job_t;

/* op: 0 memcpy, 1 read-only, 2 write-only */
static void *worker(void *arg) {
    job_t *j = arg;
    pin_cpu(j->cpu);
    uint64_t t0 = now_ns();
    for (size_t off = j->lo; off < j->hi; off += CHUNK) {
        size_t n = (j->hi - off < CHUNK) ? (j->hi - off) : CHUNK;
        if (j->op == 0)      memcpy(j->dst + off, j->src + off, n);
        else if (j->op == 1) {
            uint64_t s = 0;
            const uint64_t *p = (const uint64_t *)(j->src + off);
            for (size_t k = 0; k < n / 8; k += 8) s += p[k];
            g_sink += s;
        } else memset(j->dst + off, 0x5a, n);
    }
    j->ns = now_ns() - t0;
    return NULL;
}

static double run_op(int op, int nt, int reps) {
    size_t n = (size_t)TOTAL_MB << 20;
    uint8_t *a = xalloc(n), *b = xalloc(n);
    double best = 0;
    for (int r = 0; r < reps; r++) {
        pthread_t th[8]; job_t j[8];
        size_t per = n / nt;
        uint64_t t0 = now_ns();
        for (int k = 0; k < nt; k++) {
            j[k] = (job_t){a, b, (size_t)k * per, (k == nt - 1) ? n : (size_t)(k + 1) * per, op, k, 0};
            pthread_create(&th[k], NULL, worker, &j[k]);
        }
        for (int k = 0; k < nt; k++) pthread_join(th[k], NULL);
        double dt = (double)(now_ns() - t0) / 1e9;
        double gbps = (double)n / dt / 1e9;
        if (gbps > best) best = gbps;
    }
    free(a); free(b);
    return best;
}

int main(int argc, char **argv) {
    int cpu = (argc > 1) ? atoi(argv[1]) : 0;
    pin_cpu(cpu);
    int reps = 3;
    /* warm the pages once so the first-touch faults are not charged to memcpy */
    { size_t n = (size_t)TOTAL_MB << 20; uint8_t *t = xalloc(n); memset(t, 3, n); free(t); }

    double m1 = run_op(0, 1, reps), m2 = run_op(0, 2, reps), m4 = run_op(0, 4, reps);
    double r1 = run_op(1, 1, reps), r4 = run_op(1, 4, reps);
    double w1 = run_op(2, 1, reps), w4 = run_op(2, 4, reps);

    RES("copy|memcpy|1|%.2f", m1);
    RES("copy|memcpy|2|%.2f", m2);
    RES("copy|memcpy|4|%.2f", m4);
    RES("copy|read|1|%.2f", r1);
    RES("copy|read|4|%.2f", r4);
    RES("copy|write|1|%.2f", w1);
    RES("copy|write|4|%.2f", w4);

    /* CPU cost per byte: at ~3 GHz one core, ns/byte = 3 / GBps for memcpy.
     * print it so the article does not have to guess the clock.             */
    RES("copy|ns_per_byte_memcpy1|%.3f", 1e9 / (m1 * 1e9));
    RES("copy|bytes_per_ns_memcpy1|%.3f", m1);
    return 0;
}
