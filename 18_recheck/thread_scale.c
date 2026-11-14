/* thread_scale.c —— 修正版线程扩展测量
 *
 * 修正的错误：原 13_affinity 里每个线程各跑 2000 万次循环，
 * 于是 4 线程做了 4 倍工作量，却直接拿墙钟时间跟 1 线程比，
 * 得出"4 线程比 1 线程慢 3.5 倍"。这是把工作量当成一样的了。
 *
 * 本程序的规矩：总工作量固定（TOTAL_ITERS），只改线程数。
 * 这样墙钟时间可以直接比，吞吐 = 总迭代数 / 墙钟秒。
 * 另附一组"每线程固定工作量"的对照，说明两种口径差在哪。
 */
#include "../common.h"
#include <pthread.h>

#define TOTAL_ITERS 80000000L   /* 总工作量，所有线程数下都一样 */
#define PER_THREAD  20000000L   /* 每线程固定工作量时用这个 */
#define REP 5

static double acc_global;

static void *worker(void *arg) {
    long n = *(const long *)arg;
    double s = 0.0;
    for (long i = 0; i < n; i++) s += (double)(i & 1023) * 1.0000001;
    __sync_fetch_and_add((long *)&acc_global, (long)s);
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y);
}

/* 每个线程拿 per 次迭代，返回最小值与中位数（毫秒） */
static void measure(int nt, long per, double *mn, double *med) {
    double t[REP];
    long jobs[16];
    for (int i = 0; i < nt; i++) jobs[i] = per;

    for (int r = 0; r < REP; r++) {
        pthread_t th[16];
        double t0 = now_ns();
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        t[r] = (now_ns() - t0) / 1e6;
    }
    qsort(t, REP, sizeof(double), cmp_d);
    *mn = t[0];
    *med = t[REP / 2];
}

int main(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    RES("recheck|mode=fixed_total_work|total_iters=%ld|reps=%d|ncpu=%d",
        TOTAL_ITERS, REP, ncpu);

    double base_min = 0, base_med = 0;

    /* A. 总工作量固定：直接看墙钟与吞吐 */
    for (int nt = 1; nt <= ncpu; nt += (nt == 1 ? 1 : 1)) {
        double mn, med;
        measure(nt, TOTAL_ITERS / nt, &mn, &med);
        double tput = TOTAL_ITERS / mn / 1e6;   /* 百万次迭代/秒 */
        if (nt == 1) { base_min = mn; base_med = med; }
        RES("fixed_total|threads=%d|per_thread=%ld|min_ms=%.1f|med_ms=%.1f|"
            "mtput_min=%.1f|mtput_med=%.1f|speedup_min=%.2f|speedup_med=%.2f",
            nt, TOTAL_ITERS / nt, mn, med, tput, TOTAL_ITERS / med / 1e6,
            base_min / mn, base_med / med);
        if (nt == ncpu) break;
    }

    /* B. 每线程固定工作量：能看出"加线程不线性"，但墙钟不可跨线程数比 */
    RES("recheck|mode=fixed_per_thread|per_thread=%ld", PER_THREAD);
    for (int nt = 1; nt <= ncpu; nt++) {
        double mn, med;
        measure(nt, PER_THREAD, &mn, &med);
        RES("fixed_per|threads=%d|total_iters=%ld|min_ms=%.1f|med_ms=%.1f|"
            "tput_min=%.1f",
            nt, PER_THREAD * nt, mn, med, (double)PER_THREAD * nt / mn / 1e6);
    }

    printf("RES|note=%s\n", "B 组的墙钟时间不能拿来跨线程数比较：工作量随线程数线性增长");
    return 0;
}
