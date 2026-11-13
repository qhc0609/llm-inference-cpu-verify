/* 04 · CPU 流水线与分支预测
 * 对应文章：第 04 篇《CPU 流水线与分支预测》
 *
 * 三个实验：
 *   A. 同样的数据，排好序 vs 打乱顺序，跑同一个"小于阈值就累加"的循环。
 *      代码一字不改，只改数据顺序，速度能差好几倍——差的就是分支预测成功率。
 *   B. 依赖链：整数加法 vs 整数乘法，串成一条链，测单次操作的延迟。
 *      C 语言里都是"a = a + b"，但乘法要多等几个周期，这就是流水线深度。
 *   C. 并行度（ILP）：一条链 vs 四条独立链，看吞吐能不能翻上去。
 */
#include "common.h"

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* 返回累加和，防止整个循环被优化掉 */
static long sum_below(const int *v, int n, int thr) {
    long s = 0;
    for (int i = 0; i < n; i++) {
        if (v[i] < thr) s += v[i];   /* 关键：这个分支能否预测 */
    }
    return s;
}

int main(void) {
    const int N = 1 << 16;          /* 256KB，塞不进 L1，但放得进 L2 */
    const int REP = 400;

    int *a = (int *)xmalloc(N * sizeof(int));
    int *b = (int *)xmalloc(N * sizeof(int));

    seed_rng(20261008u);
    for (int i = 0; i < N; i++) a[i] = (int)(drand48() * 256.0);
    memcpy(b, a, N * sizeof(int));
    qsort(b, N, sizeof(int), cmp_int);

    /* ---------- A. 分支预测 ---------- */
    long r1 = 0, r2 = 0;

    double t0 = now_ns();
    for (int r = 0; r < REP; r++) r1 += sum_below(a, N, 128);
    double t_rand = (now_ns() - t0) / (double)(REP * N);

    t0 = now_ns();
    for (int r = 0; r < REP; r++) r2 += sum_below(b, N, 128);
    double t_sorted = (now_ns() - t0) / (double)(REP * N);

    RES("branch_random=%.4f|ns_per_elem", t_rand);
    RES("branch_sorted=%.4f|ns_per_elem", t_sorted);
    RES("branch_speedup=%.2f|x|same_code_diff_data", t_rand / t_sorted);
    printf("# 校验和：%ld vs %ld（必须相等，说明只改了顺序没改语义）\n", r1, r2);

    /* ---------- B. 依赖链延迟 ---------- */
    /* 加法链：每次结果喂给下一次，CPU 只能一个接一个等 */
    long x = 1;
    const long ADD_ITERS = 200000000L;
    t0 = now_ns();
    for (long i = 0; i < ADD_ITERS; i++) {
        __asm__ __volatile__("addq $1, %0" : "+r"(x));
    }
    double add_ns = (now_ns() - t0) / (double)ADD_ITERS;

    /* 乘法链：同样的形式，但乘法器延迟更高 */
    long y = 1;
    const long MUL_ITERS = 100000000L;
    t0 = now_ns();
    for (long i = 0; i < MUL_ITERS; i++) {
        __asm__ __volatile__("imulq $3, %0, %0" : "+r"(y));
    }
    double mul_ns = (now_ns() - t0) / (double)MUL_ITERS;

    RES("dep_chain_add=%.4f|ns_per_op", add_ns);
    RES("dep_chain_mul=%.4f|ns_per_op", mul_ns);
    RES("dep_chain_ratio=%.2f|x", mul_ns / add_ns);

    /* ---------- C. 并行度 ILP ---------- */
    /* 一条链 vs 四条独立链，总操作数相同 */
    const long TOTAL = 400000000L;
    long acc1 = 1, q1 = 1, q2 = 1, q3 = 1, q4 = 1;

    t0 = now_ns();
    for (long i = 0; i < TOTAL; i++) {
        __asm__ __volatile__("addq $1, %0" : "+r"(acc1));
    }
    double t_ilp1 = now_ns() - t0;

    t0 = now_ns();
    for (long i = 0; i < TOTAL / 4; i++) {
        __asm__ __volatile__("addq $1, %0" : "+r"(q1));
        __asm__ __volatile__("addq $1, %0" : "+r"(q2));
        __asm__ __volatile__("addq $1, %0" : "+r"(q3));
        __asm__ __volatile__("addq $1, %0" : "+r"(q4));
    }
    double t_ilp4 = now_ns() - t0;

    RES("ilp_1chain=%.1f|ms_total|ops=%ld", t_ilp1 / 1e6, TOTAL);
    RES("ilp_4chain=%.1f|ms_total|ops=%ld", t_ilp4 / 1e6, TOTAL);
    RES("ilp_speedup=%.2f|x|same_total_ops", t_ilp1 / t_ilp4);

    free(a); free(b);
    return 0;
}
