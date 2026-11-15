/*
 * OpenHarmony 内核直启用 init
 *
 * 用途：镜像还没打出来时，先验证「这套二进制在 OpenHarmony 内核上能不能跑」。
 * 它做的事只有四件：挂 /proc /sys /dev，把串口接成 stdin/stdout，
 * 顺序 fork 24 个静态基准，跑完 poweroff。
 *
 * 编译：x86_64-linux-ohos clang -static -O2 -o init init.c
 * 用途是测量，不做服务管理、不做多用户、不做清理。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/stat.h>

struct item { const char *name; const char *arg; };

/* 顺序与 run_device.sh 一致；cache_lat 要工作集参数，mini_transformer 要序列长度 */
static struct item items[] = {
    {"topology", 0},
    {"cache_lat", "4"},
    {"branch", 0},
    {"mini_transformer", "64"},
    {"gemm_block", 0},
    {"attn", 0},
    {"quant_gemv", 0},
    {"pd", 0},
    {"thread_cost", 0},
    {"sched_lat", 0},
    {"affinity", 0},
    {"capacity", 0},
    {"omp_scale", 0},
    {"mmap_load", 0},
    {"memcpy_recheck", 0},
    {"thread_scale", 0},
    {"affinity_recheck", 0},
    {"noise_floor", 0},
    {"cache_dip", 0},
    {"cold_warm", 0},
    {"false_share", 0},
    {"thread_proc", 0},
    {"quant_lut", 0},
    {"attn_scale", 0},
};

static void cat(const char *path) {
    int fd = open(path, O_RDONLY);
    char buf[8192];
    if (fd < 0) { printf("(missing %s)\n", path); return; }
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; printf("%s", buf); }
    close(fd);
}

int main(void) {
    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755); mkdir("/tmp", 0777);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }

    struct utsname u;
    uname(&u);
    printf("\n########## OH ENV SELF-PROOF ##########\n");
    printf("uname  : %s %s %s\n", u.sysname, u.release, u.machine);
    printf("version: %s\n", u.version);
    printf("--- /proc/version ---\n"); cat("/proc/version");
    printf("--- THP ---\n"); cat("/sys/kernel/mm/transparent_hugepage/enabled");
    printf("--- GOV ---\n"); cat("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    printf("######################################\n");
    fflush(stdout);

    int n = sizeof(items) / sizeof(items[0]);
    for (int i = 0; i < n; i++) {
        printf("\n=== [%d/%d] %s %s ===\n", i + 1, n, items[i].name,
               items[i].arg ? items[i].arg : "");
        fflush(stdout);
        char path[128];
        snprintf(path, sizeof(path), "/bin/%s", items[i].name);
        pid_t p = fork();
        if (p == 0) {
            if (items[i].arg) execl(path, items[i].name, items[i].arg, (char *)0);
            else execl(path, items[i].name, (char *)0);
            printf("EXEC_FAIL %s\n", path);
            _exit(127);
        }
        int st = 0;
        waitpid(p, &st, 0);
        printf("=== rc=%d ===\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        fflush(stdout);
    }
    printf("\n########## ALL DONE ##########\n");
    sync(); sleep(1);
    reboot(RB_POWER_OFF);
    return 0;
}
