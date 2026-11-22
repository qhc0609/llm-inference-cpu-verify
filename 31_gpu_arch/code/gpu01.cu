// gpu01.cu -- A10 微架构实测：身份、延迟阶梯、带宽、FMA 吞吐、warp 分歧、占用率、PCIe
// build: nvcc -O3 -arch=sm_86 -o /tmp/gpu01 /tmp/gpu01.cu
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>

#define CK(x) do { cudaError_t e = (x); if (e != cudaSuccess) { \
    printf("CUDA ERR %s @%d: %s\n", #x, __LINE__, cudaGetErrorString(e)); exit(1);} } while (0)

static double now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ---------------- 1. deviceQuery ----------------
static void dev() {
    int n = 0; CK(cudaGetDeviceCount(&n));
    printf("device_count %d\n", n);
    for (int i = 0; i < n; i++) {
        cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, i));
        printf("name                     %s\n", p.name);
        printf("compute_capability       %d.%d\n", p.major, p.minor);
        printf("multiProcessorCount      %d\n", p.multiProcessorCount);
        printf("maxThreadsPerMultiProc   %d\n", p.maxThreadsPerMultiProcessor);
        printf("maxThreadsPerBlock       %d\n", p.maxThreadsPerBlock);
        printf("warpSize                 %d\n", p.warpSize);
        printf("regsPerMultiprocessor    %d\n", p.regsPerMultiprocessor);
        printf("regsPerBlock             %d\n", p.regsPerBlock);
        printf("sharedMemPerMultiprocessor %zu\n", p.sharedMemPerMultiprocessor);
        printf("sharedMemPerBlock        %zu\n", p.sharedMemPerBlock);
        printf("sharedMemPerBlockOptin   %zu\n", p.sharedMemPerBlockOptin);
        printf("l2CacheSize              %d\n", p.l2CacheSize);
        printf("persistingL2CacheMaxSize %d\n", p.persistingL2CacheMaxSize);
        printf("memoryBusWidth           %d\n", p.memoryBusWidth);
        int mclk = 0, sclk = 0;
        cudaDeviceGetAttribute(&mclk, cudaDevAttrMemoryClockRate, i);
        cudaDeviceGetAttribute(&sclk, cudaDevAttrClockRate, i);
        printf("memoryClockRate_kHz      %d\n", mclk);
        printf("memory_bandwidth_GBs     %.1f\n",
               2.0 * mclk * 1e3 * (p.memoryBusWidth / 8) / 1e9);
        printf("globalMemTotal_MiB       %zu\n", p.totalGlobalMem >> 20);
        printf("clockRate_kHz            %d\n", sclk);
        printf("smCount*128(FP32 lanes)  %d\n", p.multiProcessorCount * 128);
        printf("asyncEngineCount         %d\n", p.asyncEngineCount);
        printf("concurrentKernels        %d\n", p.concurrentKernels);
        printf("unifiedAddressing        %d\n", p.unifiedAddressing);
        printf("managedMemory            %d\n", p.managedMemory);
        printf("pageableMemoryAccess     %d\n", p.pageableMemoryAccess);
        printf("pciBusID                 %04x:%02x:%02x.0\n", p.pciDomainID, p.pciBusID, p.pciDeviceID);
        printf("pciDeviceID              %d\n", p.pciDeviceID);
        printf("tccDriver                %d\n", p.tccDriver);
        int v; cudaDeviceGetAttribute(&v, cudaDevAttrMaxBlocksPerMultiprocessor, i);
        printf("maxBlocksPerSM           %d\n", v);
        cudaDeviceGetAttribute(&v, cudaDevAttrMaxSharedMemoryPerMultiprocessor, i);
        printf("attr_sharedPerSM         %d\n", v);
        cudaDeviceGetAttribute(&v, cudaDevAttrL2CacheSize, i);
        printf("attr_l2                  %d\n", v);
        cudaDeviceGetAttribute(&v, cudaDevAttrClockRate, i);
        printf("attr_clock_kHz           %d\n", v);
    }
}

// ---------------- 2. 延迟阶梯：指针追逐 ----------------
__global__ void chase_kernel(const unsigned *next, unsigned *out, unsigned n, int reps) {
    unsigned idx = threadIdx.x + blockIdx.x * blockDim.x;
    if (idx) return;
    unsigned p = 0;
    for (int r = 0; r < reps; r++) {
        #pragma unroll 8
        for (int i = 0; i < 8; i++) p = __ldg(&next[p]);
    }
    *out = p;
}

static void chase() {
    printf("#size_KB\tns_per_access\n");
    for (int kb = 4; kb <= 512 * 1024; kb *= 4) {
        size_t n = (size_t)kb * 1024 / 4;           // unsigned 元素数
        if (n < 1024) n = 1024;
        unsigned *h = (unsigned *)malloc(n * sizeof(unsigned));
        unsigned *p = (unsigned *)malloc(n * sizeof(unsigned));
        for (size_t i = 0; i < n; i++) p[i] = (unsigned)i;
        // Fisher-Yates 打乱，形成随机环
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)rand() % (i + 1);
            unsigned t = p[i]; p[i] = p[j]; p[j] = t;
        }
        for (size_t i = 0; i < n; i++) h[p[i]] = (unsigned)p[(i + 1) % n];
        unsigned *dn, *dout;
        CK(cudaMalloc(&dn, n * sizeof(unsigned)));
        CK(cudaMalloc(&dout, sizeof(unsigned)));
        CK(cudaMemcpy(dn, h, n * sizeof(unsigned), cudaMemcpyHostToDevice));
        int reps = (int)((size_t)4000000 / n); if (reps < 1) reps = 1;
        chase_kernel<<<1, 32>>>(dn, dout, (unsigned)n, 4);   // warm
        CK(cudaDeviceSynchronize());
        double t0 = now_ms();
        chase_kernel<<<1, 32>>>(dn, dout, (unsigned)n, reps);
        CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        double acc = (double)reps * 8.0;
        printf("%d\t%.2f\n", kb, (t1 - t0) * 1e6 / acc);
        fflush(stdout);
        cudaFree(dn); cudaFree(dout); free(h); free(p);
    }
}

// ---------------- 3. 带宽：float4 拷贝 ----------------
__global__ void copy_kernel(const float4 *a, float4 *b, size_t n4) {
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n4; i += stride) b[i] = a[i];
}

static void bw() {
    printf("#MB\tcopy_kernel_GBs\tmemcpy_GBs\n");
    for (int mb = 1; mb <= 512; mb *= 4) {
        size_t bytes = (size_t)mb << 20;
        float4 *a, *b;
        CK(cudaMalloc(&a, bytes)); CK(cudaMalloc(&b, bytes));
        CK(cudaMemset(a, 1, bytes));
        size_t n4 = bytes / sizeof(float4);
        int grid = 1024, blk = 256;
        copy_kernel<<<grid, blk>>>(a, b, n4); CK(cudaDeviceSynchronize());
        double t0 = now_ms();
        for (int r = 0; r < 5; r++) copy_kernel<<<grid, blk>>>(a, b, n4);
        CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        double gbs = 5.0 * 2.0 * bytes / ((t1 - t0) / 1e3) / 1e9;   // 读+写
        CK(cudaMemcpy(b, a, bytes, cudaMemcpyDeviceToDevice));
        t0 = now_ms();
        for (int r = 0; r < 5; r++) CK(cudaMemcpy(b, a, bytes, cudaMemcpyDeviceToDevice));
        CK(cudaDeviceSynchronize());
        t1 = now_ms();
        double g2 = 5.0 * 2.0 * bytes / ((t1 - t0) / 1e3) / 1e9;
        printf("%d\t%.1f\t%.1f\n", mb, gbs, g2);
        fflush(stdout);
        cudaFree(a); cudaFree(b);
    }
}

// ---------------- 4. FMA 吞吐 ----------------
__global__ void fma_kernel(float *out, int iters) {
    float a0 = threadIdx.x * 1e-6f + 1.0f, a1 = a0 + 0.1f, a2 = a0 + 0.2f, a3 = a0 + 0.3f;
    float a4 = a0 + 0.4f, a5 = a0 + 0.5f, a6 = a0 + 0.6f, a7 = a0 + 0.7f;
    const float m = 0.999999f, c = 1e-7f;
    for (int i = 0; i < iters; i++) {
        a0 = fmaf(a0, m, c); a1 = fmaf(a1, m, c); a2 = fmaf(a2, m, c); a3 = fmaf(a3, m, c);
        a4 = fmaf(a4, m, c); a5 = fmaf(a5, m, c); a6 = fmaf(a6, m, c); a7 = fmaf(a7, m, c);
    }
    if (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 == 12345.0f) *out = a0;
}

static void fma() {
    float *d; CK(cudaMalloc(&d, sizeof(float)));
    int iters = 20000;
    printf("#blocks\tthreads\tGFLOPS\n");
    int cfg[][2] = {{1,32},{1,128},{1,256},{18,256},{36,256},{72,256},{144,256},{288,256},{72,512},{72,1024}};
    for (int k = 0; k < 10; k++) {
        int grid = cfg[k][0], blk = cfg[k][1];
        fma_kernel<<<grid, blk>>>(d, 100); CK(cudaDeviceSynchronize());
        double t0 = now_ms();
        fma_kernel<<<grid, blk>>>(d, iters); CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        double flops = (double)grid * blk * iters * 8.0 * 2.0;
        printf("%d\t%d\t%.0f\n", grid, blk, flops / ((t1 - t0) / 1e3) / 1e9);
        fflush(stdout);
    }
    cudaFree(d);
}

// ---------------- 5. warp 分歧 ----------------
__global__ void div_kernel(float *out, int iters, int mode) {
    float a = threadIdx.x * 1e-6f + 1.0f;
    int tid = threadIdx.x & 31;
    // mode 0：整条 warp 统一 iters 次
    // mode 1：前 16 个线程做 2*iters 次，后 16 个一次不做 —— warp 总工作量相同
    int n = (mode == 0) ? iters : ((tid < 16) ? iters * 2 : 0);
    for (int i = 0; i < n; i++) a = fmaf(a, 0.999999f, 1e-7f);
    if (a == 12345.0f) *out = a;
}

static void div() {
    float *d; CK(cudaMalloc(&d, sizeof(float)));
    int iters = 200000;
    for (int mode = 0; mode < 2; mode++) {
        div_kernel<<<72, 256>>>(d, 100, mode); CK(cudaDeviceSynchronize());
        double t0 = now_ms();
        div_kernel<<<72, 256>>>(d, iters, mode); CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        printf("mode=%d %s\t%.2f ms\n", mode, mode ? "warp内分歧(半数2倍)" : "warp内均匀", t1 - t0);
        fflush(stdout);
    }
    cudaFree(d);
}

// ---------------- 6. 占用率：块大小扫描 ----------------
static void occ() {
    size_t bytes = 256u << 20;
    float4 *a, *b;
    CK(cudaMalloc(&a, bytes)); CK(cudaMalloc(&b, bytes));
    CK(cudaMemset(a, 1, bytes));
    size_t n4 = bytes / sizeof(float4);
    printf("#block\tGBs\n");
    int blks[] = {32, 64, 128, 256, 512, 1024};
    for (int k = 0; k < 6; k++) {
        int blk = blks[k], grid = (int)((n4 + blk - 1) / blk);
        if (grid > 200000) grid = 200000;
        copy_kernel<<<grid, blk>>>(a, b, n4); CK(cudaDeviceSynchronize());
        double t0 = now_ms();
        for (int r = 0; r < 5; r++) copy_kernel<<<grid, blk>>>(a, b, n4);
        CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        printf("%d\t%.1f\n", blk, 5.0 * 2.0 * bytes / ((t1 - t0) / 1e3) / 1e9);
        fflush(stdout);
    }
    cudaFree(a); cudaFree(b);
}

// ---------------- 7. PCIe H2D/D2H ----------------
static void pcie() {
    size_t bytes = 64u << 20;
    void *h = malloc(bytes), *d;
    CK(cudaMalloc(&d, bytes));
    memset(h, 1, bytes);
    for (int k = 0; k < 2; k++) {
        if (k) CK(cudaMemcpy(h, d, bytes, cudaMemcpyDeviceToHost));
        else   CK(cudaMemcpy(d, h, bytes, cudaMemcpyHostToDevice));
        double t0 = now_ms();
        for (int r = 0; r < 5; r++) {
            if (k) CK(cudaMemcpy(h, d, bytes, cudaMemcpyDeviceToHost));
            else   CK(cudaMemcpy(d, h, bytes, cudaMemcpyHostToDevice));
        }
        CK(cudaDeviceSynchronize());
        double t1 = now_ms();
        printf("%s\t%.2f GB/s\n", k ? "D2H" : "H2D", 5.0 * bytes / ((t1 - t0) / 1e3) / 1e9);
    }
    cudaFree(d); free(h);
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "dev";
    if (!strcmp(m, "dev")) dev();
    else if (!strcmp(m, "chase")) chase();
    else if (!strcmp(m, "bw")) bw();
    else if (!strcmp(m, "fma")) fma();
    else if (!strcmp(m, "div")) div();
    else if (!strcmp(m, "occ")) occ();
    else if (!strcmp(m, "pcie")) pcie();
    else printf("unknown\n");
    return 0;
}
