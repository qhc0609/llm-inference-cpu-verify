/* affinity_recheck.c —— 绑核到底是不是负优化
 *
 * 原 13_affinity 每个配置只跑一次，得到"单线程绑到某核慢 78.8%"。
 * 单次测量在虚拟机里不足以支撑结论，本程序：同一工作量、每个配置跑 5 次，
 * 报最小值与中位数，并逐个核试，看差异是"绑核"造成的还是"某个核"造成的。
 */
#include "../common.h"
#include <pthread.h>
#include <sched.h>

#define ITERS 40000000L
#define REP 5

static double acc;

static void *work(void *arg) {
    int cpu = *(const int *)arg;
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
            fprintf(stderr, "setaffinity failed on cpu %d\n", cpu);
        }
    }
    double s = 0.0;
    for (long i = 0; i < ITERS; i++) s += (double)(i & 1023) * 1.0000001;
    __sync_fetch_and_add((long *)&acc, (long)s);
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y);
}

static void run(int cpu, const char *label, double *mn, double *med) {
    double t[REP];
    for (int r = 0; r < REP; r++) {
        pthread_t th;
        double t0 = now_ns();
        pthread_create(&th, NULL, work, &cpu);
        pthread_join(th, NULL);
        t[r] = (now_ns() - t0) / 1e6;
    }
    qsort(t, REP, sizeof(double), cmp_d);
    *mn = t[0];
    *med = t[REP / 2];
    RES("affinity|cfg=%s|cpu=%d|iters=%ld|min_ms=%.1f|med_ms=%.1f|max_ms=%.1f|spread_pct=%.1f",
        label, cpu, ITERS, *mn, *med, t[REP - 1], (t[REP - 1] - t[0]) / t[0] * 100.0);
}

int main(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    RES("recheck|what=affinity|iters=%ld|reps=%d|ncpu=%d", ITERS, REP, ncpu);

    double f_mn, f_med;
    run(-1, "unpinned", &f_mn, &f_med);

    double worst = 0, best = 1e9;
    for (int c = 0; c < ncpu; c++) {
        double mn, med;
        char name[32];
        snprintf(name, sizeof name, "pin_cpu%d", c);
        run(c, name, &mn, &med);
        double d = (mn - f_mn) / f_mn * 100.0;
        RES("pin_vs_free|cpu=%d|delta_min_pct=%.1f", c, d);
        if (d > worst) worst = d;
        if (d < best) best = d;
    }
    RES("pin_summary|worst_delta_pct=%.1f|best_delta_pct=%.1f|"
        "verdict=%s",
        worst, best,
        (worst - best) > 20 ? "cores_differ" : "cores_uniform_within_noise");
    printf("RES|note=%s\n",
           "若各核差异本身就有 20% 以上，说明是某个 vCPU 被宿主抢占，不是绑核这件事的错");
    return 0;
}
