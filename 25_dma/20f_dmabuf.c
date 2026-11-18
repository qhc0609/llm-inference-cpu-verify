/* 20f_dmabuf.c - a real dma-buf on this VM, exported by the virtual GPU.
 *
 * vmwgfx is a full DRM driver, so /dev/dri/card0 can still do the three calls
 * that matter: CREATE_DUMB allocates a buffer the device owns, MAP_DUMB gives
 * the CPU a mapping of it, PRIME_HANDLE_TO_FD turns it into a dma-buf file
 * descriptor that any other device (or process) can import. That is the whole
 * mechanism behind "zero copy between CPU and GPU".                          */
#include "common.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define WANT_MB 64
#define LINE 64

static int card = -1;

static double read_pass(uint8_t *p, size_t n, int stride) {
    uint64_t t0 = now_ns(), s = 0;
    for (size_t i = 0; i < n; i += stride) s += p[i];
    uint64_t t1 = now_ns();
    g_sink += s;
    return (double)n / ((double)(t1 - t0) / 1e9) / 1e9;
}

int main(int argc, char **argv) {
    pin_cpu(argc > 1 ? atoi(argv[1]) : 0);
    card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) { RES("dmabuf|open_err|%d", errno); return 1; }
    if (ioctl(card, DRM_IOCTL_SET_MASTER, 0)) RES("dmabuf|set_master|%d", errno);

    /* the dumb buffer must fit the virtual VRAM, so shrink until it does */
    unsigned mb = WANT_MB;
    struct drm_mode_create_dumb cd;
    uint64_t t0 = now_ns();
    for (; mb >= 4; mb /= 2) {
        memset(&cd, 0, sizeof cd);
        cd.height = 1; cd.width = (mb << 20) / 4; cd.bpp = 32;
        if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0) break;
    }
    uint64_t t_create = now_ns() - t0;
    if (mb < 4) { RES("dmabuf|create_failed|%d", errno); return 1; }
    size_t n = (size_t)mb << 20;
    RES("dmabuf|create|%u|%.1f|%u", mb, (double)t_create / 1e6, cd.handle);

    struct drm_mode_map_dumb md; memset(&md, 0, sizeof md); md.handle = cd.handle;
    if (ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &md)) { RES("dmabuf|map_dumb_err|%d", errno); return 1; }
    uint64_t t1 = now_ns();
    uint8_t *nat = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, card, md.offset);
    uint64_t t_map_nat = now_ns() - t1;
    if (nat == MAP_FAILED) { RES("dmabuf|mmap_native_err|%d", errno); return 1; }

    /* first write: this is where the pages actually get backed (or migrated) */
    uint64_t t2 = now_ns();
    memset(nat, 0xA5, n);
    uint64_t t_first = now_ns() - t2;

    /* steady-state write bandwidth into device-owned memory */
    uint64_t v[3];
    for (int r = 0; r < 3; r++) { uint64_t a = now_ns(); memset(nat, 0x11 + r, n); v[r] = now_ns() - a; }
    uint64_t tw = med3(v[0], v[1], v[2]);
    RES("dmabuf|write_native|%u|%.2f|%.1f", mb, (double)n / ((double)tw / 1e9) / 1e9,
        (double)t_first / 1e6);

    /* ---- export to a dma-buf fd ---------------------------------------- */
    struct drm_prime_handle ph; memset(&ph, 0, sizeof ph);
    ph.handle = cd.handle; ph.flags = DRM_CLOEXEC | DRM_RDWR;
    uint64_t t3 = now_ns();
    int r = ioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph);
    uint64_t t_export = now_ns() - t3;
    if (r) { RES("dmabuf|prime_err|%d", errno); return 1; }
    struct stat st; fstat(ph.fd, &st);
    RES("dmabuf|export|%lld|%.1f", (long long)st.st_size, (double)t_export / 1e6);

    uint64_t t4 = now_ns();
    uint8_t *db = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, ph.fd, 0);
    uint64_t t_map_db = now_ns() - t4;
    if (db == MAP_FAILED) { RES("dmabuf|mmap_dmabuf_err|%d", errno); return 1; }

    /* ---- coherence: write through the GEM map, read through the dmabuf -- */
    memset(nat, 0x5C, (size_t)st.st_size);
    int bad = 0;
    for (size_t i = 0; i < (size_t)st.st_size; i += 4096)
        if (db[i] != 0x5C) bad++;
    RES("dmabuf|coherent|%d|%zu", bad, (size_t)st.st_size / 4096);

    /* and the other direction: device-side write, CPU reads through its map */
    memset(db, 0x7E, (size_t)st.st_size);
    int bad2 = 0;
    for (size_t i = 0; i < (size_t)st.st_size; i += 4096)
        if (nat[i] != 0x7E) bad2++;
    RES("dmabuf|coherent_rev|%d", bad2);

    /* ---- what the cache maintenance call costs (DMA_BUF_IOCTL_SYNC) ----- */
    {
        struct dma_buf_sync sy;
        int iters = 20000;
        uint64_t a = now_ns();
        for (int i = 0; i < iters; i++) {
            memset(&sy, 0, sizeof sy);
            sy.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
            ioctl(ph.fd, DMA_BUF_IOCTL_SYNC, &sy);
            sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
            ioctl(ph.fd, DMA_BUF_IOCTL_SYNC, &sy);
        }
        uint64_t dt = now_ns() - a;
        RES("dmabuf|sync_pair_ns|%.0f|%d", (double)dt / (double)iters, iters);
    }

    /* ---- read bandwidth: CPU reading device memory through the dmabuf --- */
    /* 64 B stride = one word per line (the streaming pattern), 8 B = dense.  */
    double r64 = read_pass(db, (size_t)st.st_size, LINE);
    double r8  = read_pass(db, (size_t)st.st_size, 8);
    RES("dmabuf|read_line|%u|%.2f", mb, r64);
    RES("dmabuf|read_dense|%u|%.2f", mb, r8);
    RES("dmabuf|mmap_ns|%.0f|%.0f", (double)t_map_nat, (double)t_map_db);

    munmap(nat, n); munmap(db, (size_t)st.st_size); close(ph.fd);
    return 0;
}
