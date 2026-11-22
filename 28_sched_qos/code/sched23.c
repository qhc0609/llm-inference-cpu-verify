/*
 * sched23.c —— 第23篇《大模型CPU调度优化》实验套件
 *
 * 设计原则：
 *   1. 只依赖 glibc + pthread + futex，不引第三方库（不 apt install 任何东西）
 *   2. 只读 /proc，不改任何全局 sysctl
 *   3. 工作负载用固定总工作量，保证跨配置可比
 *   4. 所有等待都带超时上限，绝不把机器挂死
 *
 * 用法：
 *   sched23 pp   <iters> <nspin> <cpu> [same]    唤醒往返延迟（ns）
 *   sched23 decode <tokens> <mb>                 每 token 延迟 + 等 CPU 占比
 *   sched23 work <threads> <iters> [base_cpu]    固定总工作量吞吐（ms）
 *   sched23 spin <sec>                           纯 CPU 占用
 *   sched23 starve <sec>                         饥饿实验
 *
 * 环境变量：
 *   SCHED23_POLICY = other|batch|idle|fifo|rr   默认 other
 *   SCHED23_PRIO   = 1..99                      默认 50
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAXSPIN 16
#define PP_STOP 0x40000000

static int g_policy = SCHED_OTHER;
static int g_prio = 50;

/* 反优化屏障用的全局下沉点，必须在所有函数之前声明 */
static volatile double g_sink;
static double wmul[64];

static const char *pol_name(int p) {
    switch (p) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_BATCH: return "SCHED_BATCH";
    case SCHED_IDLE:  return "SCHED_IDLE";
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    default:          return "?";
    }
}

static int parse_policy(const char *s) {
    if (!s) return SCHED_OTHER;
    if (!strcmp(s, "batch")) return SCHED_BATCH;
    if (!strcmp(s, "idle"))  return SCHED_IDLE;
    if (!strcmp(s, "fifo"))  return SCHED_FIFO;
    if (!strcmp(s, "rr"))    return SCHED_RR;
    return SCHED_OTHER;
}

static void apply_policy(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    if (g_policy == SCHED_FIFO || g_policy == SCHED_RR) sp.sched_priority = g_prio;
    if (sched_setscheduler(0, g_policy, &sp) != 0)
        fprintf(stderr, "WARN sched_setscheduler(%s,%d): %s\n",
                pol_name(g_policy), g_prio, strerror(errno));
}

static void pin_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        fprintf(stderr, "WARN setaffinity(%d): %s\n", cpu, strerror(errno));
}

/* 本线程累计获得的 CPU 时间（不含被换出/等配额的时间），单位 ms */
static long long thread_cpu_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void read_schedstat(long long *exec_ns, long long *wait_ns, long long *slices) {
    *exec_ns = *wait_ns = *slices = -1;
    FILE *f = fopen("/proc/self/schedstat", "r");
    if (!f) return;
    if (fscanf(f, "%lld %lld %lld", exec_ns, wait_ns, slices) != 3) {
        *exec_ns = *wait_ns = *slices = -1;
    }
    fclose(f);
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

static long long pct(long long *v, int n, double p) {
    if (n <= 0) return -1;
    int idx = (int)((p / 100.0) * n + 0.999999) - 1;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return v[idx];
}

/* ---------------- futex 封装：带 200ms 超时，避免死等 ---------------- */

static long futex_wait_to(atomic_int *p, int v) {
    struct timespec ts = {0, 200000000};
    return syscall(SYS_futex, (int *)p, FUTEX_WAIT_PRIVATE, v, &ts, NULL, 0);
}
static long futex_wake_(atomic_int *p, int n) {
    return syscall(SYS_futex, (int *)p, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0);
}

/* ---------------- 模式 1：ping-pong 唤醒往返 ---------------- */

static atomic_int tok_a, tok_b;
static atomic_int stop_spin;
static int pp_iters;

static void *pp_responder(void *arg) {
    int cpu = (int)(long)arg;
    pin_cpu(cpu);
    apply_policy();
    int last = 0;
    for (;;) {
        int v;
        int guard = 0;
        while ((v = atomic_load(&tok_a)) == last) {
            futex_wait_to(&tok_a, v);
            if (++guard > 500000) { fprintf(stderr, "responder timeout\n"); return NULL; }
        }
        last = v;
        atomic_store(&tok_b, v);
        futex_wake_(&tok_b, 1);
        if (v == PP_STOP) break;
    }
    return NULL;
}

/* A 端：把 seq 发给 B，等 B 原样回传 */
static void exchange(int seq) {
    int v;
    atomic_store(&tok_a, seq);
    futex_wake_(&tok_a, 1);
    while ((v = atomic_load(&tok_b)) != seq) futex_wait_to(&tok_b, v);
}

static void *spinner(void *arg) {
    int cpu = (int)(long)arg;
    pin_cpu(cpu);
    volatile unsigned long x = 0;
    while (!atomic_load(&stop_spin)) {
        for (int i = 0; i < 4096; i++) x += i;
    }
    return NULL;
}

static int mode_pp(int iters, int nspin, int cpu, int same) {
    pp_iters = iters;
    atomic_store(&tok_a, 0);
    atomic_store(&tok_b, 0);
    atomic_store(&stop_spin, 0);

    pthread_t sp[MAXSPIN];
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < nspin; i++) {
        int c = same ? cpu : (cpu + 1 + i) % ncpu;
        pthread_create(&sp[i], NULL, spinner, (void *)(long)c);
    }

    pthread_t th;
    pthread_create(&th, NULL, pp_responder, (void *)(long)cpu);

    pin_cpu(cpu);
    apply_policy();

    for (int i = 1; i <= 200; i++) exchange(i); /* 预热 */

    long long *s = malloc(sizeof(long long) * iters);
    for (int i = 1; i <= iters; i++) {
        long long t0 = now_ns();
        exchange(1000 + i);
        s[i - 1] = now_ns() - t0;
    }
    exchange(PP_STOP);
    pthread_join(th, NULL);
    atomic_store(&stop_spin, 1);
    for (int i = 0; i < nspin; i++) pthread_join(sp[i], NULL);

    qsort(s, iters, sizeof(long long), cmp_ll);
    printf("PP policy=%s prio=%d spin=%d same=%d cpu=%d n=%d "
           "p50=%lld p99=%lld p999=%lld max=%lld unit=ns\n",
           pol_name(g_policy), g_prio, nspin, same, cpu, iters,
           pct(s, iters, 50), pct(s, iters, 99), pct(s, iters, 99.9),
           s[iters - 1]);
    free(s);
    return 0;
}

/* ---------------- 模式 2：模拟 decode ---------------- */

static float *W;
static size_t WN;

static double decode_step(void) {
    double acc = 0.0;
    const float *p = W;
    for (size_t i = 0; i < WN; i++) acc += (double)p[i];
    return acc;
}

static int mode_decode(int tokens, int mb) {
    WN = (size_t)mb * 1024 * 1024 / sizeof(float);
    W = aligned_alloc(64, WN * sizeof(float));
    if (!W) { perror("alloc"); return 1; }
    for (size_t i = 0; i < WN; i++) W[i] = (float)(i % 251) * 0.001f;

    double sink = 0;
    for (int i = 0; i < 3; i++) sink += decode_step();
    __asm__ __volatile__("" : "+x"(sink) : : "memory");

    long long *lat = malloc(sizeof(long long) * tokens);
    long long ex0, wt0, sl0, ex1, wt1, sl1;
    read_schedstat(&ex0, &wt0, &sl0);
    for (int i = 0; i < tokens; i++) {
        long long t0 = now_ns();
        sink += decode_step();
        lat[i] = now_ns() - t0;
    }
    read_schedstat(&ex1, &wt1, &sl1);

    long long total = 0;
    for (int i = 0; i < tokens; i++) total += lat[i];
    qsort(lat, tokens, sizeof(long long), cmp_ll);

    double ratio = (wt0 >= 0 && wt1 > wt0)
        ? 100.0 * (double)(wt1 - wt0) / (double)((wt1 - wt0) + (ex1 - ex0)) : 0.0;

    printf("DEC policy=%s tokens=%d wmb=%d p50=%lld p99=%lld p999=%lld max=%lld "
           "mean=%.1f total_ms=%.1f runq_wait_ms=%.1f exec_ms=%.1f wait_pct=%.2f slices=%lld unit=ns sink=%.1f\n",
           pol_name(g_policy), tokens, mb,
           pct(lat, tokens, 50), pct(lat, tokens, 99), pct(lat, tokens, 99.9),
           lat[tokens - 1],
           (double)total / tokens, (double)total / 1e6,
           (wt0 >= 0) ? (double)(wt1 - wt0) / 1e6 : -1.0,
           (ex0 >= 0) ? (double)(ex1 - ex0) / 1e6 : -1.0,
           ratio, (sl0 >= 0) ? (sl1 - sl0) : -1, sink);
    g_sink = sink;
    free(lat);
    free(W);
    return 0;
}

/* ---------------- 模式 3：固定总工作量 ---------------- */

#define ARR 8192

static double *small;
static long g_iters;

static atomic_int g_next;
static int g_base_cpu;

static void *work_thread(void *arg) {
    (void)arg;
    if (g_base_cpu >= 0) {
        int idx = atomic_fetch_add(&g_next, 1);
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        pin_cpu(g_base_cpu + idx % ncpu);
    }
    apply_policy();
    double acc = 0.0;
    for (long k = 0; k < g_iters; k++) {
        const double *p = small;
        /* m 随 k 变化：如果不这样做，内层循环对 k 是循环不变的，
         * GCC 会把它整段提到外层，于是耗时与迭代数无关（我们就踩过这个坑） */
        const double m = wmul[k & 63];
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < ARR; i += 4) {
            s0 += p[i] * m; s1 += p[i + 1] * m; s2 += p[i + 2] * m; s3 += p[i + 3] * m;
        }
        acc += s0 + s1 + s2 + s3;
        /* 每轮都做一次 volatile 写：这是唯一能保证 GCC 不把整个循环
         * 折成一次乘法的办法（可观察副作用强制每轮执行） */
        g_sink = acc;
    }
    /* 反 DCE 屏障：没有这一句，-O2 会把整个累加循环删掉，
     * 于是量到的只是 pthread_create 的开销 */
    __asm__ __volatile__("" : "+x"(acc) : : "memory");
    g_sink = acc;
    return NULL;
}

static int mode_work(int threads, long iters, int base_cpu) {
    small = aligned_alloc(64, ARR * sizeof(double));
    for (int i = 0; i < ARR; i++) small[i] = (double)(i % 97) * 0.01;
    for (int i = 0; i < 64; i++) wmul[i] = 1.0 + i * 1e-6;
    g_iters = iters;
    g_base_cpu = base_cpu;
    atomic_store(&g_next, 0);

    pthread_t th[64];
    long long t0 = now_ns();
    for (long i = 0; i < threads; i++) pthread_create(&th[i], NULL, work_thread, (void *)i);
    for (long i = 0; i < threads; i++) pthread_join(th[i], NULL);
    long long dt = now_ns() - t0;

    printf("WORK policy=%s threads=%d iters_per=%ld total_iters=%ld wall_ms=%.3f unit=ms\n",
           pol_name(g_policy), threads, iters, iters * threads, (double)dt / 1e6);
    free(small);
    return 0;
}

/* ---------------- 模式 4：纯自旋 ---------------- */

static int mode_spin(int sec) {
    apply_policy();
    long long end = now_ns() + (long long)sec * 1000000000LL;
    volatile unsigned long x = 0;
    while (now_ns() < end) {
        for (int i = 0; i < 8192; i++) x += i;
    }
    return 0;
}

/* ---------------- 模式 4b：自报 CPU 时间 ---------------- */

/* 一个独立的进程跑固定墙钟时长，用 getrusage 报自己真正拿到多少 CPU。
   与线程版相比，进程口径不会被线程组的统计口径污染。 */
static int mode_cpuprobe(int sec) {
    apply_policy();
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    long long t0 = now_ns();
    long long end = t0 + (long long)sec * 1000000000LL;
    volatile unsigned long x = 0;
    while (now_ns() < end) {
        for (int i = 0; i < 8192; i++) x += i;
    }
    long long wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &r1);
    double c0 = r0.ru_utime.tv_sec * 1000.0 + r0.ru_utime.tv_usec / 1000.0
              + r0.ru_stime.tv_sec * 1000.0 + r0.ru_stime.tv_usec / 1000.0;
    double c1 = r1.ru_utime.tv_sec * 1000.0 + r1.ru_utime.tv_usec / 1000.0
              + r1.ru_stime.tv_sec * 1000.0 + r1.ru_stime.tv_usec / 1000.0;
    printf("CPUPROBE policy=%s wall_ms=%.1f cpu_ms=%.1f cpu_pct=%.2f unit=pct\n",
           pol_name(g_policy), wall / 1e6, c1 - c0,
           wall > 0 ? 100.0 * (c1 - c0) / (wall / 1e6) : -1.0);
    return 0;
}

/* ---------------- 模式 5：饥饿 ---------------- */

static int g_starve_sec;
static long long g_n_exec_ms;
static long long g_r_exec_ms;

static void *rt_spinner(void *arg) {
    int cpu = (int)(long)arg;
    pin_cpu(cpu);
    struct sched_param sp = {.sched_priority = g_prio};
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "WARN fifo: %s\n", strerror(errno));
    long long c0 = thread_cpu_ms();
    long long end = now_ns() + (long long)g_starve_sec * 1000000000LL;
    volatile unsigned long x = 0;
    long n = 0;
    while (now_ns() < end) {
        for (int i = 0; i < 8192; i++) x += i; /* 不做任何系统调用 */
        n += 8192;
    }
    g_r_exec_ms = thread_cpu_ms() - c0;
    return (void *)n;
}

static void *normal_counter(void *arg) {
    int cpu = (int)(long)arg;
    pin_cpu(cpu);
    apply_policy();
    long long c0 = thread_cpu_ms();
    long long end = now_ns() + (long long)g_starve_sec * 1000000000LL;
    volatile unsigned long x = 0;
    long n = 0;
    while (now_ns() < end) {
        for (int i = 0; i < 8192; i++) x += i;
        n += 8192;
    }
    g_n_exec_ms = thread_cpu_ms() - c0;
    return (void *)n;
}

static int mode_starve(int sec) {
    int cpu = (int)sysconf(_SC_NPROCESSORS_ONLN) - 1;
    g_starve_sec = sec;

    pthread_t t;
    void *ret;
    g_n_exec_ms = -1;
    pthread_create(&t, NULL, normal_counter, (void *)(long)cpu);
    pthread_join(t, &ret);
    long baseline = (long)ret;
    long long solo_exec_ms = g_n_exec_ms;

    pthread_t rt;
    void *rtret;
    pthread_create(&rt, NULL, rt_spinner, (void *)(long)cpu);
    usleep(200000);
    pthread_create(&t, NULL, normal_counter, (void *)(long)cpu);
    pthread_join(t, &ret);
    long starved = (long)ret;
    pthread_join(rt, &rtret);
    long rt_ops = (long)rtret;

    /* 三方对照：普通任务独占时的量、被 RT 压着时的量、RT 自己推进的量 */
    printf("STARVE sec=%d cpu=%d normal_pct=%.2f rt_pct=%.2f "
           "normal_exec_ms=%lld rt_exec_ms=%lld solo_exec_ms=%lld "
           "normal_ops=%ld rt_ops=%ld rt_prio=%d unit=pct\n",
           sec, cpu,
           baseline > 0 ? 100.0 * starved / baseline : -1.0,
           baseline > 0 ? 100.0 * rt_ops / baseline : -1.0,
           g_n_exec_ms, g_r_exec_ms, solo_exec_ms,
           starved, rt_ops, g_prio);
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s pp|decode|work|spin|starve ...\n", argv[0]);
        return 2;
    }
    const char *pe = getenv("SCHED23_POLICY");
    const char *pr = getenv("SCHED23_PRIO");
    g_policy = parse_policy(pe);
    if (pr) g_prio = atoi(pr);

    const char *m = argv[1];
    if (!strcmp(m, "pp") && argc >= 5) {
        int iters = atoi(argv[2]), nspin = atoi(argv[3]), cpu = atoi(argv[4]);
        int same = (argc >= 6) ? atoi(argv[5]) : 0;
        return mode_pp(iters, nspin, cpu, same);
    }
    if (!strcmp(m, "decode") && argc >= 4) return mode_decode(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(m, "work") && argc >= 4)
        return mode_work(atoi(argv[2]), atol(argv[3]), argc >= 5 ? atoi(argv[4]) : -1);
    if (!strcmp(m, "spin") && argc >= 3) return mode_spin(atoi(argv[2]));
    if (!strcmp(m, "cpuprobe") && argc >= 3) return mode_cpuprobe(atoi(argv[2]));
    if (!strcmp(m, "starve") && argc >= 3) return mode_starve(atoi(argv[2]));

    fprintf(stderr, "bad args\n");
    return 2;
}
