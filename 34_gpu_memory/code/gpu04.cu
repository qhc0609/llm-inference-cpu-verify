// GPU 第04篇：内存体系实测（Kaggle Tesla T4, cc7.5）
// 编译: nvcc -O3 -arch=sm_75 -Xptxas -v -o /tmp/gpu04 /tmp/gpu04.cu
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <cuda_runtime.h>

#define CHK(e) do { cudaError_t r=(e); if(r!=cudaSuccess){ \
  fprintf(stderr,"CUDA_ERR %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(r)); exit(1);} } while(0)

static const int THREADS = 256;
static const int BLOCKS  = 40;          // T4 有 40 个 SM，每 SM 一个块
static const int SH_ELEMS = 8192;       // 32 KB 共享内存

// ---------- 1. 全局内存：连续 / 跨步 ----------
__global__ void k_global_bw(const float* in, float* out, int n, int stride, int loops)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int step = gridDim.x * blockDim.x;
    float s = 0.f;
    for (int r = 0; r < loops; ++r) {
        for (int i = tid; i < n; i += step) s += in[i * stride];
    }
    out[tid] = s;
}

// ---------- 2. 共享内存带宽 / Bank Conflict ----------
__global__ void k_shared_bw(float* out, int stride, int iters)
{
    extern __shared__ float sh[];
    int tid = threadIdx.x;
    for (int i = tid; i < SH_ELEMS; i += blockDim.x) sh[i] = (float)(i & 1023) * 0.001f;
    __syncthreads();
    float s = 0.f;
    for (int k = 0; k < iters; ++k) s += sh[(tid * stride + k) & (SH_ELEMS - 1)];
    out[blockIdx.x * blockDim.x + tid] = s;
}

// ---------- 3. 寄存器驻留 vs 本地内存（动态下标） ----------
__global__ void k_reg_local(const float* in, float* out, int iters, int mode)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    float acc = 0.f;
    if (mode == 0) {
        float x = 0.f;                       // 标量，编译器放寄存器
        #pragma unroll 1
        for (int i = 0; i < iters; ++i) x += in[(tid + i) & 15];
        acc = x;
    } else if (mode == 1) {
        float arr[64];                       // 动态下标（编译器可能仍优化到寄存器）
        #pragma unroll 1
        for (int i = 0; i < iters; ++i) {
            int idx = (tid + i) & 63;
            arr[idx] = in[i & 15];
            acc += arr[idx];
        }
    } else {
        volatile float arr[256];             // volatile 强制落到 local memory
        #pragma unroll 1
        for (int i = 0; i < iters; ++i) {
            int idx = (tid + i) & 255;
            arr[idx] = in[i & 15];
            acc += arr[idx];
        }
    }
    out[tid] = acc;
}

// ---------- 4. 常量内存：广播 vs 分散 ----------
__constant__ float cdata[1024];
__global__ void k_const(const float* g, float* out, int iters, int mode, int use_const)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    float acc = 0.f;
    #pragma unroll 1
    for (int i = 0; i < iters; ++i) {
        int idx = mode == 0 ? 0 : ((tid + i) & 1023);
        acc += use_const ? cdata[idx] : g[idx];
    }
    out[tid] = acc;
}

// ---------- 5. 纹理 / 只读缓存 ----------
__global__ void k_texture(const float* g, float* out, cudaTextureObject_t tex, int n, int loops, int use_tex)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int step = gridDim.x * blockDim.x;
    float s = 0.f;
    for (int r = 0; r < loops; ++r) {
        for (int i = tid; i < n; i += step) s += use_tex ? tex1Dfetch<float>(tex, i) : g[i];
    }
    out[tid] = s;
}

// ---------- 6. 指针追逐延迟（相关访存，无法预取） ----------
__global__ void k_chase_global(const uint32_t* next, uint32_t* out, int n, int steps)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t idx = (uint32_t)(tid & (n - 1));
    uint32_t acc = 0;
    #pragma unroll 1
    for (int i = 0; i < steps; ++i) { idx = next[idx]; acc += idx; }
    out[tid] = acc;
}

__global__ void k_chase_shared(const uint32_t* gnext, uint32_t* out, int n, int steps)
{
    extern __shared__ uint32_t snext[];
    int tid = threadIdx.x;
    for (int i = tid; i < n; i += blockDim.x) snext[i] = gnext[i];
    __syncthreads();
    uint32_t idx = (uint32_t)(tid & (n - 1));
    uint32_t acc = 0;
    #pragma unroll 1
    for (int i = 0; i < steps; ++i) { idx = snext[idx]; acc += idx; }
    out[blockIdx.x * blockDim.x + tid] = acc;
}

// ---------- 计时工具 ----------
template <typename F>
static double bench_us(F launch, int reps)
{
    cudaEvent_t a, b;
    cudaEventCreate(&a); cudaEventCreate(&b);
    launch();
    CHK(cudaDeviceSynchronize());
    cudaEventRecord(a);
    for (int i = 0; i < reps; ++i) launch();
    cudaEventRecord(b);
    CHK(cudaEventSynchronize(b));
    float ms = 0.f;
    cudaEventElapsedTime(&ms, a, b);
    cudaEventDestroy(a); cudaEventDestroy(b);
    return (double)ms * 1000.0 / reps;      // us / call
}

static void make_list(uint32_t* h, int n)
{
    for (int i = 0; i < n; ++i) h[i] = ((uint32_t)i * 1664525u + 1013904223u) & (uint32_t)(n - 1);
}

int main()
{
    cudaDeviceProp p; CHK(cudaGetDeviceProperties(&p, 0));
    int clk = 0, memclk = 0;
    cudaDeviceGetAttribute(&clk, cudaDevAttrClockRate, 0);
    cudaDeviceGetAttribute(&memclk, cudaDevAttrMemoryClockRate, 0);

    printf("=== 第04篇实验原始数据（Kaggle Tesla T4 / cc7.5 / CUDA 12.x）===\n");
    printf("DEV|name=%s|cc=%d.%d|sm=%d|l2_bytes=%d|mem_clock_khz=%d|bus_width=%d|smem_per_sm=%d|regs_per_sm=%d|global_bytes=%zu\n",
           p.name, p.major, p.minor, p.multiProcessorCount, (int)p.l2CacheSize,
           memclk, (int)p.memoryBusWidth, (int)p.sharedMemPerMultiprocessor,
           (int)p.regsPerMultiprocessor, (size_t)p.totalGlobalMem);
    printf("CLK|clk_mhz=%d\n", clk / 1000);

    float* d_out; CHK(cudaMalloc(&d_out, sizeof(float) * BLOCKS * THREADS));
    uint32_t* d_iout; CHK(cudaMalloc(&d_iout, sizeof(uint32_t) * BLOCKS * THREADS));

    // ---- 全局内存：连续 vs 跨步 ----
    {
        const int N = 1 << 22;               // 4 M 个 float = 16 MB 有用数据
        for (int s = 0; s < 4; ++s) {
            int stride = 1 << s;
            size_t bytes = (size_t)N * stride * sizeof(float);
            float* d_in = NULL; CHK(cudaMalloc(&d_in, bytes));
            std::vector<float> h(N, 1.f);
            // 只需填前 N*stride 个中的少量锚点，其余保持 0 也没关系（我们只测带宽）
            CHK(cudaMemset(d_in, 0, bytes));
            CHK(cudaMemcpy(d_in, h.data(), h.size() * sizeof(float), cudaMemcpyHostToDevice));
            int loops = 8;
            double us = bench_us([&]{
                k_global_bw<<<BLOCKS, THREADS>>>(d_in, d_out, N, stride, loops);
            }, 12);
            double gb = (double)N * sizeof(float) * loops / us / 1000.0;
            printf("BW|name=global_stride_%d|us_per_call=%.2f|useful_GBs=%.1f|footprint_MB=%.0f\n",
                   stride, us, gb, bytes / 1048576.0);
            CHK(cudaFree(d_in));
        }
    }

    // ---- 共享内存带宽 + Bank Conflict ----
    {
        for (int s = 0; s < 6; ++s) {
            int stride = 1 << s;             // 1,2,4,8,16,32 -> 1..32 way conflict
            int iters = 2048;
            size_t sh_bytes = SH_ELEMS * sizeof(float);
            double us = bench_us([&]{
                k_shared_bw<<<BLOCKS, THREADS, sh_bytes>>>(d_out, stride, iters);
            }, 20);
            double gb = (double)BLOCKS * THREADS * iters * sizeof(float) / us / 1000.0;
            printf("BW|name=shared_stride_%d_way|us_per_call=%.2f|GBs=%.1f|note=%d_way_conflict\n",
                   stride, us, gb, stride);
        }
    }

    // ---- 寄存器 vs 本地内存 ----
    {
        float* d_small; CHK(cudaMalloc(&d_small, 16 * sizeof(float)));
        std::vector<float> h(16, 1.f);
        CHK(cudaMemcpy(d_small, h.data(), h.size() * sizeof(float), cudaMemcpyHostToDevice));
        int iters = 4096;
        const char* rnames[] = {"register_scalar", "local_array_dynamic_index", "local_volatile_forced"};
        for (int mode = 0; mode < 3; ++mode) {
            double us = bench_us([&]{
                k_reg_local<<<BLOCKS, THREADS>>>(d_small, d_out, iters, mode);
            }, 20);
            printf("REG|name=%s|us_per_call=%.2f|ns_per_iter=%.3f|iters=%d\n",
                   rnames[mode], us, us * 1000.0 / iters, iters);
        }
        CHK(cudaFree(d_small));
    }

    // ---- 常量内存：广播 vs 分散，和全局对照 ----
    {
        std::vector<float> h(1024);
        for (int i = 0; i < 1024; ++i) h[i] = (float)(i & 255) * 0.01f;
        CHK(cudaMemcpyToSymbol(cdata, h.data(), h.size() * sizeof(float)));
        float* d_g = NULL; CHK(cudaMalloc(&d_g, 1024 * sizeof(float)));
        CHK(cudaMemcpy(d_g, h.data(), h.size() * sizeof(float), cudaMemcpyHostToDevice));
        int iters = 8192;
        for (int mode = 0; mode < 2; ++mode) {
            for (int uc = 0; uc < 2; ++uc) {
                double us = bench_us([&]{
                    k_const<<<BLOCKS, THREADS>>>(d_g, d_out, iters, mode, uc);
                }, 12);
                printf("CONST|pattern=%s|space=%s|us_per_call=%.2f|ns_per_read=%.3f\n",
                       mode == 0 ? "broadcast_same_address" : "scatter_1024_words",
                       uc ? "constant" : "global", us, us * 1000.0 / iters);
            }
        }
        CHK(cudaFree(d_g));
    }

    // ---- 纹理 / 只读缓存 ----
    {
        const int N = 1 << 22;
        float* d_in = NULL; CHK(cudaMalloc(&d_in, (size_t)N * sizeof(float)));
        std::vector<float> h(1024, 1.f);
        for (int i = 0; i < 1024; ++i) h[i] = (float)(i & 255);
        CHK(cudaMemset(d_in, 0, (size_t)N * sizeof(float)));
        CHK(cudaMemcpy(d_in, h.data(), h.size() * sizeof(float), cudaMemcpyHostToDevice));
        cudaResourceDesc res; memset(&res, 0, sizeof(res));
        res.resType = cudaResourceTypeLinear;
        res.res.linear.devPtr = d_in;
        res.res.linear.desc.f = cudaChannelFormatKindFloat;
        res.res.linear.desc.x = 32;
        res.res.linear.sizeInBytes = (size_t)N * sizeof(float);
        cudaTextureDesc tdesc; memset(&tdesc, 0, sizeof(tdesc));
        tdesc.readMode = cudaReadModeElementType;
        cudaTextureObject_t tex = 0;
        CHK(cudaCreateTextureObject(&tex, &res, &tdesc, NULL));
        int loops = 8;
        for (int ut = 0; ut < 2; ++ut) {
            double us = bench_us([&]{
                k_texture<<<BLOCKS, THREADS>>>(d_in, d_out, tex, N, loops, ut);
            }, 12);
            printf("TEX|name=%s|us_per_call=%.2f|GBs=%.1f\n",
                   ut ? "texture_fetch" : "normal_global_load", us,
                   (double)N * sizeof(float) * loops / us / 1000.0);
        }
        CHK(cudaDestroyTextureObject(tex));
        CHK(cudaFree(d_in));
    }

    // ---- 工作集大小：L1 / L2 / DRAM ----
    {
        int sizes[] = {16384, 262144, 4194304, 67108864}; // 64KB, 1MB, 16MB, 256MB
        for (int k = 0; k < 4; ++k) {
            int n = sizes[k];
            float* d_in = NULL; CHK(cudaMalloc(&d_in, (size_t)n * sizeof(float)));
            CHK(cudaMemset(d_in, 1, (size_t)n * sizeof(float)));
            size_t target = 268435456ull;                 // 每轮读约 256 MB
            int loops = (int)(target / ((size_t)n * sizeof(float)));
            if (loops < 1) loops = 1;
            double us = bench_us([&]{
                k_global_bw<<<BLOCKS, THREADS>>>(d_in, d_out, n, 1, loops);
            }, 12);
            printf("WS|bytes=%d|loops=%d|us_per_call=%.2f|GBs=%.1f\n",
                   n * (int)sizeof(float), loops, us,
                   (double)n * sizeof(float) * loops / us / 1000.0);
            CHK(cudaFree(d_in));
        }
    }

    // ---- 指针追逐：相关访存延迟 ----
    {
        int ns[] = {8192, 1 << 20, 1 << 26};              // 32KB / 4MB / 256MB
        const char* names[] = {"global_32KB", "global_4MB", "global_256MB"};
        int steps = 512;
        for (int k = 0; k < 3; ++k) {
            int n = ns[k];
            std::vector<uint32_t> h(n); make_list(h.data(), n);
            uint32_t* d_next = NULL; CHK(cudaMalloc(&d_next, (size_t)n * sizeof(uint32_t)));
            CHK(cudaMemcpy(d_next, h.data(), (size_t)n * sizeof(uint32_t), cudaMemcpyHostToDevice));
            double us = bench_us([&]{
                k_chase_global<<<BLOCKS, THREADS>>>(d_next, d_iout, n, steps);
            }, 8);
            printf("LAT|name=%s|steps=%d|us_per_call=%.2f|ns_per_load=%.2f\n",
                   names[k], steps, us, us * 1000.0 / steps);
            CHK(cudaFree(d_next));
        }
        // 共享内存指针追逐
        const int n = 8192;
        std::vector<uint32_t> h(n); make_list(h.data(), n);
        uint32_t* d_next = NULL; CHK(cudaMalloc(&d_next, n * sizeof(uint32_t)));
        CHK(cudaMemcpy(d_next, h.data(), n * sizeof(uint32_t), cudaMemcpyHostToDevice));
        double us = bench_us([&]{
            k_chase_shared<<<BLOCKS, THREADS, n * sizeof(uint32_t)>>>(d_next, d_iout, n, steps);
        }, 8);
        printf("LAT|name=shared_32KB|steps=%d|us_per_call=%.2f|ns_per_load=%.2f\n",
               steps, us, us * 1000.0 / steps);
        CHK(cudaFree(d_next));
    }

    CHK(cudaFree(d_out)); CHK(cudaFree(d_iout));
    printf("=== END ===\n");
    return 0;
}
