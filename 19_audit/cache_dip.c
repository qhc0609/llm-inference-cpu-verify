/* 19 · 缓存阶梯复测：专门查那处"256KB 比 128KB 还快"的凹陷
 *
 * 原文章里我写过一句：256KB 的 5.85ns 比 128KB 的 6.84ns 更低，
 * 并解释成"128KB 更容易撞上 L2 的组冲突"。这正是我自己批评过的
 * "给异常编解释"。所以这次把 64KB~320KB 每 32KB 一档全扫一遍，
 * 每档跑 7 次取最小值 —— 如果凹陷在各档之间来回跳，那就是噪声。
 *
 * 第一版这里我写错了：节点用 size_t（8 字节），却按 64 字节算节点数，
 * 于是"256KB"实际只追了 32KB，整条阶梯整体左移 8 倍，还测出了假平台。
 * 这一版每个节点独占一条 64 字节缓存行，工作集就是标称大小。
 */
#include "common.h"

static double chase(size_t bytes, int reps, int *seed) {
    size_t n = bytes / 64;                    /* 每条缓存行一个节点 */
    if (n < 16) n = 16;
    size_t *mem = (size_t *)xmalloc(n * 64);  /* 真实占用 = bytes */
    size_t *perm = (size_t *)xmalloc(n * sizeof(size_t));

    for (size_t i = 0; i < n; i++) perm[i] = i;
    for (size_t i = n - 1; i > 0; i--) {      /* Fisher-Yates，固定种子 */
        size_t j = (size_t)((double)rand_r(seed) / RAND_MAX * (i + 1));
        if (j > i) j = i;
        size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    /* perm 形成一条访问顺序，把它写成链表：节点 k 存下一个节点的字偏移 */
    for (size_t k = 0; k < n; k++)
        mem[perm[k] * 8] = perm[(k + 1) % n] * 8;

    size_t pos = perm[0] * 8;
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        for (size_t k = 0; k < n * 4; k++) pos = mem[pos];
        double dt = now_ns() - t0;
        if (pos == (size_t)-1) fprintf(stderr, "impossible\n");
        double ns = dt / (double)(n * 4);
        if (ns < best) best = ns;
    }
    free(mem); free(perm);
    return best;
}

int main(void) {
    int seed = 12345;
    /* 细扫：64KB ~ 320KB，每档 32KB。L2 是 256KB，凹陷应该在这里现形 */
    for (size_t kb = 64; kb <= 320; kb += 32) {
        double ns = chase(kb * 1024, 7, &seed);
        RES("l2_fine|kb=%zu|ns_per_access=%.3f|cycles_at_2.08ghz=%.1f", kb, ns, ns * 2.08);
    }
    /* 粗扫：32KB ~ 256MB，看三级台阶 */
    size_t coarse[] = {32, 256, 512, 1024, 2048, 4096, 6144, 8192, 16384,
                       32768, 65536, 131072, 262144};
    for (unsigned i = 0; i < sizeof(coarse) / sizeof(coarse[0]); i++) {
        double ns = chase(coarse[i] * 1024, 7, &seed);
        RES("ladder|kb=%zu|ns_per_access=%.3f|cycles_at_2.08ghz=%.1f",
            coarse[i], ns, ns * 2.08);
    }
    return 0;
}
