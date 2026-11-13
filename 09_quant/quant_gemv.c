/* 09 · 大模型量化计算：FP16 / INT8 / INT4
 * 对应文章：第 09 篇《大模型量化计算》
 *
 * 三个问题一次答完：
 *   1) 量化到底省了多少内存带宽？（有效带宽 = 权重字节数 / 时间）
 *   2) 量化带来多大误差？（与 FP32 结果比 RMSE 和最大相对误差）
 *   3) 为什么 INT8 通常不掉点，INT4 就要小心？（量化步长差 16 倍）
 *
 * 用的量化方式就是工业界最朴素的那种：per-channel 对称量化。
 * 权重 w_q = round(w / scale)，scale = max|w| / 127（INT8）或 / 7（INT4）
 */
#include "common.h"

#define M 2048      /* 输出维度 */
#define K 2048      /* 输入维度 */

static void gemv_f32(const float *W, const float *x, float *y) {
    for (int i = 0; i < M; i++) {
        float s = 0;
        const float *row = W + (size_t)i * K;
        for (int k = 0; k < K; k++) s += row[k] * x[k];
        y[i] = s;
    }
}

/* INT8：一个字节一个权重，内层用整数累加，最后乘回 scale */
static void gemv_i8(const int8_t *W, const float *scale, const float *x, float *y) {
    for (int i = 0; i < M; i++) {
        int32_t s = 0;
        const int8_t *row = W + (size_t)i * K;
        for (int k = 0; k < K; k++) s += (int32_t)row[k] * (int32_t)x[k];
        y[i] = (float)s * scale[i];
    }
}

/* INT4：两个权重塞一个字节，高低各四位 */
static int8_t unpack4(uint8_t b, int hi) {
    int v = hi ? (b >> 4) : (b & 0x0F);
    return (int8_t)(v - 8);          /* 偏移码：0..15 映射到 -8..7 */
}

static void gemv_i4(const uint8_t *W, const float *scale, const float *x, float *y) {
    for (int i = 0; i < M; i++) {
        int32_t s = 0;
        const uint8_t *row = W + (size_t)i * (K / 2);
        for (int k = 0; k < K; k++) {
            int8_t w = unpack4(row[k >> 1], k & 1);
            s += (int32_t)w * (int32_t)x[k];
        }
        y[i] = (float)s * scale[i];
    }
}

static double rmse(const float *a, const float *b) {
    double s = 0;
    for (int i = 0; i < M; i++) { double d = (double)a[i] - b[i]; s += d * d; }
    return sqrt(s / M);
}

int main(void) {
    float *W = (float *)xmalloc((size_t)M * K * sizeof(float));
    float *x = (float *)xmalloc((size_t)K * sizeof(float));
    float *y_ref = (float *)xmalloc((size_t)M * sizeof(float));
    float *y = (float *)xmalloc((size_t)M * sizeof(float));

    seed_rng(20261008u);
    for (size_t i = 0; i < (size_t)M * K; i++) W[i] = (frand() - 0.5f) * 0.1f;
    /* 激活值量化成整数，模拟真实推理里"激活也量化"的做法 */
    for (int i = 0; i < K; i++) x[i] = (float)((int)(drand48() * 255) - 128);

    /* --- 参考结果 --- */
    double t0 = now_ns();
    gemv_f32(W, x, y_ref);
    double t_f32 = now_ns() - t0;

    /* --- 量化权重 --- */
    int8_t *W8 = (int8_t *)xmalloc((size_t)M * K);
    float *s8 = (float *)xmalloc((size_t)M * sizeof(float));
    uint8_t *W4 = (uint8_t *)xmalloc((size_t)M * (K / 2));
    float *s4 = (float *)xmalloc((size_t)M * sizeof(float));

    for (int i = 0; i < M; i++) {
        const float *row = W + (size_t)i * K;
        float amax = 0;
        for (int k = 0; k < K; k++) if (fabsf(row[k]) > amax) amax = fabsf(row[k]);

        float scale8 = amax / 127.0f;
        s8[i] = scale8;
        for (int k = 0; k < K; k++) {
            int q = (int)lrintf(row[k] / scale8);
            if (q > 127) q = 127;
            if (q < -127) q = -127;
            W8[(size_t)i * K + k] = (int8_t)q;
        }

        float scale4 = amax / 7.0f;
        s4[i] = scale4;
        for (int k = 0; k < K; k += 2) {
            int q0 = (int)lrintf(row[k] / scale4);
            int q1 = (int)lrintf(row[k + 1] / scale4);
            if (q0 > 7) q0 = 7; if (q0 < -8) q0 = -8;
            if (q1 > 7) q1 = 7; if (q1 < -8) q1 = -8;
            W4[(size_t)i * (K / 2) + k / 2] =
                (uint8_t)(((q0 + 8) & 0x0F) | (((q1 + 8) & 0x0F) << 4));
        }
    }

    t0 = now_ns();
    gemv_i8(W8, s8, x, y);
    double t_i8 = now_ns() - t0;
    double e8 = rmse(y_ref, y);

    t0 = now_ns();
    gemv_i4(W4, s4, x, y);
    double t_i4 = now_ns() - t0;
    double e4 = rmse(y_ref, y);

    /* 权重体积 */
    double bytes_f32 = (double)M * K * 4;
    double bytes_i8  = (double)M * K * 1;
    double bytes_i4  = (double)M * K * 0.5;

    printf("# 精度      权重体积(MB)   耗时(ms)   有效带宽(GB/s)   相对误差RMSE\n");
    printf("Q|FP32  %12.2f  %9.3f  %14.1f   %12.2e\n", bytes_f32 / 1048576, t_f32 / 1e6, bytes_f32 / t_f32, 0.0);
    printf("Q|INT8  %12.2f  %9.3f  %14.1f   %12.2e\n", bytes_i8 / 1048576, t_i8 / 1e6, bytes_i8 / t_i8, e8);
    printf("Q|INT4  %12.2f  %9.3f  %14.1f   %12.2e\n", bytes_i4 / 1048576, t_i4 / 1e6, bytes_i4 / t_i4, e4);

    RES("quant_f32=%.3f|ms|bytes_mb=%.2f|bw_gbs=%.1f", t_f32 / 1e6, bytes_f32 / 1048576, bytes_f32 / t_f32);
    RES("quant_int8=%.3f|ms|bytes_mb=%.2f|bw_gbs=%.1f|rmse=%.4e", t_i8 / 1e6, bytes_i8 / 1048576, bytes_i8 / t_i8, e8);
    RES("quant_int4=%.3f|ms|bytes_mb=%.2f|bw_gbs=%.1f|rmse=%.4e", t_i4 / 1e6, bytes_i4 / 1048576, bytes_i4 / t_i4, e4);
    RES("quant_speedup_i8=%.2f|x|vs_f32", t_f32 / t_i8);
    RES("quant_speedup_i4=%.2f|x|vs_f32", t_f32 / t_i4);
    RES("quant_err_ratio=%.1f|x|i4_over_i8", e4 / (e8 > 0 ? e8 : 1));

    return 0;
}
