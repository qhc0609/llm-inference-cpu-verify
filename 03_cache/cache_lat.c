/* 03 · CPU Cache 体系：实测"工作集变大，访问变慢"
 * 对应文章：第 03 篇《CPU Cache 体系》
 *
 * 方法：构造一条随机置换的指针链，沿链跳 N 次。
 * 每次跳转的地址依赖上一次的结果，CPU 既无法预测也无法并行，
 * 所以测出来的就是一次真实的访存延迟（load-to-use）。
 *
 * 工作集从 8KB 一路拉到 256MB，会依次跨过 L1 / L2 / L3 / 内存，
 * 曲线上会出现明显的台阶——台阶的位置就是各级缓存的容量。
 */
#include "common.h"

int main(int argc, char **argv) {
    int iters_factor = 4;  /* 每个元素平均跳 iters_factor 次 */
    if (argc > 1) iters_factor = atoi(argv[1]);

    printf("# 工作集(字节)  每级元素数  每次访问(ns)  等效带宽(GB/s)\n");

    size_t sizes[] = {
        8u << 10, 16u << 10, 32u << 10, 64u << 10, 128u << 10,
        256u << 10, 512u << 10, 1u << 20, 2u << 20, 4u << 20,
        8u << 20, 16u << 20, 32u << 20, 64u << 20, 128u << 20, 256u << 20
    };
    int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int si = 0; si < nsizes; si++) {
        size_t bytes = sizes[si];
        size_t n = bytes / sizeof(uint32_t);
        if (n < 64) continue;

        uint32_t *idx = (uint32_t *)xmalloc(n * sizeof(uint32_t));
        uint32_t *next = (uint32_t *)xmalloc(n * sizeof(uint32_t));

        for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;

        /* 用固定种子洗牌，然后把洗好的顺序串成一个环：
         * 从任意节点出发跳 n 次，正好把所有元素走一遍 */
        seed_rng(20261008u);
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)(drand48() * (double)(i + 1));
            uint32_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
        }
        for (size_t i = 0; i < n; i++) next[idx[i]] = idx[(i + 1) % n];

        /* 预热：把整条链走一遍，让该进缓存的都进去 */
        uint32_t p = idx[0];
        for (size_t i = 0; i < n; i++) p = next[p];

        long iters = (long)n * iters_factor;
        uint32_t cur = idx[0];
        volatile uint32_t sink = 0;

        double t0 = now_ns();
        for (long i = 0; i < iters; i++) {
            cur = next[cur];
            __asm__ __volatile__("" : "+r"(cur) : : "memory");
        }
        double dt = now_ns() - t0;
        sink = cur;

        double ns_per = dt / (double)iters;
        /* 每次访问实际拉取一整条 cache line（64B），据此换算等效带宽 */
        double gbps = 64.0 / ns_per;
        printf("WS|%9zu|%10zu|%13.2f|%14.2f   # sink=%u\n",
               bytes, n, ns_per, gbps, sink);
        RES("cache_ws_%zuB=%.2f|ns_per_access|n=%zu", bytes, ns_per, n);

        free(idx); free(next);
    }
    return 0;
}
