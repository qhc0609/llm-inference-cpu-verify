/*
 * init4 —— 把「冷读」这件事量干净
 *
 * init3 那次用的是 cold_warm 自己的逻辑：它每轮先写文件、读一次、再 fadvise 丢弃后读一次，
 * 结果下一轮的「热读」其实也是从盘上读的，冷热比被搅成 0.48x。
 *
 * 这里换成最不容易搞错的做法：
 *   1) 文件由宿主机预先放进 ext4 盘里（guest 不写它）；
 *   2) drop_caches 后读一次  = 真冷；
 *   3) 紧接着再读一次        = 真热（页缓存）。
 * 全程只做 read()，8MB 缓冲，跟基准程序一个口径。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void drop_caches(void) {
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) { printf("DROP_FAIL\n"); return; }
    write(fd, "3", 1);
    close(fd);
    sync();
    usleep(300000);
}

/* 返回秒；got 回填读到的字节数 */
static double read_all(const char *path, size_t *got_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }
    size_t buf_sz = 8u << 20;
    char *buf = malloc(buf_sz);
    size_t got = 0;
    double t0 = now_ns();
    ssize_t r;
    while ((r = read(fd, buf, buf_sz)) > 0) got += (size_t)r;
    double dt = now_ns() - t0;
    free(buf);
    close(fd);
    if (got_out) *got_out = got;
    return dt / 1e9;
}

int main(void) {
    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    int cfd = open("/dev/console", O_RDWR);
    if (cfd >= 0) { dup2(cfd, 0); dup2(cfd, 1); dup2(cfd, 2); }

    struct utsname u; uname(&u);
    printf("\n########## INIT4 / REAL DISK ##########\n");
    printf("kernel : %s %s\n", u.release, u.machine);
    printf("#######################################\n");

    mkdir("/root", 0755);
    int rc = mount("/dev/vda", "/root", "ext4", 0, 0);
    printf("MOUNT|dev=/dev/vda|rc=%d\n", rc);
    if (rc != 0) { printf("SKIP\n"); goto out; }

    const char *p = "/root/audit/big.bin";
    struct stat st;
    if (stat(p, &st) != 0) { printf("MISSING|%s\n", p); goto out; }
    double mb = (double)st.st_size / 1048576.0;
    printf("FILE|%s|%.0fMB\n", p, mb);

    /* 冷：丢三次，确认真的丢了再读 */
    for (int r = 0; r < 3; r++) {
        drop_caches();
        size_t got = 0;
        double s = read_all(p, &got);
        printf("RES|disk_cold|round=%d|ms=%.1f|gbps=%.2f|got_mb=%.0f\n",
               r, s * 1e3, (double)got / s / 1e9, (double)got / 1048576.0);
        fflush(stdout);
    }

    /* 热：紧接着连读三次，都不丢缓存 */
    for (int r = 0; r < 3; r++) {
        size_t got = 0;
        double s = read_all(p, &got);
        printf("RES|disk_warm|round=%d|ms=%.1f|gbps=%.2f\n", r, s * 1e3, (double)got / s / 1e9);
        fflush(stdout);
    }

    /* 页表口径：mmap 读同一个文件，冷热各一次 */
    drop_caches();
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            double t0 = now_ns(); volatile long acc = 0;
            for (off_t o = 0; o < st.st_size; o += 4096) acc += m[o];
            double s = (now_ns() - t0) / 1e9;
            printf("RES|mmap_cold|ms=%.1f|gbps=%.2f|sum=%ld\n", s * 1e3, mb / 1024.0 / s, acc);
            t0 = now_ns(); acc = 0;
            for (off_t o = 0; o < st.st_size; o += 4096) acc += m[o];
            s = (now_ns() - t0) / 1e9;
            printf("RES|mmap_warm|ms=%.1f|gbps=%.2f|sum=%ld\n", s * 1e3, mb / 1024.0 / s, acc);
            munmap(m, st.st_size);
        }
        close(fd);
    }

out:
    printf("\n########## ALL DONE ##########\n");
    sync(); sleep(1);
    reboot(RB_POWER_OFF);
    return 0;
}
