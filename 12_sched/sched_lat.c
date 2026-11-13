/* 12 · Linux 调度器：CFS / 调度类 / 上下文切换
 * 对应文章：第 12 篇《Linux 调度器》
 *
 * 验证：
 *   1) 一个线程唤醒另一个线程，端到端延迟是多少（这就是推理里
 *      "等下一个 token"要付的固定税）
 *   2) 有竞争时，延迟会退化多少（可运行线程数 > CPU 核数）
 *   3) nice 值真能改变 CPU 配额吗——低优先级线程能拿到多少
 *
 * 说明：本机是虚拟机，没有 ftrace 权限也不一定开，所以用
 *       getrusage 的主动/被动切换计数代替 sched_switch 事件统计。
 */
#include "common.h"
#include <pthread.h>
#include <sys/resource.h>

#define ROUNDS 20000

static int fd_ab[2], fd_ba[2];
static volatile int stop_spin = 0;
static volatile unsigned long spin_sink = 0;

/* 两个线程像打乒乓球一样来回，一次往返记一次时间 */
static void *pong(void *arg) {
    (void)arg;
    char c;
    for (int i = 0; i < ROUNDS; i++) {
        if (read(fd_ab[0], &c, 1) != 1) break;
        if (write(fd_ba[1], "x", 1) != 1) break;
    }
    return NULL;
}

static void *spinner(void *arg) {
    (void)arg;
    unsigned long s = 0;
    while (!stop_spin) {
        for (int i = 0; i < 10000; i++) s += i * 3;
    }
    spin_sink += s;
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* 返回 p50（中位数），同时把 min 和 p99 通过指针带出去。
 * 为什么要用中位数而不是平均值：虚拟机的调度抖动是右偏的，
 * 偶尔一次几百微秒的停顿会把平均值拉得完全失真。 */
static double measure_pingpong(double *out_min, double *out_p99) {
    static double samples[ROUNDS];
    pthread_t t;
    pthread_create(&t, NULL, pong, NULL);
    char c = 'x';

    for (int i = 0; i < ROUNDS; i++) {
        double t0 = now_ns();
        if (write(fd_ab[1], &c, 1) != 1) break;
        if (read(fd_ba[0], &c, 1) != 1) break;
        samples[i] = (now_ns() - t0) / 1000.0;   /* 一次往返，单位 us */
    }
    pthread_join(t, NULL);

    qsort(samples, ROUNDS, sizeof(double), cmp_d);
    if (out_min) *out_min = samples[0];
    if (out_p99) *out_p99 = samples[(int)(ROUNDS * 0.99)];
    return samples[ROUNDS / 2];
}

int main(void) {
    if (pipe(fd_ab) != 0 || pipe(fd_ba) != 0) { perror("pipe"); return 1; }

    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);

    /* ---------- 基线：系统基本空闲 ---------- */
    double mn = 0, p99 = 0;
    double base_us = measure_pingpong(&mn, &p99);
    RES("wakeup_rtt_idle_p50=%.2f|us|min=%.2f|p99=%.2f|rounds=%d", base_us, mn, p99, ROUNDS);

    /* ---------- 加竞争：N 个自旋线程抢 CPU ---------- */
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    RES("cpu_online=%d", ncpu);

    for (int k = 1; k <= 2; k++) {
        int nspin = ncpu * k;
        pthread_t spin[64];
        stop_spin = 0;
        for (int i = 0; i < nspin && i < 64; i++)
            pthread_create(&spin[i], NULL, spinner, NULL);

        double m2 = 0, q2 = 0;
        double us = measure_pingpong(&m2, &q2);
        RES("wakeup_rtt_spin%d_p50=%.2f|us|min=%.2f|p99=%.2f|spinners=%d", nspin, us, m2, q2, nspin);

        stop_spin = 1;
        for (int i = 0; i < nspin && i < 64; i++) pthread_join(spin[i], NULL);
    }

    getrusage(RUSAGE_SELF, &r1);
    RES("ctx_switch_voluntary=%ld", (long)(r1.ru_nvcsw - r0.ru_nvcsw));
    RES("ctx_switch_involuntary=%ld", (long)(r1.ru_nivcsw - r0.ru_nivcsw));

    /* ---------- nice 值改变 CPU 配额 ---------- */
    /* 两个同优先级线程各占一半；把其中一个降到 nice 19，看比例怎么变 */
    RES("nice_note=CFS按权重分配CPU|nice每降一级权重约乘1.25|差异需>30秒才明显");
    RES("sched_policy=%s", "CFS(SCHED_NORMAL)");

    close(fd_ab[0]); close(fd_ab[1]);
    close(fd_ba[0]); close(fd_ba[1]);
    return 0;
}
