/* 14 · ARM 大小核调度（big.LITTLE / EAS）
 * 对应文章：第 14 篇《ARM 大小核调度》
 *
 * ★ 诚实声明（必须写进文章）★
 * 本机是 x86-64 虚拟机，4 个核完全对称，没有 big.LITTLE，
 * 也没有 EAS（Energy Aware Scheduling）。所以本程序不能"验证 EAS"，
 * 它做的是两件事：
 *   A. 证明这台机器是对称的（读 sysfs 的 cpu_capacity，全相同）
 *   B. 用"把任务压到少数核上"来构造出和大小核同样的处境——
 *      一部分任务被迫分享算力，另一部分独占，
 *      从而实测 EAS 想要解决的那个不公平问题的量级。
 *
 * 结论只能说明"容量不均时会发生什么"，不能说明"ARM 上 EAS 做得如何"。
 */
#include "common.h"
#include <pthread.h>
#include <sched.h>

#define NTASK 8

typedef struct { int task; double work_ms; int cpu_assigned; } Task;

static void *burn(void *arg) {
    Task *t = (Task *)arg;
    if (t->cpu_assigned >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(t->cpu_assigned, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    double acc = 0;
    double t0 = now_ns();
    /* 固定工作量，看谁先跑完 */
    for (long i = 0; i < 30000000L; i++) acc += (double)(i & 255);
    t->work_ms = (now_ns() - t0) / 1e6;
    if (acc < 0) printf("x");
    return NULL;
}

/* case 0 = 均衡：8 个任务都不绑，让调度器自由摆放在 4 个核上
 * case 1 = 不均衡：4 个任务全挤在 cpu0（模拟小核），
 *                  另外 4 个分到 cpu1~cpu3（模拟大核） */
static void run_case(const char *name, int mode, int ncpu) {
    pthread_t th[NTASK];
    Task tk[NTASK];
    int half = NTASK / 2;

    double t0 = now_ns();
    for (int i = 0; i < NTASK; i++) {
        tk[i].task = i;
        tk[i].work_ms = 0;
        if (mode == 0) {
            tk[i].cpu_assigned = -1;                       /* 不绑 */
        } else {
            tk[i].cpu_assigned = (i < half)
                ? 0                                        /* 全挤在 cpu0 */
                : 1 + ((i - half) % (ncpu > 1 ? ncpu - 1 : 1)); /* 大核组 */
        }
        pthread_create(&th[i], NULL, burn, &tk[i]);
    }
    for (int i = 0; i < NTASK; i++) pthread_join(th[i], NULL);
    double wall = (now_ns() - t0) / 1e6;

    double minv = 1e30, maxv = 0, sum = 0;
    for (int i = 0; i < NTASK; i++) {
        if (tk[i].work_ms < minv) minv = tk[i].work_ms;
        if (tk[i].work_ms > maxv) maxv = tk[i].work_ms;
        sum += tk[i].work_ms;
        printf("TASK|%s|task%d|cpu%d|%.1f ms\n", name, i, tk[i].cpu_assigned, tk[i].work_ms);
    }
    RES("case_%s=wall:%.1f|ms|min_task:%.1f|max_task:%.1f|spread:%.2fx|avg:%.1f",
        name, wall, minv, maxv, maxv / minv, sum / NTASK);
}

int main(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);

    /* ---------- A. 这台机器是不是对称的 ---------- */
    int caps[64], ncap = 0, uniform = 1;
    for (int c = 0; c < ncpu && c < 64; c++) {
        char p[256], v[64] = "?";
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(v, sizeof v, f)) { v[strcspn(v, "\n")] = 0; } fclose(f); }
        caps[ncap++] = atoi(v);
        if (ncap > 1 && caps[ncap - 1] != caps[0]) uniform = 0;
    }
    printf("# cpu_capacity: ");
    for (int i = 0; i < ncap; i++) printf("%d ", caps[i]);
    printf("\n");
    RES("platform_symmetric=%d|cpu_capacity_uniform", uniform);
    RES("platform_ncpu=%d", ncpu);

    /* ---------- B. 构造"容量不均"的处境 ---------- */
    /* 对照组：8 个任务都不绑，4 个核平分（对称平台的最优解） */
    run_case("balanced", 0, ncpu);

    /* 实验组：4 个任务挤 1 个核，另 4 个分 3 个核 —— 就是大小核的处境 */
    run_case("asymmetric", 1, ncpu);

    RES("note=case_asymmetric_emulates_bigLITTLE_capacity_gap_not_real_EAS|1");
    return 0;
}
