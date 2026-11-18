/* 20g_direct.c - drive a real DMA engine on this VM.
 *
 * The guest disk is virtio-scsi: every request is a descriptor the driver puts
 * on a ring, and the host (QEMU) does the transfer into guest memory. O_DIRECT
 * asks the kernel to DMA straight into the caller's pages instead of copying
 * through the page cache, so this is the one place in the lab where a device
 * really moves the bytes and the CPU only sets it up.
 *
 * Two things get measured: how throughput depends on request size (that is the
 * per-descriptor cost) and how much CPU time per byte each access mode burns.  */
#include "common.h"
#include <fcntl.h>
#include <sys/stat.h>

#define TOTAL_MB 256
#define FILE_MB  384

static double cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void drop_caches(void) {
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd >= 0) { if (write(fd, "3\n", 2) < 0) {} close(fd); }
}

static void make_file(const char *p) {
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("create"); exit(1); }
    size_t n = (size_t)FILE_MB << 20;
    char *b = xalloc(1u << 20);
    for (size_t off = 0; off < n; off += (1u << 20))
        if (write(fd, b, 1u << 20) != (1 << 20)) { perror("fill"); exit(1); }
    fsync(fd); close(fd); free(b);
}

/* one pass: total_mb bytes, request size `bs`, O_DIRECT if direct */
static int pass(const char *path, size_t bs, int direct, double *gbps, double *cpu_per_byte) {
    int fl = O_RDONLY | (direct ? O_DIRECT : 0);
    int fd = open(path, fl);
    if (fd < 0) return -errno;
    size_t total = (size_t)TOTAL_MB << 20;
    char *buf = NULL;
    if (posix_memalign((void **)&buf, 4096, bs)) return -1;
    double c0 = cpu_ns(); uint64_t t0 = now_ns();
    size_t got = 0;
    while (got < total) {
        ssize_t r = read(fd, buf, bs);
        if (r <= 0) break;
        got += (size_t)r;
    }
    uint64_t t1 = now_ns(); double c1 = cpu_ns();
    close(fd); free(buf);
    if (!got) return -1;
    *gbps = (double)got / ((double)(t1 - t0) / 1e9) / 1e9;
    *cpu_per_byte = (c1 - c0) / (double)got;
    return 0;
}

int main(int argc, char **argv) {
    pin_cpu(argc > 1 ? atoi(argv[1]) : 0);
    const char *path = "/root/lab20/big.bin";
    make_file(path);

    /* --- request-size sweep: this is the descriptor-granularity curve ---- */
    size_t sizes[] = { 4096, 16384, 65536, 262144, 1048576 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        double best = 0, cpu = 0;
        for (int r = 0; r < 3; r++) {
            drop_caches();
            double g, c; int rc = pass(path, sizes[i], 1, &g, &c);
            if (rc) { RES("direct|size|%zu|err|%d", sizes[i], rc); best = 0; break; }
            if (g > best) { best = g; cpu = c; }
        }
        if (best > 0) RES("direct|odirect|%zu|%.2f|%.3f", sizes[i], best, cpu);
    }

    /* --- access mode at a fixed 256 KiB request ------------------------- */
    {
        double g, c;
        drop_caches(); pass(path, 262144, 0, &g, &c);
        RES("direct|buffered|262144|%.2f|%.3f", g, c);
        drop_caches(); pass(path, 262144, 1, &g, &c);
        RES("direct|odirect|262144|%.2f|%.3f", g, c);
    }
    /* --- page cache: read + copy into a second buffer (2 passes) -------- */
    {
        size_t bs = 262144; double g, c;
        drop_caches();
        int fd = open(path, O_RDONLY);
        char *a, *b;
        posix_memalign((void **)&a, 4096, bs); posix_memalign((void **)&b, 4096, bs);
        size_t total = (size_t)TOTAL_MB << 20, got = 0;
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        while (got < total) {
            ssize_t r = read(fd, a, bs);
            if (r <= 0) break;
            memcpy(b, a, (size_t)r);
            got += (size_t)r;
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        g = (double)got / ((double)(t1 - t0) / 1e9) / 1e9;
        c = (c1 - c0) / (double)got;
        RES("direct|read_plus_copy|262144|%.2f|%.3f", g, c);
        close(fd); free(a); free(b);
    }
    return 0;
}
