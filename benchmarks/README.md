# 性能基准测试

8 组无第三方依赖的微基准测试，用于在统一环境下对不同并发策略、内存管理与调度实现进行横向对照。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmarks/ckit_bench_spsc
# 运行全部基准
for b in build/benchmarks/ckit_bench_*; do "$b"; done
```

所有基准目标默认强制开启 `-O3 -DNDEBUG`（不受全局构建类型影响）。计时基于 `bench_util.hpp` 中的 `std::chrono::steady_clock`，每项测试均包含一轮预热（warm-up），输出吞吐量（Mops/s）与单次延迟（ns/op）。测试线程未绑核，数据包含 OS 调度噪声；波动较大的 SPSC 项在根 README 中取 5 次运行中位数。

**参考环境：** Ubuntu 22.04.5、12 vCPU、16 GB RAM、GCC 11.4、x86-64（Hyper-V 共享虚拟机）。绝对数值仅反映当前环境量级，请在同表内进行纵向对比。

---

## 基准结果

### 1. SPSC 环形队列（1P / 1C，容量 4096，500 万项）

| 实现 | Mops/s（5 次中位数） | ns/op |
|---|---:|---:|
| `spsc::rigtorp::SPSCQueue` | 526 | 1.90 |
| `spsc::boostv::spsc_queue` | 321 | 3.11 |
| `spsc::follyv::ProducerConsumerQueue` | 306 | 3.27 |

**分析：** `rigtorp` 在本地缓存对端索引，并将读写端索引隔离至独立 cache line，稳态路径避免跨核缓存行失效；`boost` 每次操作均重读对端索引；`folly` 采用 2 的幂掩码与单调索引，但单次读写路径指令开销相对更高。

### 2. MPMC 有界队列（4P / 4C，容量 4096，400 万项）

| 实现 | Mops/s | ns/op |
|---|---:|---:|
| `mpmc::vyukov::mpmc_bounded_queue` | 8.79 | 113.8 |
| `mpmc::rigtorp::Queue` | 8.45 | 118.4 |
| `mpmc::xenium::vyukov_bounded_queue` | 7.74 | 129.1 |

**分析：** 三种实现收敛于同一量级，性能瓶颈集中在全局序列号/位置计数器上的 cache line 争用；`xenium` 的 strong `try_*` 语义额外包含 pending ticket 校验，吞吐略低。

### 3. CCIA 队列与栈（4P / 4C，100 万项）

| 容器 | Mops/s | ns/op |
|---|---:|---:|
| `threadsafe_queue`（互斥 + 条件变量） | 2.89 | 346.5 |
| `threadsafe_queue_fine_grained`（双互斥） | 3.76 | 266.0 |
| `lock_free_queue_half`（单分支 push） | 1.62 | 617.2 |
| `lock_free_queue_helping` | 1.64 | 610.2 |
| `threadsafe_stack`（互斥） | 9.86 | 101.4 |
| `lock_free_stack_shared_ptr` | 3.55 | 281.7 |
| `lock_free_stack_hp`（风险指针） | 7.62 | 131.2 |

**分析：** 8 线程竞争下基于链表的无锁队列吞吐低于互斥队列，主因是每次入队均触发堆节点分配及 `head`/`tail` 高频 CAS 争用，分配器与共享计数器开销主导总耗时。HP 无锁栈通过消除 `shared_ptr` 原子引用计数流量显著快于 `shared_ptr` 版本，接近互斥栈表现，差距主要来自风险指针的发布与扫描开销。

### 4. Chase-Lev 任务窃取双端队列（100 万项，3 个 thief）

| 实现 | owner push+pop（ns/op） | owner + 3 thief（ns/项） |
|---|---:|---:|
| 论文版（Lê et al. 2013） | 5.19 | 75.7 |
| Taskflow `UnboundedWSQ` | 4.51 | 73.2 |
| charlietsao | 3.36 | 82.5 |
| xenium | 5.23 | 86.1 |

**分析：** 无窃取竞争时 owner 路径延迟为 3–5 ns；引入 3 个 thief 后，`top` 索引竞争与缓存行跨核迁移将单任务开销抬升约一个数量级。各实现的相对排序与其 cache line padding 布局及索引缓存策略一致。

### 5. PMR 多态内存资源（100 万次操作）

| 操作 | Mops/s | ns/op |
|---|---:|---:|
| `new` / `delete`（64 B） | 38.1 | 26.2 |
| `unsynchronized_pool_resource`（64 B） | 95.3 | 10.5 |
| `synchronized_pool_resource`（64 B） | 41.2 | 24.3 |
| `monotonic_buffer_resource`（64 B） | 1304.8 | 0.77 |
| `std::vector<int>` 增长（不 reserve） | 1434.7 | 0.70 |
| `std::vector<int>` 增长（reserve） | 1943.9 | 0.51 |
| `pmrt::vector<int>` on monotonic（不 reserve） | 1140.0 | 0.88 |
| `pmrt::vector<int>` on pool（reserve） | 1541.1 | 0.65 |

**分析：** `monotonic_buffer_resource` 仅执行指针递增（bump pointer）且无 free-list 维护，64 B 分配延迟较 `new/delete` 降低约 34 倍；`unsynchronized_pool_resource` 通过 chunk 批分配摊薄系统调用，但保留 size-class 查找与链表操作开销；`synchronized_pool_resource` 在单线程微基准中受锁同步开销影响，表现接近默认 `new/delete`。

### 6. 类型擦除（2000 万次调用 / 30 万次构造）

| 版本 | invoke（ns） | 构造+调用，小对象（ns） | 构造+调用，大对象（ns） |
|---|---:|---:|---:|
| `MyFunction`（堆存储） | 1.21–1.26 | 6.39–6.47 | 11.3–13.0 |
| `MyFunction`（SBO + `target<T>`） | 1.26–1.40 | **1.26–1.58** | 12.3–13.9 |
| `std::function` | 1.20–1.32 | 0.89 | 12.7–12.9 |

**分析：** 纯调用路径由间接分发主导，三者处于同一误差区间（1.2–1.4 ns）。SBO 的核心收益在于消除小可调用对象的堆分配（构造+调用降至 1.26–1.58 ns）；当载荷为 96 B 触发堆溢出后，三者收敛至约 13 ns。

### 7. 安全内存回收读侧开销（500 万次受保护读）

| 方案 | Mops/s | ns/op |
|---|---:|---:|
| 裸 `atomic` load | 712.3 | 1.40 |
| 风险指针（HP）guard | 184.2 | 5.43 |
| EBR guard | 150.9 | 6.63 |
| QSBR guard | 115.0 | 8.70 |

**分析：** 相比裸原子读，所有 SMR 方案均需承担临界区进入/退出同步开销；其中 QSBR 在最外层临界区退出时需发布静默态更新，读侧单次延迟相对更高。

### 8. Executor 任务调度器（4 worker）

| 工作负载 | Mops/s | ns/任务 |
|---|---:|---:|
| 串行循环（原子自增基线） | 268.0 | 3.73 |
| 平坦图（20 万任务） | 10.55 | 94.8 |
| 线性链（5 万任务） | 45.30 | 22.1 |

**分析：** 平坦图每个任务均需经过本地队列入队/出队、窃取探测与通知器路径；线性链依赖 continuation 局部性优化使后继节点留在当前 worker 直接执行，调度开销显著低于平坦图。

---

## 测量边界与注意事项

* 除特别标注项外，各基准均按单进程单次运行采集；受共享虚拟化环境影响，SPSC 类低延迟项跨次运行可能出现最高 2 倍波动。
* 测试未配置 CPU 绑核、`isolcpus` 隔离或实时调度优先级。
* 微基准测量涵盖完整执行路径（含内存分配、安全回收与线程调度），反映端到端开销而非孤立算法复杂度。
* 默认测试载荷为 `int`；大尺寸或非平凡可复制对象会显著放大每节点分配开销，进而改变不同设计的相对优劣。