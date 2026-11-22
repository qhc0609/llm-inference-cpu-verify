/* pow24.c —— 第24篇《CPU功耗与温控优化》实验程序
 *
 * 背景：这台 VMware 虚拟机里没有 cpufreq、没有 thermal_zone、MSR 0x198 读出 0，
 *       perf 的 cycles/instructions 显示 <not supported>。频率、温度、瓦数都读不到。
 * 办法：改测「行为」。
 *       1) 固定工作量的 decode 步骤反复跑，记录每次 token 花了多少 CPU 时间。
 *          频率掉了 → 同样的活要更多 CPU 时间。cpu_us_per_token 就是 1/f 的代理。
 *       2) 同时读 /proc/stat 的 steal 与 PSI，把「宿主抢走的时间」和「自己变慢」分开。
 *       3) probe 模式跑一条依赖链，Mops/s 正比于有效频率，用来做恢复曲线。
 *
 * 用法：
 *   pow24 sustain <threads> <seconds> <wmb> <window_s>
 *   pow24 burst   <threads> <on_s> <off_s> <cycles> <wmb>
 *   pow24 probe   <seconds> <interval_s>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

static volatile float g_sink;

typedef struct {
    float  *w;
    size_t  n;
    float  *x;
    int     K;
    volatile int *stop;
    volatile unsigned long long tokens;
    double  *samp;              /* 每个 token 的 CPU 微秒 */
    unsigned long long cnt;
} arg_t;

#define SAMP_CAP 16384
#define RUSAGE_SELF_ 0

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double cpu_s(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
         + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* 当前线程的 CPU 时间（RUSAGE_THREAD）：比进程级更干净，能逐 token 归因 */
static double cpu_thread_s(void)
{
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
         + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* /proc/stat 第一行里的 steal 与总 jiffies */
static void stat_cpu(double *steal, double *total)
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[10] = {0};
    *steal = *total = 0;
    if (!f) return;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) >= 8) {
        for (int i = 0; i < 8; i++) *total += (double)v[i];
        *steal = (double)v[7];
    }
    fclose(f);
}

/* PSI：有就取 some avg10，没有返回 -1 */
static double psi_cpu(void)
{
    FILE *f = fopen("/proc/pressure/cpu", "r");
    char buf[256];
    double p = -1;
    if (!f) return -1;
    if (fgets(buf, sizeof buf, f)) {
        char *s = strstr(buf, "avg10=");
        if (s) p = atof(s + 6);
    }
    fclose(f);
    return p;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* 取数组里第 pct 百分位（pct 用 0.1 表示 10%） */
static double pctile(double *v, int n, double pct)
{
    if (n <= 0) return 0;
    qsort(v, n, sizeof(double), cmpd);
    int k = (int)(pct * (n - 1) + 0.5);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return v[k];
}

static void *worker(void *p)
{
    arg_t *a = (arg_t *)p;
    const int mask = a->K - 1;
    while (!*a->stop) {
        double c0 = cpu_thread_s();
        float acc = 0.f;
        for (size_t i = 0; i < a->n; i++)
            acc += a->w[i] * a->x[i & mask];
        g_sink = acc;            /* 防止整轮被优化掉 */
        if (a->samp) a->samp[a->cnt % SAMP_CAP] = (cpu_thread_s() - c0) * 1e6;
        a->tokens++;
        a->cnt++;
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

/* 依赖链：每次迭代 1 次乘加，延迟受限，耗时 ∝ 1/频率 */
static double probe_ops(double seconds)
{
    double a = 1.0, b = 1.000000001;
    double t0 = now_s();
    unsigned long long i = 0;
    while (now_s() - t0 < seconds) {
        for (int k = 0; k < 1000000; k++) { a = a * b + 1e-12; }
        i += 1000000ULL;
    }
    g_sink = (float)a;      /* 结果必须逃逸到 volatile，否则整条依赖链会被删掉 */
    return (double)i / (now_s() - t0) / 1e6;   /* Mops/s */
}

static int run_sustain(int threads, double seconds, int wmb, double win)
{
    size_t n = (size_t)wmb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    volatile int stop = 0;
    arg_t *args = calloc(threads, sizeof(arg_t));
    pthread_t *th = calloc(threads, sizeof(pthread_t));
    for (int i = 0; i < threads; i++) { fill(&args[i], n, 8192, 1234 + i); args[i].stop = &stop; }
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &args[i]);

    printf("### sustain threads=%d seconds=%.0f wmb=%d window=%.0f\n", threads, seconds, wmb, win);
    printf("t_s,win_s,tokens,tokens_per_s,cpu_us_per_token,cpu_pct,steal_pct,psi10\n");

    double t0 = now_s(), last_t = t0, last_cpu = cpu_s(), last_steal, last_total, st, tt;
    stat_cpu(&last_steal, &last_total);
    double last_tok = 0;

    while (now_s() - t0 < seconds) {
        struct timespec req = { (time_t)win, (long)((win - (long)win) * 1e9) };
        nanosleep(&req, NULL);

        double t = now_s();
        double c = cpu_s();
        unsigned long long tok = 0;
        for (int i = 0; i < threads; i++) tok += args[i].tokens;

        stat_cpu(&st, &tt);
        double dt = t - last_t, dc = c - last_cpu;
        double dtok = (double)(tok - last_tok);
        double dsteal = st - last_steal, dtotal = tt - last_total;
        printf("%.1f,%.1f,%.0f,%.1f,%.1f,%.1f,%.2f,%.2f\n",
               t - t0, dt, dtok, dtok / dt,
               dtok > 0 ? dc * 1e6 / dtok : 0.0,
               dt > 0 ? dc / dt / threads * 100.0 : 0.0,
               dtotal > 0 ? dsteal / dtotal * 100.0 : 0.0,
               psi_cpu());
        fflush(stdout);

        last_t = t; last_cpu = c; last_steal = st; last_total = tt; last_tok = (double)tok;
    }

    stop = 1;
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);

    /* 汇总：前 20% 与后 20% 的 cpu_us_per_token 之比 */
    printf("### done\n");
    free(args); free(th);
    return 0;
}

static int run_sustain2(int threads, double seconds, int wmb, double win)
{
    size_t n = (size_t)wmb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    volatile int stop = 0;
    arg_t *args = calloc(threads, sizeof(arg_t));
    pthread_t *th = calloc(threads, sizeof(pthread_t));
    for (int i = 0; i < threads; i++) {
        fill(&args[i], n, 8192, 1234 + i);
        args[i].stop = &stop;
        args[i].samp = malloc(sizeof(double) * SAMP_CAP);
        args[i].cnt = 0;
    }
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &args[i]);

    printf("### sustain2 threads=%d seconds=%.0f wmb=%d window=%.0f\n", threads, seconds, wmb, win);
    printf("t_s,tokens,tokens_per_s,p10_us,p50_us,psi10\n");

    double t0 = now_s();
    unsigned long long last_tok = 0, last_cnt[64] = {0};
    double *buf = malloc(sizeof(double) * SAMP_CAP);

    while (now_s() - t0 < seconds) {
        struct timespec req = { (time_t)win, (long)((win - (long)win) * 1e9) };
        nanosleep(&req, NULL);
        double t = now_s() - t0;

        unsigned long long tok = 0;
        int m = 0;
        for (int i = 0; i < threads; i++) {
            tok += args[i].tokens;
            unsigned long long c = args[i].cnt, lc = last_cnt[i];
            unsigned long long w = c - lc;
            if (w > SAMP_CAP) w = SAMP_CAP;
            for (unsigned long long k = c - w; k < c; k++) buf[m++] = args[i].samp[k % SAMP_CAP];
            last_cnt[i] = c;
        }
        double dtok = (double)(tok - last_tok);
        double p10 = pctile(buf, m, 0.10);
        double p50 = pctile(buf, m, 0.50);
        printf("%.1f,%.0f,%.1f,%.1f,%.1f,%.2f\n", t, dtok, dtok / win, p10, p50, psi_cpu());
        fflush(stdout);
        last_tok = tok;
    }
    stop = 1;
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    printf("### done\n");
    for (int i = 0; i < threads; i++) { free(args[i].w); free(args[i].x); free(args[i].samp); }
    free(buf); free(args); free(th);
    return 0;
}

static int run_burst(int threads, double on_s, double off_s, int cycles, int wmb)
{
    size_t n = (size_t)wmb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    printf("### burst threads=%d on=%.0f off=%.0f cycles=%d wmb=%d\n", threads, on_s, off_s, cycles, wmb);
    printf("cycle,on_s,tokens,tokens_per_s,cpu_us_per_token\n");

    for (int cyc = 1; cyc <= cycles; cyc++) {
        volatile int stop = 0;
        arg_t *args = calloc(threads, sizeof(arg_t));
        pthread_t *th = calloc(threads, sizeof(pthread_t));
        for (int i = 0; i < threads; i++) { fill(&args[i], n, 8192, 777 + i); args[i].stop = &stop; }
        double c0 = cpu_s(), t0 = now_s();
        for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &args[i]);
        struct timespec req = { (time_t)on_s, (long)((on_s - (long)on_s) * 1e9) };
        nanosleep(&req, NULL);
        stop = 1;
        for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
        double dt = now_s() - t0, dc = cpu_s() - c0;
        unsigned long long tok = 0;
        for (int i = 0; i < threads; i++) tok += args[i].tokens;
        printf("%d,%.1f,%.0f,%.1f,%.1f\n", cyc, dt, (double)tok, tok / dt,
               tok ? dc * 1e6 / tok : 0.0);
        fflush(stdout);
        for (int i = 0; i < threads; i++) { free(args[i].w); free(args[i].x); }
        free(args); free(th);
        if (cyc < cycles) {
            struct timespec off = { (time_t)off_s, 0 };
            nanosleep(&off, NULL);
        }
    }
    return 0;
}

static int run_probe(double seconds, double interval)
{
    printf("### probe seconds=%.0f interval=%.1f\n", seconds, interval);
    printf("t_s,mops_per_s\n");
    double t0 = now_s();
    while (now_s() - t0 < seconds) {
        double m = probe_ops(interval * 0.8);
        printf("%.1f,%.4f\n", now_s() - t0, m);
        fflush(stdout);
        struct timespec req = { 0, (long)(interval * 0.2 * 1e9) };
        nanosleep(&req, NULL);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s sustain|sustain2|burst|probe ...\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "sustain") && argc >= 6)
        return run_sustain(atoi(argv[2]), atof(argv[3]), atoi(argv[4]), atof(argv[5]));
    if (!strcmp(argv[1], "sustain2") && argc >= 6)
        return run_sustain2(atoi(argv[2]), atof(argv[3]), atoi(argv[4]), atof(argv[5]));
    if (!strcmp(argv[1], "burst") && argc >= 7)
        return run_burst(atoi(argv[2]), atof(argv[3]), atof(argv[4]), atoi(argv[5]), atoi(argv[6]));
    if (!strcmp(argv[1], "probe") && argc >= 4)
        return run_probe(atof(argv[2]), atof(argv[3]));
    fprintf(stderr, "bad args\n");
    return 2;
}
