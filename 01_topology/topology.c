/* 01 · CPU 硬件整体认知：拓扑、Cache 几何、各级带宽
 * 对应文章：第 01 篇《CPU 硬件整体认知》
 * 验证目标：文章里讲的 cache 层级与带宽递减，在这台机器上是否真的成立
 */
#include "common.h"

static void read_file_str(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, n, "?"); return; }
    if (!fgets(buf, (int)n, f)) snprintf(buf, n, "?");
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
}

int main(void) {
    char buf[256];

    read_file_str("/sys/devices/system/cpu/online", buf, sizeof buf);
    RES("cpu_online_list=%s", buf);

    int ncpu = 0;
    for (;;) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d", ncpu);
        if (access(p, F_OK) != 0) break;
        ncpu++;
    }
    RES("cpu_count=%d", ncpu);

    /* 读 model name：判断是不是同一型号，有没有大小核 */
    FILE *f = fopen("/proc/cpuinfo", "r");
    char model[256] = "unknown";
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) { snprintf(model, sizeof model, "%s", c + 2); model[strcspn(model, "\n")] = 0; }
                break;
            }
        }
        fclose(f);
    }
    RES("cpu_model=%s", model);

    /* Cache 几何：每个 index 目录是一个缓存层级 */
    for (int i = 0; i < 5; i++) {
        char base[256], level[32], type[32], size[32], line_sz[32], ways[32];
        snprintf(base, sizeof base, "/sys/devices/system/cpu/cpu0/cache/index%d", i);
        if (access(base, F_OK) != 0) break;
        char p[320];
        snprintf(p, sizeof p, "%s/level", base);        read_file_str(p, level, sizeof level);
        snprintf(p, sizeof p, "%s/type", base);         read_file_str(p, type, sizeof type);
        snprintf(p, sizeof p, "%s/size", base);         read_file_str(p, size, sizeof size);
        snprintf(p, sizeof p, "%s/coherency_line_size", base); read_file_str(p, line_sz, sizeof line_sz);
        snprintf(p, sizeof p, "%s/ways_of_associativity", base); read_file_str(p, ways, sizeof ways);
        RES("cache_L%s_%s=%s|size|line=%s|ways=%s", level, type, size, line_sz, ways);
    }

    /* 各级工作集下的 memcpy 带宽：印证"越靠近 CPU 越快" */
    const double sizes[] = {32.0 * 1024, 256.0 * 1024, 2.0 * 1024 * 1024,
                            32.0 * 1024 * 1024, 256.0 * 1024 * 1024};
    const char *names[] = {"32KB", "256KB", "2MB", "32MB", "256MB"};

    for (int s = 0; s < 5; s++) {
        size_t n = (size_t)sizes[s];
        char *a = (char *)xmalloc(n);
        char *b = (char *)xmalloc(n);
        memset(b, 1, n); /* 先摸一遍，避免把缺页算进带宽 */

        int iters = (int)(3e9 / (double)n) + 3;   /* 控制总搬运量 ~ 3GB */
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            memcpy(a, b, n);
            __asm__ __volatile__("" ::: "memory");
        }
        double dt = now_ns() - t0;
        double gbps = ((double)n * iters) / dt;   /* bytes/ns == GB/s */
        RES("memcpy_%s=%.2f|GB/s|iters=%d", names[s], gbps, iters);
        free(a); free(b);
    }

    /* 核心拓扑：是不是每个核都有自己的 L2，L3 是否共享 */
    for (int c = 0; c < ncpu && c < 8; c++) {
        char p[320], a[64], b[64];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", c);
        read_file_str(p, a, sizeof a);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", c);
        read_file_str(p, b, sizeof b);
        RES("cpu%d=core_id:%s|package:%s", c, a, b);
    }

    /* 是否暴露 cpu_capacity（大小核才会明显不同） */
    int available = 0, uniform = 1;
    long first = -1;
    for (int c = 0; c < ncpu; c++) {
        char p[320], v[64];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        read_file_str(p, v, sizeof v);
        if (strcmp(v, "?") == 0) continue;      /* 这个内核不上报，跳过 */
        available = 1;
        long cap = atol(v);
        if (first < 0) first = cap;
        else if (cap != first) uniform = 0;
    }
    RES("cpu_capacity_available=%d", available);
    RES("cpu_capacity_uniform=%d|first=%ld", available ? uniform : -1, first);

    return 0;
}
