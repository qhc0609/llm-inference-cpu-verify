/* 19 · 噪声地板与频率：任何"差距很大"的结论，先过这一关
 *
 * 为什么需要它：
 *   同一份代码在虚拟机里跑两次，能差多少？如果两次的差距本身就有 10%，
 *   那么任何小于 10% 的"结论"都不成立。这个数字叫噪声地板（noise floor），
 *   它决定了这台机器上"多小的差异不值得讨论"。
 *
 * 顺便估一下真实主频：整数加法链是 1 周期/迭代的强依赖，
 *   ns/迭代 的倒数就是 GHz。这样能判断 VM 报的 2496 MHz 是不是真的。
 */
#include "common.h"

/* 强依赖加法链：每迭代 1 个周期，编译器无法化简、无法向量化 */
static inline uint64_t chain_add(uint64_t n) {
    uint64_t x = 1;
    for (uint64_t i = 0; i < n; i++)
        __asm__ __volatile__("add $1, %0" : "+r"(x));
    return x;
}

/* 定长工作量：50M 次，2.5GHz 下约 20ms。用来测重复性 */
static inline double one_round(void) {
    double t0 = now_ns();
    uint64_t v = chain_add(50000000ULL);
    double dt = now_ns() - t0;
    if (v == 0) fprintf(stderr, "impossible\n");   /* 防止整个循环被当成死代码 */
    return dt;
}

int main(void) {
    /* ---------- 1. 主频估算 ---------- */
    double best = 1e18;
    for (int i = 0; i < 5; i++) {
        double t0 = now_ns();
        uint64_t v = chain_add(200000000ULL);
        double dt = now_ns() - t0;
        if (v == 0) fprintf(stderr, "impossible\n");
        double ns_per_op = dt / 200000000.0;
        if (dt < best) best = dt;
        RES("freq_est|round=%d|ns_per_add=%.4f|ghz=%.3f", i, ns_per_op, 1.0 / ns_per_op);
    }

    /* ---------- 2. 噪声地板：同一工作量跑 20 次 ---------- */
    enum { R = 20 };
    double ms[R];
    for (int i = 0; i < R; i++) ms[i] = one_round() / 1e6;

    /* 排序取分位数 */
    for (int i = 0; i < R; i++)
        for (int j = i + 1; j < R; j++)
            if (ms[j] < ms[i]) { double t = ms[i]; ms[i] = ms[j]; ms[j] = t; }

    double sum = 0;
    for (int i = 0; i < R; i++) sum += ms[i];
    double mean = sum / R;
    RES("noise|rounds=%d|min_ms=%.2f|p50_ms=%.2f|p99_ms=%.2f|max_ms=%.2f|mean_ms=%.2f",
        R, ms[0], ms[R / 2], ms[(int)(R * 0.99)], ms[R - 1], mean);
    RES("noise|spread_max_over_min_pct=%.2f|spread_p99_over_min_pct=%.2f",
        (ms[R - 1] / ms[0] - 1) * 100, (ms[(int)(R * 0.99)] / ms[0] - 1) * 100);

    /* 这是全篇的尺子：小于这个百分比的差异，不要写成结论 */
    RES("verdict|min_effect_to_claim_pct=%.1f", (ms[R - 1] / ms[0] - 1) * 100);
    return 0;
}
