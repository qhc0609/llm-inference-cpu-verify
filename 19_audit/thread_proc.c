/* 19 · 线程 vs 进程：原实验测出"大内存时 fork 反而更便宜"
 *
 * 原值：小内存 fork/thread = 6.72×，512MB 常驻后 = 0.70×（反转）。
 * 复核要回答两件事：
 *   1) 反转能不能复现；
 *   2) 如果能，是不是 THP 干的 —— 512MB 走 2MB 大页只有 256 个页表项，
 *      fork 复制页表几乎免费；4KB 页要复制 13 万个页表项。
 * 所以这里把 THP 显式打开/关掉各测一遍，看差异是否随页大小翻转。
 */
#include "common.h"
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>

static void *noop(void *a) { (void)a; return NULL; }

static double thread_us(int n) {
    double t0 = now_ns();
    for (int i = 0; i < n; i++) {
        pthread_t t;
        pthread_create(&t, NULL, noop, NULL);
        pthread_join(t, NULL);
    }
    return (now_ns() - t0) / 1000.0 / n;
}

static double fork_us(int n) {
    double t0 = now_ns();
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        int st; waitpid(p, &st, 0);
    }
    return (now_ns() - t0) / 1000.0 / n;
}

/* 摸一遍让物理页真的落到进程上；huge=1 时显式要大页，0 时显式拒绝 */
static char *touch(size_t bytes, int huge) {
    char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(p, bytes, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    for (size_t i = 0; i < bytes; i += 4096) p[i] = 1;
    return p;
}

static void case_report(const char *tag, size_t mb, int huge) {
    char *p = NULL;
    if (mb) p = touch(mb << 20, huge);

    /* 各跑 7 轮，取最小 —— 单次测量在这台机器上没有意义 */
    double bt = 1e18, bf = 1e18;
    for (int r = 0; r < 7; r++) {
        double t = thread_us(200); if (t < bt) bt = t;
        double f = fork_us(60);    if (f < bf) bf = f;
    }
    RES("thr_proc|case=%s|rss_mb=%zu|thp=%s|thread_us=%.1f|fork_us=%.1f|fork_over_thread=%.2f",
        tag, mb, huge ? "on" : "off", bt, bf, bf / bt);
    if (p) munmap(p, mb << 20);
}

int main(void) {
    case_report("small", 0, 1);
    case_report("rss64", 64, 1);
    case_report("rss512_thp_on", 512, 1);
    case_report("rss512_thp_off", 512, 0);
    case_report("rss1024_thp_on", 1024, 1);
    case_report("rss1024_thp_off", 1024, 0);
    return 0;
}
