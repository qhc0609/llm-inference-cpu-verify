# ohos · 在 OpenHarmony 上跑同一套微基准

上面 01~19 那套代码是在 openEuler 24.03（glibc、gcc 12.3）上跑的。这个目录是同一批 `.c` 文件，
换到 **OpenHarmony（musl、OH NDK clang 15）** 上再跑一遍。

一共跑了两轮，用的是两种环境：

| 轮次 | 环境 | 内核 | 目录 |
|---|---|---|---|
| 一 | 自编内核 + 自带 initramfs（无用户态） | 6.6.62 | `results/oh_summary.txt` |
| 二 | **完整镜像，launcher / systemui / samgr 常驻** | 6.6.101 | `results/oh7_full_image.txt` |

两轮跑的是同一批二进制、同一台机器、同一个 QEMU/KVM 位置，所以两轮之间能直接相减，
差值就是「用户态在不在」的代价。对照见文末。

## 第一轮：内核直启

`x86_64_virt` 的完整系统镜像在官方渠道下不到（教育版预编译包已过期，`ci.openharmony.cn` 不可用），
自己编要十几小时，为了先拿到数据，这里用的是**已经编好的内核 + 自带 initramfs**：

```
qemu-kvm -kernel bzImage -initrd initrd.img -append "console=ttyS0 rdinit=/init"
```

`init` 只做四件事：挂 `/proc` `/sys` `/dev`、把串口接成 stdout、顺序 fork 24 个静态基准、跑完 `poweroff`。

**口径要说清楚**：这是 OpenHarmony 的**内核**，不是完整的 OpenHarmony 系统。没有 init、hdc、HDF 服务、
它的 cgroup / zswap 配置。所以：

| 类别 | 是否可信 | 例子 |
|---|---|---|
| 纯硬件行为 | 可信 | 缓存台阶、memcpy 带宽、GEMM、attention 平方律、量化 |
| 纯内核机制 | 可信 | 页表、缺页、fork 拷贝、线程创建、CFS 唤醒延迟 |
| 依赖用户态的量 | 待复测 | 系统服务带来的负载与噪声、真实块设备 IO、cgroup 内存限制 |

## 怎么跑

```bash
# 1. 交叉编译（需要 OpenHarmony SDK 的 native 目录）
OH_SDK=/path/to/ohos-sdk/linux/15/native OUT=./bin bash ohos/build_oh.sh

# 2. 编静态 init（同一个 NDK）
$OH_SDK/llvm/bin/clang --target=x86_64-linux-ohos --sysroot=$OH_SDK/sysroot \
    -static -O2 -o ./init ohos/initramfs/init.c

# 3. 打 initramfs
BIN=./bin INIT=./init bash ohos/mk_initrd.sh ./initrd.img

# 4. 起 QEMU（要 KVM，否则延迟类数据全部失真）
KERNEL=/path/to/bzImage INITRD=./initrd.img BIN=./bin bash ohos/qemu_boot.sh
```

完整镜像出来以后，改用 `ohos/run_device.sh`：`hdc file send` 推 `/data/local/tmp/oh_bench`，
`hdc shell sh /data/local/tmp/oh_bench_run.sh`，输出格式同样是 `RES|key=value|unit`。

## 结果

- `results/oh_summary.txt` —— 282 条 `RES|` 行，24 组全部跑完，无失败
- `results/oh_env.txt` —— 环境自证：内核版本、CPU、缓存、调度策略

## 这个环境的两个限制（会影响结论，写文章时必须带上）

1. **没有 THP**：`/sys/kernel/mm/transparent_hugepage/enabled` 不存在，`thread_proc` 里 THP on/off
   两组因此跑成一样的数（10683 vs 10443 µs），那组结论在 openEuler 上是 25 倍差异，在这里不成立。
2. **没有 cpufreq**：`scaling_governor` 不存在，频率不受 governor 控制，但宿主仍有热节流，
   `noise_floor` 实测 20 次极差 45.9%，比 openEuler 那台的 15.2% 大三倍。小于这个数的差异不要当结论。

## 补测：把「完整镜像才有的那一层」单独造出来

全镜像和内核直启的差别只有四件事：进程/负载、cgroup、动态链接、真实块设备。前三件能被已有数据框住
（唤醒延迟的空闲端 232 µs 和有核忙的 5.5 µs 就是负载两端），只有第四件必须额外搭。

`initramfs/init_disk.c`：给 QEMU 挂一块 2 GB 的 ext4（`-drive if=virtio,cache=none`），
256 MB 文件由宿主机 loop 挂载写进去、guest 只读，`echo 3 > drop_caches` 后连读三次，
再在不丢缓存的情况下连读三次，最后用 mmap 逐页碰一遍。这样「冷」和「热」不再互相污染。

`initramfs/init_cgroup.c`：mount cgroup2，建 `memory.max=768MB` 的组，把 init 自己写进
`cgroup.procs`（子进程自动继承），再跑 `thread_cost`。

结果（`results/oh_disk.txt`）：

| 口径 | 数字 |
|---|---|
| 真盘冷读 256 MB | 235 / 368 ms → **0.7~1.1 GB/s** |
| 真盘页缓存热读 | 42.5 ms → **6.3 GB/s** |
| mmap 逐页冷 / 热 | 567 ms → 0.7 ms，**818 倍** |
| initramfs（全在内存）冷读 | 28.3 ms / 512 MB = **18.97 GB/s（虚高 20 倍）** |
| 768 MB cgroup 内 fork / 建线程 | 187.1 / 113.6 µs（组外 232.8 / 117.0，**在噪声内**） |

结论：cgroup 记账改不了这两个数；真正被镜像改变的是文件 IO，而且方向和想象相反——
不是「完整镜像更慢」，是**之前那个 18.97 GB/s 压根不是磁盘**。

## 第二轮：完整镜像跑通了

`x86_64_virt` 的完整包其实有现成的：`harmony-contrib/ohos-qemu` 的 GitHub Release 提供
OpenHarmony 7.0-Release / `x86_64_virt` / phone 的整套镜像（643 MB tar.gz，解开 4.7 GB）。

```bash
# 直连 GitHub 只有 16 KB/s，加代理前缀后 2 MB/s
curl -L -o oh.tar.gz "https://gh-proxy.com/https://github.com/harmony-contrib/ohos-qemu/\
releases/download/v20260919/openharmony-qemu-x86_64-x86_64_virt-phone.tar.gz"
tar xzf oh.tar.gz && cd openharmony-qemu-x86_64-x86_64_virt-phone

# 必须显式指定 qemu 路径，qemu-system-x86_64 不在 PATH 里
setsid ./launch/linux.sh --headless --serial-port 4444 \
    -m 6G -s 4 -q /usr/libexec/qemu-kvm -a kvm > boot.log 2>&1 &

# 进系统（root）
HDC=/path/to/ohos-sdk/linux/15/toolchains/hdc
$HDC tconn 127.0.0.1:5555 && $HDC shell
```

进到系统里是这样：

```
Linux localhost 6.6.101 #1 SMP PREEMPT_DYNAMIC x86_64 Toybox
uid=0(root)
THP: [always] madvise never
launcher / systemui / appspawn / samgr 常驻，锁屏界面已渲染
```

第一轮编的 24 个静态二进制**直接推上去就能跑**，不用重编（同为 `x86_64-linux-ohos`）。

### 两轮对照：哪些数会变

111 项可比，按差异幅度分四档：

| 差异 | 项数 | 代表 |
|---|---|---|
| ±10% | 53 | GEMM 分块、attention 各序列长度、量化误差、依赖链、ILP、`cpu_count` |
| 10~30% | 26 | `thread_create_us` +29%、`memcpy_32KB` +17%、`gemm_loop_ikj` +15% |
| 30~100% | 27 | `fork_us` +115%、`wakeup_rtt_idle_p50` −72%、`memcpy_256MB` +63% |
| >100% | 5 | `fork_us`、`omp_memory_threads4`、`memcpy_2MB`、线程数上限 |

关键几条：

| 指标 | initramfs（6.6.62） | 完整镜像（6.6.101） | 说明 |
|---|---|---|---|
| `fork_us` | 232.8 µs | **500.4 µs** | 系统常驻几十个进程，页表与调度实体都变多 |
| `thread_create_us` | 117.0 µs | **151.2 µs** | 同上，+29% |
| `wakeup_rtt_idle_p50` | 232.86 µs | **66.13 µs** | 服务常驻让 CPU 进不了深 C-state，唤醒反而**变快** 3.5 倍 |
| `fork 1GB thp_on` | 测不出 | **1220.8 µs** | 完整镜像 THP=always 生效 |
| `fork 1GB thp_off` | 10683 µs | **22911 µs** | — |
| THP on/off 差 | 1.0 倍（无 THP） | **18.8 倍** | 第一轮那条「THP 结论不成立」在这里被补上 |

### 缓存曲线：只用第二轮这版

第一轮那版在 128 KB 处出现 22.34 ns 的坏点（比 256 KB 的 10.72 ns 还高），单调性被破坏，
采样不足或受了干扰。完整镜像这版是干净的：

```
8K 1.65 → 16K 1.62 → 32K 1.64 │ 64K 2.80 → 128K 3.81 → 256K 5.95 → 512K 12.96
→ 1M 15.50 → 2M 22.22 │ 4M 41.00 → 8M 76.05 │ 16M 98.61 → 32M 110.70 → 256M 124.86
```

三个拐点正好落在硬件规格上：**32 KB（L1d）、4 MB（L2）、16 MB（L3）**。
512 KB 那个台阶是二级 TLB / 预取边界，不是缓存容量边界。

### 结论怎么用

- 纯硬件与纯算力项（53 项 ±10%）两轮可以互换，写哪个都对。
- 涉及进程、线程、唤醒、内存管理器的项，**必须用第二轮**，第一轮只能当上界/下界参考。
- `memcpy` 全系第二轮高 49~141%，同时大工作集延迟低 12~32%：这两件事同源——THP 让大页多了，
  TLB 缺页少了，带宽和延迟一起改善。第一轮没有 THP，所以两头都吃亏。

