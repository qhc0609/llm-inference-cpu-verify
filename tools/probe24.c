/*
 * probe24.c —— 在没有 PSI、也没有可用 perf 的容器里，量两件事：
 *
 *   wait  <threads> <seconds> <wmb> <window_s>
 *        N 个线程做与 pow24 完全相同的 GEMV 负载，主线程每 window 秒
 *        读一次每个 worker 的 /proc/self/task/<tid>/schedstat，取
 *        run_delay（第 2 字段，单位 ns，即"在 runqueue 上等 CPU"的累计时间）。
 *        输出每个窗口的吞吐、平均等待占比、单线程最大等待占比、忙占比。
 *        —— 这是 PSI cpu.some 的逐线程版本：PSI 在 CPU 上核算的就是
 *           runnable 任务的排队时间，schedstat 记账的是同一事件。
 *
 *   tsc   <iters> <wmb>
 *        单线程跑 iters 个 token，每 token 前后读 TSC，输出
 *        cycles/token 与 ms/token。TSC 是恒定频率计数器，
 *        cycles/token 只反映"指令路径 × IPC"，与核心频率无关。
 *        用它可以把"变慢是因为降频"和"变慢是因为代码路径变差"分开。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/syscall.h>
#include <x86intrin.h>

static volatile float g_sink;

typedef struct {
    size_t n;
    int K;
    float *w, *x;
    volatile int *stop;
    volatile unsigned long long tokens;
    pid_t tid;
} arg_t;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static pid_t self_tid(void) { return (pid_t)syscall(SYS_gettid); }

/* 读 /proc/self/task/<tid>/schedstat → "runtime_ns run_delay_ns pcount" */
static int read_schedstat(pid_t tid, unsigned long long *run, unsigned long long *delay)
{
    char p[80];
    unsigned long long pc = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", (int)tid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int r = fscanf(f, "%llu %llu %llu", run, delay, &pc);
    fclose(f);
    return r == 3 ? 0 : -1;
}

static void *worker(void *p)
{
    arg_t *a = (arg_t *)p;
    const int mask = a->K - 1;
    a->tid = self_tid();
    while (!*a->stop) {
        float acc = 0.f;
        for (size_t i = 0; i < a->n; i++) acc += a->w[i] * a->x[i & mask];
        g_sink = acc;
        a->tokens++;
    }
    return NULL;
}

static void fill(arg_t *a, size_t n, int K, unsigned seed)
{
    a->n = n;
    a->K = K;
    a->w = aligned_alloc(64, n * sizeof(float));
    a->x = aligned_alloc(64, (K + 16) * sizeof(float));
    if (!a->w || !a->x) { fprintf(stderr, "alloc fail\n"); exit(1); }
    srand(seed);
    for (size_t i = 0; i < n; i++) a->w[i] = (float)((rand() % 200) - 100) * 0.001f;
    for (int i = 0; i < K + 16; i++) a->x[i] = (float)((i % 37) * 0.01);
    a->tokens = 0;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pctile(double *v, int n, double pct)
{
    if (n <= 0) return 0;
    qsort(v, n, sizeof(double), cmpd);
    int k = (int)(pct * (n - 1) + 0.5);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return v[k];
}

static int run_wait(int threads, double secs, int wmb, double win)
{
    size_t n = (size_t)wmb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    volatile int stop = 0;
    arg_t *args = calloc(threads, sizeof(arg_t));
    pthread_t *th = calloc(threads, sizeof(pthread_t));
    if (!args || !th) { fprintf(stderr, "alloc fail\n"); return 1; }
    for (int i = 0; i < threads; i++) {
        fill(&args[i], n, 8192, 1234 + i);
        args[i].stop = &stop;
    }
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &args[i]);
    struct timespec warm = { 0, 50000000 };
    nanosleep(&warm, NULL);           /* 等 worker 写好 tid */

    unsigned long long *p_run = calloc(threads, sizeof(unsigned long long));
    unsigned long long *p_dly = calloc(threads, sizeof(unsigned long long));
    int ok = 1;
    for (int i = 0; i < threads; i++)
        if (read_schedstat(args[i].tid, &p_run[i], &p_dly[i]) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "schedstat unreadable\n"); stop = 1; for (int i = 0; i < threads; i++) pthread_join(th[i], NULL); return 1; }

    printf("### wait threads=%d seconds=%.0f wmb=%d window=%.2f\n", threads, secs, wmb, win);
    printf("t_s,tokens_per_s,wait_pct_avg,wait_pct_max,busy_pct_avg,n_waiting\n");

    double t0 = now_s(), last_t = t0;
    unsigned long long last_tok = 0;
    int cap = (int)(secs / win) + 8;
    double *tps = malloc(cap * sizeof(double));
    double *wp = malloc(cap * sizeof(double));
    int cnt = 0;

    while (now_s() - t0 < secs) {
        struct timespec req = { (time_t)win, (long)((win - (time_t)win) * 1e9) };
        nanosleep(&req, NULL);
        double t = now_s(), dt = t - last_t;
        if (dt <= 0) continue;

        unsigned long long tok = 0, sr = 0, sd = 0, maxd = 0;
        int nw = 0;
        for (int i = 0; i < threads; i++) {
            tok += args[i].tokens;
            unsigned long long r, d;
            if (read_schedstat(args[i].tid, &r, &d) == 0) {
                unsigned long long dr = r - p_run[i], dd = d - p_dly[i];
                p_run[i] = r; p_dly[i] = d;
                sr += dr; sd += dd;
                if (dd > maxd) maxd = dd;
                if (dd > 1000000ULL) nw++;      /* 该窗口内等待超过 1 ms */
            }
        }
        double dtok = (double)(tok - last_tok);
        double tpsv = dtok / dt;
        double wpv = (double)sd / (threads * dt * 1e9) * 100.0;
        double wmax = (double)maxd / (dt * 1e9) * 100.0;
        double bpv = (double)sr / (threads * dt * 1e9) * 100.0;

        printf("%.1f,%.1f,%.2f,%.2f,%.2f,%d\n", t - t0, tpsv, wpv, wmax, bpv, nw);
        fflush(stdout);
        if (cnt < cap) { tps[cnt] = tpsv; wp[cnt] = wpv; cnt++; }

        last_t = t; last_tok = tok;
    }

    stop = 1;
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);

    printf("### summary threads=%d tps_p50=%.1f wait_pct_p50=%.2f wait_pct_p90=%.2f\n",
           threads, pctile(tps, cnt, 0.5), pctile(wp, cnt, 0.5), pctile(wp, cnt, 0.9));

    for (int i = 0; i < threads; i++) { free(args[i].w); free(args[i].x); }
    free(args); free(th); free(p_run); free(p_dly); free(tps); free(wp);
    return 0;
}

static double tsc_ghz(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    unsigned long long t0 = __rdtsc();
    struct timespec s = { 0, 200000000 };
    nanosleep(&s, NULL);
    unsigned long long t1 = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    return (double)(t1 - t0) / dt / 1e9;
}

static int run_tsc(int iters, int wmb)
{
    size_t n = (size_t)wmb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    arg_t a;
    fill(&a, n, 8192, 1234);
    const int mask = a.K - 1;

    double ghz = tsc_ghz();
    printf("### tsc iters=%d wmb=%d tsc_ghz=%.4f\n", iters, wmb, ghz);
    printf("i,cycles,ms\n");

    int cap = iters > 0 ? iters : 1;
    double *cyc = malloc(cap * sizeof(double));
    double *ms = malloc(cap * sizeof(double));
    int cnt = 0;
    for (int it = 0; it < iters; it++) {
        struct timespec ta, tb;
        clock_gettime(CLOCK_MONOTONIC, &ta);
        unsigned long long c0 = __rdtsc();
        float acc = 0.f;
        for (size_t i = 0; i < n; i++) acc += a.w[i] * a.x[i & mask];
        g_sink = acc;
        unsigned long long c1 = __rdtsc();
        clock_gettime(CLOCK_MONOTONIC, &tb);
        double d = (tb.tv_sec - ta.tv_sec) + (tb.tv_nsec - ta.tv_nsec) / 1e9;
        printf("%d,%llu,%.4f\n", it, c1 - c0, d * 1e3);
        if (cnt < cap) { cyc[cnt] = (double)(c1 - c0); ms[cnt] = d * 1e3; cnt++; }
    }
    printf("### summary cycles_p10=%.0f cycles_p50=%.0f ms_p50=%.4f ghz=%.4f\n",
           pctile(cyc, cnt, 0.1), pctile(cyc, cnt, 0.5), pctile(ms, cnt, 0.5), ghz);
    free(a.w); free(a.x); free(cyc); free(ms);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s wait <threads> <seconds> <wmb> <window> | tsc <iters> <wmb>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "wait") && argc >= 6)
        return run_wait(atoi(argv[2]), atof(argv[3]), atoi(argv[4]), atof(argv[5]));
    if (!strcmp(argv[1], "tsc") && argc >= 4)
        return run_tsc(atoi(argv[2]), atoi(argv[3]));
    fprintf(stderr, "bad args\n");
    return 2;
}
