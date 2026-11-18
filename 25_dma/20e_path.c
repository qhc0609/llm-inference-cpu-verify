/* 20e_path.c - six ways to hand 64 MiB from a producer to a consumer.
 *
 * The producer stands in for "the CPU feeding a GPU/NPU". What matters is not
 * only bandwidth but how much CPU time the producer burns, because that is CPU
 * time the model does not get. Copies counted are the ones the *producer* pays:
 * a write() copies user->kernel, sendfile/splice do not touch the payload, and
 * vmsplice hands the producer's own pages to the pipe (they get pinned).      */
#include "common.h"
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define TOTAL   (64u << 20)
#define CHUNK   (256u << 10)
#define PCHUNK  (256u << 10)

static uint8_t *g_buf;
static size_t   g_total;

typedef struct { int fd; size_t want; int cpu; } drain_t;

static void *drain(void *arg) {
    drain_t *d = arg;
    pin_cpu(d->cpu);
    static __thread uint8_t tmp[PCHUNK];
    size_t got = 0;
    while (got < d->want) {
        size_t want = d->want - got < PCHUNK ? d->want - got : PCHUNK;
        ssize_t r = read(d->fd, tmp, want);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return NULL;
}

static double cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void report(const char *name, int copies, double bytes, uint64_t wall, double cpu) {
    double gbps = bytes / ((double)wall / 1e9) / 1e9;
    RES("path|%s|%d|%.2f|%.1f|%.2f", name, copies, gbps, (double)wall / 1e6,
        cpu / bytes);
}

int main(int argc, char **argv) {
    pin_cpu(argc > 1 ? atoi(argv[1]) : 0);
    g_total = TOTAL;
    g_buf = xalloc(TOTAL);
    memset(g_buf, 0x33, TOTAL);
    double bytes = (double)TOTAL;

    /* ---- 1. pipe: producer copies into the pipe, consumer copies out ---- */
    {
        int p[2]; if (pipe(p)) { perror("pipe"); return 1; }
        fcntl(p[1], F_SETPIPE_SZ, 1u << 20);
        drain_t d = { p[0], TOTAL, 1 };
        pthread_t th; pthread_create(&th, NULL, drain, &d);
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        size_t off = 0;
        while (off < TOTAL) {
            size_t n = TOTAL - off < CHUNK ? TOTAL - off : CHUNK;
            ssize_t w = write(p[1], g_buf + off, n);
            if (w <= 0) break;
            off += (size_t)w;
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        pthread_join(th, NULL);
        report("pipe", 2, bytes, t1 - t0, c1 - c0);
        close(p[0]); close(p[1]);
    }

    /* ---- 2. unix socket: same two copies, plus socket bookkeeping ------- */
    {
        int s[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) { perror("socketpair"); return 1; }
        int big = 4 << 20;
        setsockopt(s[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        setsockopt(s[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        drain_t d = { s[1], TOTAL, 1 };
        pthread_t th; pthread_create(&th, NULL, drain, &d);
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        size_t off = 0;
        while (off < TOTAL) {
            size_t n = TOTAL - off < CHUNK ? TOTAL - off : CHUNK;
            ssize_t w = send(s[0], g_buf + off, n, 0);
            if (w <= 0) break;
            off += (size_t)w;
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        pthread_join(th, NULL);
        report("unixsock", 2, bytes, t1 - t0, c1 - c0);
        close(s[0]); close(s[1]);
    }

    /* ---- 3. memfd + MAP_SHARED: both sides map the same pages ----------- */
    {
        int fd = memfd_create("shared", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, TOTAL)) { perror("memfd"); return 1; }
        uint8_t *m = mmap(NULL, TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { perror("mmap memfd"); return 1; }
        memset(m, 0, TOTAL);
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        memcpy(m, g_buf, TOTAL);              /* producer side: one store pass */
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        uint64_t s = 0;
        for (size_t i = 0; i < TOTAL; i += 64) s += m[i];
        g_sink += s;
        report("memfd_shared", 0, bytes, t1 - t0, c1 - c0);
        munmap(m, TOTAL); close(fd);
    }

    /* ---- 4. sendfile: file page cache -> socket, no user copy ----------- */
    {
        const char *path = "/tmp/dma_lab_src.bin";
        int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (f < 0) { perror("open tmp"); return 1; }
        for (size_t off = 0; off < TOTAL; off += CHUNK)
            if (write(f, g_buf + off, CHUNK) != (ssize_t)CHUNK) { perror("fill"); return 1; }
        fsync(f); lseek(f, 0, SEEK_SET);

        int s[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) { perror("socketpair"); return 1; }
        drain_t d = { s[1], TOTAL, 1 };
        pthread_t th; pthread_create(&th, NULL, drain, &d);
        off_t off = 0;
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        size_t sent = 0;
        while (sent < TOTAL) {
            ssize_t w = sendfile(s[0], f, &off, CHUNK);
            if (w <= 0) { RES("path|sendfile_err|%d", (int)w); break; }
            sent += (size_t)w;
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        pthread_join(th, NULL);
        report("sendfile", 1, (double)sent, t1 - t0, c1 - c0);
        close(s[0]); close(s[1]); close(f);
    }

    /* ---- 5. splice: file -> pipe -> socket, payload never in user space - */
    {
        int f = open("/tmp/dma_lab_src.bin", O_RDONLY);
        if (f < 0) { perror("open src"); return 1; }
        int p[2]; if (pipe(p)) { perror("pipe"); return 1; }
        fcntl(p[1], F_SETPIPE_SZ, 1u << 20);
        int s[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) { perror("socketpair"); return 1; }
        drain_t d = { s[1], TOTAL, 1 };
        pthread_t th; pthread_create(&th, NULL, drain, &d);
        loff_t off = 0; size_t moved = 0;
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        while (moved < TOTAL) {
            ssize_t a = splice(f, &off, p[1], NULL, PCHUNK, 0);
            if (a <= 0) break;
            size_t left = (size_t)a;
            while (left) {
                ssize_t b = splice(p[0], NULL, s[0], NULL, left, 0);
                if (b <= 0) { left = 0; break; }
                left -= (size_t)b; moved += (size_t)b;
            }
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        pthread_join(th, NULL);
        report("splice", 0, (double)moved, t1 - t0, c1 - c0);
        close(f); close(p[0]); close(p[1]); close(s[0]); close(s[1]);
    }

    /* ---- 6. vmsplice: hand the producer's own pages to the pipe --------- */
    {
        int p[2]; if (pipe(p)) { perror("pipe"); return 1; }
        fcntl(p[1], F_SETPIPE_SZ, 1u << 20);
        int s[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) { perror("socketpair"); return 1; }
        drain_t d = { s[1], TOTAL, 1 };
        pthread_t th; pthread_create(&th, NULL, drain, &d);
        size_t moved = 0;
        double c0 = cpu_ns(); uint64_t t0 = now_ns();
        while (moved < TOTAL) {
            size_t n = TOTAL - moved < PCHUNK ? TOTAL - moved : PCHUNK;
            struct iovec iov = { g_buf + moved, n };
            ssize_t a = vmsplice(p[1], &iov, 1, 0);
            if (a <= 0) { RES("path|vmsplice_err|%d", (int)a); break; }
            size_t left = (size_t)a;
            while (left) {
                ssize_t b = splice(p[0], NULL, s[0], NULL, left, 0);
                if (b <= 0) { left = 0; break; }
                left -= (size_t)b; moved += (size_t)b;
            }
        }
        uint64_t t1 = now_ns(); double c1 = cpu_ns();
        pthread_join(th, NULL);
        report("vmsplice", 0, (double)moved, t1 - t0, c1 - c0);
        close(p[0]); close(p[1]); close(s[0]); close(s[1]);
    }
    return 0;
}
