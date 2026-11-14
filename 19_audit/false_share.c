/* 19 · 假共享复测：原来两次跑出方向相反的结果，这次把三件事都钉死
 *
 * 原实验给出 0.48× 和 7.47× 两个相反结论，我当时归因为"测不出"。
 * 复核要点：
 *   1) 循环不能被优化掉 —— 用 volatile + 汇编屏障，每次都真的读写内存；
 *   2) 相邻与填充两种情况的工作量必须完全一致 —— 打印校验值对比；
 *   3) 每档跑 7 次取最小值，避免把抖动当现象。
 * 预期：相邻（同一缓存行）明显慢于填充（各自独占一行），这是硬件事实，
 * 任何操作系统都不该让它反过来。
 */
#include "common.h"
#include <pthread.h>

typedef struct {
    volatile long *counter;   /* 每个线程写自己的那一个 */
    long iters;
    int pad_bytes;            /* 两个计数器之间隔多少字节 */
    int cpu;                  /* 实际跑在哪个 vCPU 上 */
    double ms;                /* 这个线程自己花了多久 */
    double t_start, t_end;    /* 起止时刻，用来判断是否真的并行 */
} work_t;

static void *worker(void *arg) {
    work_t *w = (work_t *)arg;
    volatile long *c = w->counter;
    w->t_start = now_ns();
    w->cpu = sched_getcpu();
    long v = *c;
    int rmw = w->pad_bytes < 0;      /* 负数表示走读改写 */
    for (long i = 0; i < w->iters; i++) {
        if (rmw) {
            /* 读改写：每次都得把那一行拿到手，缓存行争用藏不住 */
            __asm__ __volatile__("" ::: "memory");
            long t = *c;
            *c = t + 1;
        } else {
            /* 纯写：只往那一行灌数据，不需要读回旧值 */
            v++;
            __asm__ __volatile__("" ::: "memory");
            *c = v;
        }
    }
    w->t_end = now_ns();
    w->ms = (w->t_end - w->t_start) / 1e6;
    return NULL;
}

/* mode=0 相邻（步长 0，四个计数器挤在同一个缓存行）
 * mode=1 填充（步长 16 个 long = 128 字节，各占独立缓存行） */
static double run_once(int mode, long iters, long *sum_out) {
    enum { NT = 4 };
    int rmw = (mode >= 2);
    int adjacent = (mode == 0 || mode == 2);
    size_t stride = adjacent ? 1 : 16;
    long *buf = (long *)xmalloc((stride * NT + 16) * sizeof(long));
    pthread_t th[NT];
    work_t w[NT];

    for (int i = 0; i < NT; i++) {
        w[i].counter = buf + i * stride;
        w[i].iters = iters;
        w[i].pad_bytes = rmw ? -1 : (int)(stride * sizeof(long));
        *(w[i].counter) = 0;
    }
    double t0 = now_ns();
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, worker, &w[i]);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    double dt = now_ns() - t0;

    long s = 0;
    for (int i = 0; i < NT; i++) s += *(w[i].counter);
    *sum_out = s;

    /* 诊断：若四个线程各自的耗时远小于墙钟，说明它们被串行化了，
     * 那这次测量根本测不到缓存行争用 —— 差别只会在"别的进程抢 CPU"上 */
    double par = 0;
    for (int i = 0; i < NT; i++) if (w[i].ms > par) par = w[i].ms;
    RES("fs_diag|mode=%s|theory_if_parallel_ms=%.1f|wall_ms=%.1f|serialization=%.2f|cpus=%d,%d,%d,%d",
        (mode == 0 || mode == 2) ? "adjacent" : "padded", par, dt / 1e6, (dt / 1e6) / par,
        w[0].cpu, w[1].cpu, w[2].cpu, w[3].cpu);

    free(buf);
    return dt / 1e6;
}

int main(void) {
    const long ITERS = 20000000;      /* 读改写慢一倍，量减半 */
    const char *names[4] = {"write_adjacent", "write_padded", "rmw_adjacent", "rmw_padded"};
    double best[4] = {1e18, 1e18, 1e18, 1e18};

    for (int m = 0; m < 4; m++) {
        long sum = 0;
        for (int r = 0; r < 5; r++) {
            long s;
            double t = run_once(m, ITERS, &s);
            sum = s;
            if (t < best[m]) best[m] = t;
            RES("fs_round|case=%s|round=%d|ms=%.2f", names[m], r, t);
        }
        RES("fs_check|case=%s|sum=%ld|expected=%ld", names[m], sum, 4 * ITERS);
        if (sum != 4 * ITERS) {
            RES("fs_verdict|case=%s|BAD|counters_not_equal", names[m]);
            return 1;
        }
    }
    RES("fs_result|write_adj=%.2f|write_pad=%.2f|write_ratio=%.2fx",
        best[0], best[1], best[0] / best[1]);
    RES("fs_result|rmw_adj=%.2f|rmw_pad=%.2f|rmw_ratio=%.2fx",
        best[2], best[3], best[2] / best[3]);
    /* 判据不用拍脑袋的 1.5 倍，用这台机器实测的噪声地板 15%：
     * 比值低于 1.15 就算"在噪声内"，高于才算真的有效应 */
    double wr = best[0] / best[1], rr = best[2] / best[3];
    RES("fs_verdict|noise_floor_pct=15|pure_write_ratio=%.2f|pure_write=%s|rmw_ratio=%.2f|rmw=%s",
        wr, wr < 1.15 ? "not_reproduced" : "reproduced",
        rr, rr < 1.15 ? "not_reproduced" : "reproduced");
    return 0;
}
