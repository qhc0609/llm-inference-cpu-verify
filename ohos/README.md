# ohos · 在 OpenHarmony 上跑同一套微基准

上面 01~19 那套代码是在 openEuler 24.03（glibc、gcc 12.3）上跑的。这个目录是同一批 `.c` 文件，
换到 **OpenHarmony 5.0.3（EduDist `x86_64_virt`、musl、OH NDK clang 15）** 上再跑一遍。

## 为什么是「内核直启」而不是完整镜像

`x86_64_virt` 的完整系统镜像在官方渠道下不到（教育版预编译包已过期，`ci.openharmony.cn` 不可用），
只能自己编。全量编译要几小时，为了先拿到数据，这里用的是**已经编好的内核 + 自带 initramfs**：

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
