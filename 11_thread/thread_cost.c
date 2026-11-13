/* 11 · Linux 进程与线程：task_struct / pthread
 * 对应文章：第 11 篇《Linux 进程与线程》
 *
 * 验证四件事：
 *   1) 创建进程比创建线程贵多少（差距来自哪里：地址空间、页表复制）
 *   2) 线程默认栈有多大（虚拟的很大，实际驻留很小）
 *   3) 线程真的共享地址空间吗（改一个变量，主线程立刻看见）
 *   4) 上下文切换一次要多久
 */
#include "common.h"
#include <pthread.h>
#include <sys/wait.h>

static volatile int shared_counter = 0;
static pthread_barrier_t barrier;

/* 空线程：只用来测创建/销毀开销 */
static void *noop(void *arg) { (void)arg; return NULL; }

/* 一起冲过栅栏再累加，保证 8 个线程真的同时活着 */
static void *bump(void *arg) {
    (void)arg;
    pthread_barrier_wait(&barrier);
    __sync_fetch_and_add(&shared_counter, 1);
    return NULL;
}

int main(void) {
    /* ---------- 1. fork 成本 ---------- */
    const int N_FORK = 200;
    double t0 = now_ns();
    for (int i = 0; i < N_FORK; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0);          /* 子进程立刻退出，只测创建+销毁 */
        int st; waitpid(p, &st, 0);
    }
    double fork_us = (now_ns() - t0) / 1000.0 / N_FORK;

    /* ---------- 2. pthread 创建成本 ---------- */
    const int N_TH = 2000;
    t0 = now_ns();
    for (int i = 0; i < N_TH; i++) {
        pthread_t t;
        pthread_create(&t, NULL, noop, NULL);
        pthread_join(t, NULL);
    }
    double thr_us = (now_ns() - t0) / 1000.0 / N_TH;

    RES("fork_us=%.1f|us|n=%d", fork_us, N_FORK);
    RES("thread_create_us=%.1f|us|n=%d", thr_us, N_TH);
    RES("fork_vs_thread_small_rss=%.2f|x", fork_us / thr_us);

    /* ---------- 1b. 地址空间变大后，fork 有多贵 ----------
     * fork 要复制页表：进程占的内存越多，页表越长，复制越慢。
     * 线程不复制页表，所以不受影响。这一条才是 fork/线程真正的分水岭。 */
    const size_t BIG = 512u << 20;      /* 512MB */
    char *big = (char *)malloc(BIG);
    if (big) {
        memset(big, 1, BIG);            /* 真的摸一遍，让物理页落到进程身上 */
        long rss_big = vm_rss_kb();

        double t1 = now_ns();
        for (int i = 0; i < 50; i++) {
            pid_t p = fork();
            if (p == 0) _exit(0);
            int st; waitpid(p, &st, 0);
        }
        double fork_big_us = (now_ns() - t1) / 1000.0 / 50;

        t1 = now_ns();
        for (int i = 0; i < 500; i++) {
            pthread_t t;
            pthread_create(&t, NULL, noop, NULL);
            pthread_join(t, NULL);
        }
        double thr_big_us = (now_ns() - t1) / 1000.0 / 500;

        RES("rss_when_fork_big=%.1f|MB", rss_big / 1024.0);
        RES("fork_us_big_rss=%.1f|us|n=50", fork_big_us);
        RES("thread_us_big_rss=%.1f|us|n=500", thr_big_us);
        RES("fork_growth=%.2f|x|when_rss_grew", fork_big_us / fork_us);
        RES("fork_vs_thread_big_rss=%.1f|x", fork_big_us / thr_big_us);
        free(big);
    }

    /* ---------- 3. 默认栈大小 ---------- */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stack_sz = 0;
    pthread_attr_getstacksize(&attr, &stack_sz);
    RES("default_thread_stack=%.1f|MB", (double)stack_sz / 1048576.0);

    long rss_before = vm_rss_kb();

    /* ---------- 4. 8 线程共享地址空间 ---------- */
    const int NT = 8;
    pthread_t th[8];
    pthread_barrier_init(&barrier, NULL, NT);
    shared_counter = 0;

    t0 = now_ns();
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, bump, NULL);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    double t_8th = (now_ns() - t0) / 1e6;

    long rss_after = vm_rss_kb();

    RES("barrier_8threads=%.2f|ms", t_8th);
    RES("shared_counter_after=%d|expected=%d", shared_counter, NT);
    RES("rss_before=%.1f|MB", rss_before / 1024.0);
    RES("rss_after=%.1f|MB", rss_after / 1024.0);
    RES("rss_delta_8threads=%.1f|MB|vs_virtual_stack_%.0f_MB",
        (rss_after - rss_before) / 1024.0, NT * (double)stack_sz / 1048576.0);

    /* ---------- 5. 上下文切换代价 ---------- */
    const int N_Y = 100000;
    t0 = now_ns();
    for (int i = 0; i < N_Y; i++) sched_yield();
    double yield_us = (now_ns() - t0) / 1000.0 / N_Y;
    RES("sched_yield_once=%.3f|us|n=%d", yield_us, N_Y);

    return 0;
}
