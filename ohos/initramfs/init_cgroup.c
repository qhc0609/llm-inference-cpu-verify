/*
 * init3 —— 补「完整镜像才有的那一层」：真实块设备 + 文件系统 + cgroup
 *
 * 全镜像和内核直启的差别只有四件事：进程/负载、cgroup、动态链接、真实块设备。
 * 前三件已有端点数据（空闲 232 µs ↔ 4 自旋核 5.5 µs），第四件这里补：
 * /root 挂 virtio-blk 上的 ext4，/tmp 指到同一个盘，cold_warm 与 mmap_load 就落在真磁盘上。
 * 再顺手量一次 cgroup 记账对建线程/建进程的影响。
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
#include <sys/stat.h>
#include <sys/utsname.h>

static int run(const char *name, const char *arg) {
    printf("\n=== %s %s ===\n", name, arg ? arg : "");
    fflush(stdout);
    char path[128];
    snprintf(path, sizeof path, "/bin/%s", name);
    pid_t p = fork();
    if (p == 0) {
        if (arg) execl(path, name, arg, (char *)0);
        else execl(path, name, (char *)0);
        printf("EXEC_FAIL %s\n", path);
        _exit(127);
    }
    int st = 0; waitpid(p, &st, 0);
    printf("=== rc=%d ===\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    fflush(stdout);
    return st;
}

static void cat(const char *path) {
    int fd = open(path, O_RDONLY);
    char buf[4096];
    if (fd < 0) { printf("(missing %s)\n", path); return; }
    int n = read(fd, buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; printf("%s", buf); }
    close(fd);
}

int main(void) {
    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755); mkdir("/tmp", 0755);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }

    struct utsname u; uname(&u);
    printf("\n########## INIT3 / DISK+CGROUP ##########\n");
    printf("kernel : %s %s\n", u.release, u.machine);
    printf("#########################################\n");

    /* --- 真磁盘：virtio-blk 上的 ext4 --- */
    mkdir("/root", 0755);
    int rc = mount("/dev/vda", "/root", "ext4", 0, 0);
    printf("MOUNT|dev=/dev/vda|rc=%d\n", rc);
    if (rc == 0) {
        mkdir("/root/audit", 0755);
        mkdir("/root/tmp", 0755);
        rmdir("/tmp");
        if (symlink("/root/tmp", "/tmp") != 0) printf("SYMLINK_FAIL /tmp\n");
        printf("--- 挂载信息 ---\n"); cat("/proc/mounts");
        run("cold_warm", 0);
        run("mmap_load", 0);
    } else {
        printf("SKIP|没有 /dev/vda 或 ext4 不可用\n");
    }

    /* --- cgroup：把 init 自己关进 768M 的组，子进程继承 --- */
    mkdir("/sys/fs/cgroup", 0755);
    int crc = mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, 0);
    printf("CGROUP|mount_rc=%d\n", crc);
    if (crc == 0) {
        mkdir("/sys/fs/cgroup/lim", 0755);
        int f = open("/sys/fs/cgroup/lim/memory.max", O_WRONLY);
        if (f >= 0) { write(f, "805306368", 9); close(f); }
        f = open("/sys/fs/cgroup/lim/cgroup.procs", O_WRONLY);
        if (f >= 0) { char b[16]; int n = snprintf(b, sizeof b, "%d", (int)getpid()); write(f, b, n); close(f); }
        printf("--- cgroup 限额 ---\n");
        cat("/sys/fs/cgroup/lim/memory.max");
        printf("--- 在 cgroup 内重测建线程/建进程 ---\n");
        run("thread_cost", 0);
    }

    printf("\n########## ALL DONE ##########\n");
    sync(); sleep(1);
    reboot(RB_POWER_OFF);
    return 0;
}
