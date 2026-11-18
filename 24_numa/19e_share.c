/* 19e_share.c - two threads writing the same thing, on purpose.
 *
 * packed : the two counters live in the same 64 B cache line
 * padded : the counters are 128 B apart, one line each
 *
 * Run it twice, once with two cpus on the same node and once with one cpu from
 * each node, to see which of "same line" and "same node" actually costs money.
 *
 * Usage: ./19e_share <cpuA> <cpuB>
 */
#include "common.h"
#include "numa_util.h"
#include <pthread.h>

#define ITER (20000000L)

typedef struct { volatile long *p; int cpu; } wk_t;

static void *bump(void *arg)
{
    wk_t *w = (wk_t *)arg;
    pin_cpu(w->cpu);
    volatile long *p = w->p;
    for (long i = 0; i < ITER; i++) *p += 1;
    return NULL;
}

static double run(int ca, int cb, int padded, const char *tag)
{
    static long mem[64] __attribute__((aligned(128)));
    mem[0] = 0;
    mem[padded ? 16 : 1] = 0;     /* 16 longs = 128 B away */
    wk_t a = {&mem[0], ca}, b = {&mem[padded ? 16 : 1], cb};

    pthread_t ta, tb;
    uint64_t t0 = now_ns();
    pthread_create(&ta, NULL, bump, &a);
    pthread_create(&tb, NULL, bump, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    uint64_t t1 = now_ns();

    keep((uint64_t)mem[0] + (uint64_t)mem[padded ? 16 : 1]);
    double ns = (double)(t1 - t0) / (double)(ITER * 2);
    printf("RES|share|%s|%.2f\n", tag, ns);
    return ns;
}

int main(int argc, char **argv)
{
    int ca = (argc > 1) ? atoi(argv[1]) : 0;
    int cb = (argc > 2) ? atoi(argv[2]) : 1;
    run(ca, cb, 0, "packed");
    run(ca, cb, 1, "padded");
    return 0;
}
