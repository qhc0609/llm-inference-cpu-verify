/* 02 · 从 C 代码到 ARM64 机器指令
 * 对应文章：第 02 篇《从 C 代码到 ARM64 机器指令》
 *
 * 验证目标：同一份 C，在 x86-64 和 aarch64 上编出的指令序列完全不同，
 *           但算出来的结果必须一模一样。
 *
 * 注意这里的写法：
 *   本机是 x86-64 虚拟机，没有 ARM 板子。为了能在 QEMU 里真正跑起
 *   aarch64 程序，ARM 侧用 BARE_METAL 宏切到"纯系统调用"模式——
 *   不链接 libc，自己用 svc 指令写 write/exit。
 *   这不是炫技，是因为 openEuler 的 aarch64 glibc 在 qemu-user 下
 *   启动会崩（rseq 未实现）。顺带也说明了一件事：
 *   printf 并不是语言的一部分，它只是 libc 提供的一层包装。
 */
#include <stdio.h>
#include <stdint.h>

#ifdef BARE_METAL
/* ---- 无 libc 模式：直接和内核说话 ---- */
static inline long sys_write(int fd, const void *buf, unsigned long n) {
    register long x0 __asm__("x0") = fd;
    register const void *x1 __asm__("x1") = buf;
    register long x2 __asm__("x2") = (long)n;
    register long x8 __asm__("x8") = 64;              /* __NR_write */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}
static void emit(const char *s, unsigned long n) { sys_write(1, s, n); }
#else
static void emit(const char *s, unsigned long n) { fwrite(s, 1, n, stdout); }
#endif

static void emit_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    emit(s, n);
}

/* 把定点整数按 "整数部分.小数部分" 打出来，避免依赖 printf 的浮点格式化 */
static void emit_fixed(long long scaled, int digits) {
    char buf[40];
    int p = 0;
    if (scaled < 0) { buf[p++] = '-'; scaled = -scaled; }
    long long div = 1;
    for (int i = 0; i < digits; i++) div *= 10;
    long long ip = scaled / div, fp = scaled % div;
    char tmp[24]; int t = 0;
    if (ip == 0) tmp[t++] = '0';
    while (ip > 0) { tmp[t++] = (char)('0' + ip % 10); ip /= 10; }
    while (t > 0) buf[p++] = tmp[--t];
    buf[p++] = '.';
    for (int i = digits - 1; i >= 0; i--) {
        buf[p + i] = (char)('0' + fp % 10);
        fp /= 10;
    }
    p += digits;
    buf[p++] = '\n';
    emit(buf, (unsigned long)p);
}

/* 点积：推理里最内层的那个循环 */
float dot(const float *a, const float *b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

/* 带分支的逐元素加：对比"条件选择"和"条件跳转"两种编法 */
float add_if(float x, float y, int flag) {
    if (flag) return x + y;
    return x - y;
}

static float A[256], B[256];

void run_checks(void) {
    for (int i = 0; i < 256; i++) {
        A[i] = (float)(i % 17) * 0.5f + 1.0f;
        B[i] = (float)(i % 13) * 0.25f - 2.0f;
    }
    float d = dot(A, B, 256);
    float s1 = add_if(A[3], B[3], 1);
    float s2 = add_if(A[3], B[3], 0);

    emit_str("RES|dot=");
    emit_fixed((long long)(d * 1000000.0f), 6);
    emit_str("RES|add_if_true=");
    emit_fixed((long long)(s1 * 1000000.0f), 6);
    emit_str("RES|add_if_false=");
    emit_fixed((long long)(s2 * 1000000.0f), 6);
}

#ifdef BARE_METAL
void _start(void) { run_checks(); __asm__ __volatile__("mov x0,#0\n mov x8,#93\n svc #0"); }
#else
int main(void) { run_checks(); return 0; }
#endif
