/* 13 · CPU Affinity 与绑核
 * 对应文章：第 13 篇《CPU Affinity 与绑核》
 *
 * 验证：
 *   1) 不绑核时，线程会在核之间漂移（看被动上下文切换次数）
 *   2) 绑核后，漂移消失，但并行扩展是否变好取决于负载类型
 *   3) 绑核在什么情况下是负优化：线程数 > 物理核数时，
 *      把两个线程绑到同一个核上，比让调度器自由摆放更慢
 */
#include "common.h"
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

typedef struct {
    int tid;
    int pin_cpu;        /* -1 表示不绑 */
    long iters;
    double elapsed_ns;
} Job;

static void *work(void *arg) {
    Job *j = (Job *)arg;

    if (j->pin_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(j->pin_cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }

    /* 纯计算负载：能完全放进寄存器和 L1，代表"算力受限"型任务 */
    double acc = 0.0;
    double t0 = now_ns();
    for (long i = 0; i < j->iters; i++) {
        acc += (double)(i & 1023) * 1.000001;
    }
    j->elapsed_ns = now_ns() - t0;

    if (acc < 0) printf("impossible\n");  /* 防止整个循环被优化掉 */
    return NULL;
}

static double run(int nthreads, int pin_distinct) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    pthread_t th[64];
    Job jobs[64];

    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);

    double t0 = now_ns();
    for (int i = 0; i < nthreads; i++) {
        jobs[i].tid = i;
        jobs[i].pin_cpu = pin_distinct ? (i % ncpu) : -1;
        jobs[i].iters = 20000000L;
        jobs[i].elapsed_ns = 0;
        pthread_create(&th[i], NULL, work, &jobs[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    double wall = (now_ns() - t0) / 1e6;

    getrusage(RUSAGE_SELF, &r1);
    long invol = (long)(r1.ru_nivcsw - r0.ru_nivcsw);

    RES("affinity_run=threads:%d|pin:%d|wall_ms:%.1f|invol_ctxsw:%ld",
        nthreads, pin_distinct, wall, invol);
    return wall;
}

int main(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    RES("cpu_online=%d", ncpu);

    double t1_free = run(1, 0);
    double t1_pin  = run(1, 1);
    RES("single_thread_pin_effect=%.1f|%%|free=%.1fms|pinned=%.1fms",
        (t1_pin - t1_free) / t1_free * 100.0, t1_free, t1_pin);

    double tN_free = run(ncpu, 0);
    double tN_pin  = run(ncpu, 1);
    RES("n_threads_free=%.1f|ms|threads=%d", tN_free, ncpu);
    RES("n_threads_pinned=%.1f|ms|threads=%d", tN_pin, ncpu);
    RES("speedup_free=%.2f|x|vs_single", t1_free / tN_free);
    RES("speedup_pinned=%.2f|x|vs_single", t1_pin / tN_pin);

    /* 过载：线程数是核数的 2 倍，全绑到同一组核上 */
    double t2N_pin = run(ncpu * 2, 1);
    RES("oversub_pinned=%.1f|ms|threads=%d", t2N_pin, ncpu * 2);

    return 0;
}
