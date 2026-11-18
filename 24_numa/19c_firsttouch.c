/* 19c_firsttouch.c - who touches a page first decides where it lives.
 *
 * 256 MiB split into one quarter per worker, one worker per cpu, cpus spread
 * over the nodes. Four placements are compared:
 *
 *   own     each worker touches its own quarter, then reads its own quarter
 *   cross   each worker touches its own quarter, then reads a quarter that
 *           belongs to a worker on the other node
 *   single  one thread touches every page, then all workers read their quarter
 *   cold    nobody touches; the read itself pays the first fault
 *
 * After every touch phase we read /proc/self/numa_maps, so "where the pages
 * actually landed" is measured, not assumed.
 */
#include "common.h"
#include "numa_util.h"
#include <pthread.h>
#include <sys/mman.h>

#define TOTAL (256u << 20)
#define NW 4

typedef struct {
    uint64_t *base;
    size_t lo, hi;      /* element range of this worker's own quarter */
    size_t rlo, rhi;    /* element range this worker reads this phase */
    int cpu, node;
    uint64_t sum;
    uint64_t t0[5], t1[5];  /* absolute start/end of every read round */
    int phase;
} wk_t;

static pthread_barrier_t bar;
static int g_cpu[16], g_node[16], g_nw;

static void *worker(void *p)
{
    wk_t *w = (wk_t *)p;
    pin_cpu(w->cpu);

    if (w->phase == 0 || w->phase == 1) { /* 2 = single toucher, 3 = cold: none */
        for (size_t i = w->lo; i < w->hi; i++) w->base[i] = (uint64_t)i;
    }
    pthread_barrier_wait(&bar);
    pthread_barrier_wait(&bar);

    uint64_t best[5];
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = now_ns(), s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        w->t0[r] = t0;
        size_t i = w->rlo;
        for (; i + 4 <= w->rhi; i += 4) {
            s0 += w->base[i]; s1 += w->base[i + 1];
            s2 += w->base[i + 2]; s3 += w->base[i + 3];
        }
        for (; i < w->rhi; i++) s0 += w->base[i];
        uint64_t t1 = now_ns();
        w->t1[r] = t1;
        w->sum += s0 + s1 + s2 + s3;
        best[r] = t1 - t0;
    }
    keep(w->sum);
    /* fastest lap per worker; the slow ones are host interference, not NUMA */
    uint64_t b = best[0];
    for (int r = 1; r < 5; r++) if (best[r] < b) b = best[r];
    w->sum = b;
    return NULL;
}

static double run_phase(int phase, const char *tag)
{
    uint64_t *buf = mmap(NULL, TOTAL, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); exit(1); }
    size_t n = TOTAL / sizeof(uint64_t), per = n / NW;

    pthread_barrier_init(&bar, NULL, g_nw + 1);
    pthread_t th[16];
    wk_t w[16];
    /* pass 1: every worker's own range must be known before pass 2 reads a
     * neighbour's range, otherwise w[j].lo is still uninitialised for j > k */
    for (int k = 0; k < g_nw; k++) {
        w[k].base = buf;
        w[k].lo = (size_t)k * per;
        w[k].hi = (k == g_nw - 1) ? n : (size_t)(k + 1) * per;
        w[k].cpu = g_cpu[k];
        w[k].node = g_node[k];
        w[k].sum = 0;
        w[k].phase = phase;
        w[k].rlo = w[k].lo;
        w[k].rhi = w[k].hi;
    }
    /* pass 2: cross means "read the quarter that lives on the other node" */
    if (phase == 1) {
        for (int k = 0; k < g_nw; k++) {
            int j = (k + g_nw / 2) % g_nw;
            w[k].rlo = w[j].lo;
            w[k].rhi = w[j].hi;
        }
    }
    for (int k = 0; k < g_nw; k++)
        pthread_create(&th[k], NULL, worker, &w[k]);
    if (phase == 2) { /* single: one thread touches everything first.
                       * Pin it, otherwise the toucher itself migrates and the
                       * pages scatter over both nodes -- which is a real trap,
                       * but here we want the controlled version: all of it on
                       * one node, then read by workers on both. */
        pin_cpu(g_cpu[0]);
        for (size_t i = 0; i < n; i++) buf[i] = (uint64_t)i;
    }
    pthread_barrier_wait(&bar);   /* workers have (or have not) touched */
    pthread_barrier_wait(&bar);

    int pages[8];
    numa_pages(pages, 8);
    printf("RES|ft|%s|pages|%d|%d\n", tag, pages[0], pages[1]);

    /* Aggregate bandwidth needs the wall clock of a round where all workers
     * really ran together. On a shared host a vCPU can be descheduled for tens
     * of milliseconds, so the fastest of the five rounds is the honest one --
     * a slow round measures the hypervisor, not the memory. */
    for (int k = 0; k < g_nw; k++) { pthread_join(th[k], NULL); keep(w[k].sum); }
    uint64_t tmax = UINT64_MAX;
    for (int r = 0; r < 5; r++) {
        uint64_t first = UINT64_MAX, last = 0;
        for (int k = 0; k < g_nw; k++) {
            if (w[k].t0[r] < first) first = w[k].t0[r];
            if (w[k].t1[r] > last)  last  = w[k].t1[r];
        }
        if (last - first < tmax) tmax = last - first;
    }
    pthread_barrier_destroy(&bar);

    /* every worker read one quarter, in parallel: total bytes = TOTAL */
    double gbps = (double)TOTAL / (double)tmax;
    printf("RES|ft|%s|gbps|%.2f\n", tag, gbps);
    munmap(buf, TOTAL);
    return gbps;
}

int main(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) { perror("affinity"); return 1; }

    int nn = node_count();
    int cpu2node[64];
    for (int i = 0; i < 64; i++) cpu2node[i] = -1;
    for (int nd = 0; nd < nn; nd++) {
        int c[64], m = node_cpus(nd, c, 64);
        for (int i = 0; i < m; i++) cpu2node[c[i]] = nd;
    }

    g_nw = 0;
    for (int c = 0; c < 64 && g_nw < NW; c++) {
        if (!CPU_ISSET(c, &set)) continue;
        g_cpu[g_nw] = c;
        g_node[g_nw] = cpu2node[c];
        g_nw++;
    }
    if (g_nw < 2) { fprintf(stderr, "need >=2 usable cpus\n"); return 1; }

    printf("RES|ft|setup|nodes|%d|workers|%d\n", nn, g_nw);
    for (int k = 0; k < g_nw; k++)
        printf("RES|ft|worker|%d|cpu|%d|node|%d\n", k, g_cpu[k], g_node[k]);

    run_phase(0, "own");
    run_phase(1, "cross");
    run_phase(2, "single");
    run_phase(3, "untouched");
    return 0;
}
