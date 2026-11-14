/* 19 · 量化复测：INT4 到底能不能赢 FP32
 *
 * 原文章的结论是"INT4 比 FP32 慢 1.67 倍"。复核发现那条只对"逐元素解包"
 * 这一种写法成立 —— 真引擎没人这么写，都是查表。所以这里摆四种写法：
 *   A. FP32 点积（基线）
 *   B. INT8 点积（整数累加）
 *   C. INT4 朴素解包（移位 + 减零点 + 转浮点）
 *   D. INT4 查表（16 项小表，编译器可放寄存器/一级缓存）
 * 同样两档数据量：16MB（走内存，看带宽）与 1MB（留在缓存，看指令）。
 * 同一件事，写法不同结论就相反 —— 这才是量化真正的样子。
 */
#include "common.h"

static double bench(void (*fn)(void *), void *arg, int reps) {
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        fn(arg);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

typedef struct { const float *a, *b; int n; volatile double sink; } f32_arg;
typedef struct { const int8_t *a, *b; int n; volatile int sink; } i8_arg;
typedef struct { const uint8_t *a, *b; int n; volatile int sink; } i4_arg;

static void dot_f32(void *p) {
    f32_arg *x = (f32_arg *)p;
    /* 四个累加器：拆掉依赖链，gcc 才能用 AVX2 的 8 路 FMA 向量化。
     * 第一版只用一个累加器，浮点加法又有 4 周期延迟，结果被卡成标量，
     * 测出 1.35 GFLOP/s —— 那不是 FP32 的水平，是我写法的水平。 */
    const float *restrict a = x->a, *restrict b = x->b;
    int n = x->n;
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int i = 0;
    for (; i + 3 < n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    x->sink = (double)(s0 + s1 + s2 + s3);
}

static void dot_i8(void *p) {
    i8_arg *x = (i8_arg *)p;
    int s = 0;
    for (int i = 0; i < x->n; i++) s += (int)x->a[i] * (int)x->b[i];
    x->sink = s;
}

/* 朴素：每一个元素都要移位、取值、减零点、转浮点 */
static void dot_i4_naive(void *p) {
    i4_arg *x = (i4_arg *)p;
    int s = 0;
    for (int i = 0; i < x->n; i += 2) {
        int a0 = (x->a[i >> 1] & 0x0F) - 8;
        int b0 = (x->b[i >> 1] & 0x0F) - 8;
        int a1 = (x->a[i >> 1] >> 4) - 8;
        int b1 = (x->b[i >> 1] >> 4) - 8;
        s += a0 * b0 + a1 * b1;
    }
    x->sink = s;
}

/* 查表：nibble 直接当索引，减零点这件事在表里做完了
 * 表只有 16 个 int，编译器会把它放在寄存器或一级缓存里 */
static void dot_i4_lut(void *p) {
    i4_arg *x = (i4_arg *)p;
    static int lut[16];
    static int init = 0;
    if (!init) { for (int i = 0; i < 16; i++) lut[i] = i - 8; init = 1; }
    int s = 0;
    for (int i = 0; i < x->n; i += 2) {
        s += lut[x->a[i >> 1] & 0x0F] * lut[x->b[i >> 1] & 0x0F]
           + lut[x->a[i >> 1] >> 4]    * lut[x->b[i >> 1] >> 4];
    }
    x->sink = s;
}

static void run_case(int n, const char *tag) {
    size_t fb = (size_t)n * sizeof(float);
    float *fa = (float *)xmalloc(fb), *fbb = (float *)xmalloc(fb);
    for (int i = 0; i < n; i++) { fa[i] = frand() * 2 - 1; fbb[i] = frand() * 2 - 1; }

    int8_t *ia = (int8_t *)xmalloc(n), *ib = (int8_t *)xmalloc(n);
    for (int i = 0; i < n; i++) {
        ia[i] = (int8_t)((int)(frand() * 255) - 128);
        ib[i] = (int8_t)((int)(frand() * 255) - 128);
    }
    uint8_t *qa = (uint8_t *)xmalloc((n + 1) / 2), *qb = (uint8_t *)xmalloc((n + 1) / 2);
    for (int i = 0; i < n; i++) {
        int v = (int)(frand() * 15.999) & 0x0F;
        if (i & 1) { qa[i >> 1] |= (uint8_t)(v << 4); qb[i >> 1] |= (uint8_t)(v << 4); }
        else       { qa[i >> 1]  = (uint8_t)v;        qb[i >> 1]  = (uint8_t)v; }
    }

    f32_arg F = { fa, fbb, n, 0 };
    i8_arg  I = { ia, ib, n, 0 };
    i4_arg  Q = { qa, qb, n, 0 };

    double t_f = bench(dot_f32, &F, 5);
    double t_i8 = bench(dot_i8, &I, 5);
    double t_nv = bench(dot_i4_naive, &Q, 5);
    double t_lu = bench(dot_i4_lut, &Q, 5);

    /* 有效带宽：浮点算 2 份 4 字节，int8 算 2 份 1 字节，int4 算 2 份 0.5 字节 */
    double gb_f = 2.0 * fb / t_f;
    double gb_i8 = 2.0 * n / t_i8;
    double gb_i4 = 2.0 * ((n + 1) / 2) / t_lu;

    RES("quant|case=%s|n=%d|fp32_ms=%.3f|gflops=%.2f|eff_gbps=%.2f",
        tag, n, t_f / 1e6, 2.0 * n / t_f, gb_f);
    RES("quant|case=%s|int8_ms=%.3f|vs_fp32=%.2fx|eff_gbps=%.2f",
        tag, t_i8 / 1e6, t_f / t_i8, gb_i8);
    RES("quant|case=%s|int4_naive_ms=%.3f|vs_fp32=%.2fx",
        tag, t_nv / 1e6, t_f / t_nv);
    RES("quant|case=%s|int4_lut_ms=%.3f|vs_fp32=%.2fx|eff_gbps=%.2f|vs_naive=%.2fx",
        tag, t_lu / 1e6, t_f / t_lu, gb_i4, t_nv / t_lu);

    free(fa); free(fbb); free(ia); free(ib); free(qa); free(qb);
}

int main(void) {
    seed_rng(20261114);
    run_case(4 * 1024 * 1024, "16MB_memory_bound");   /* 浮点 32MB，int4 4MB */
    run_case(256 * 1024, "1MB_cache_resident");
    return 0;
}
