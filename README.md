# ckit

Header-only C++20 并发与底层基础设施库，涵盖无锁并发容器、安全内存回收（SMR）、多态内存管理（PMR）、类型擦除与工作窃取任务调度器。

各并发模块提供多种经典论文与工业界方案的对照实现，用于在统一测试与微基准框架下量化锁竞争、cache line 归属、内存序及回收策略的性能开销。全部组件已通过 **GCC / Clang** 双编译器构建、**ASan / UBSan / TSan** 动态检测；EBR、HP、QSBR 与 Executor 通知/在飞计数协议另经 **GenMC** 检验。

`36 个头文件 · 12 个测试套件 · 8 个微基准 · 7 个 GenMC 协议模型`

---

## 模块概览

| 模块 | 头文件数 | 内容 |
|---|---:|---|
| [`ccia`](include/ckit/ccia) | 11 | 《C++ Concurrency in Action》第 6/7 章并发容器：互斥栈、两种互斥队列、查找表、链表；四种无锁栈（HP、`shared_ptr`、`atomic<shared_ptr>`、拆分引用计数）及两种无锁队列（单分支 push、helping）。 |
| [`type_erasure`](include/ckit/type_erasure) | 4 | `MyFunction<R(Args...)>` 四种变体：可拷贝 / 仅移动 × 堆分配 / SBO（32 B）+ 运行期 `target<T>()`。 |
| [`pmr`](include/ckit/pmr) | 2 | 多态内存资源体系：`memory_resource`、`monotonic_buffer_resource`、`unsynchronized_pool_resource`、`synchronized_pool_resource` 及 `polymorphic_allocator`。 |
| [`spsc`](include/ckit/spsc) | 4 | 三种有界 SPSC 环形队列：rigtorp/SPSCQueue、boost::lockfree::spsc_queue（核心实现）、folly::ProducerConsumerQueue。 |
| [`mpmc`](include/ckit/mpmc) | 4 | 三种有界 MPMC 队列：Vyukov、rigtorp/MPMCQueue、xenium `vyukov_bounded_queue`。 |
| [`chase_lev`](include/ckit/chase_lev) | 5 | Chase-Lev 任务窃取双端队列：论文原版（Lê et al. 2013）、Taskflow Bounded/Unbounded WSQ、charlietsao、xenium。 |
| [`reclamation`](include/ckit/reclamation) | 5 | 安全内存回收：三纪元 EBR、风险指针（`hp.hpp` 与偏特化改写 `hp_v2.hpp`）、liburcu 风格与 xenium 风格 QSBR。 |
| [`threadpool`](include/ckit/threadpool) | 1 | Taskflow 风格 `Executor` / `Taskflow`：continuation cache、spill 溢出队列、两阶段通知器及 `corun`。 |

## 构建与集成

环境要求：C++20、CMake ≥ 3.20、POSIX 线程环境（已验证 Linux + GCC / Clang；未测试 MSVC）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

作为 header-only 库接入外部项目：

```cmake
add_subdirectory(path/to/ckit)          # 或直接将 include/ 加入 include path
target_link_libraries(app PRIVATE ckit::ckit)
```

```cpp
#include <ckit/spsc/spsc_rigtorp.hpp>
#include <ckit/threadpool/executor.hpp>
```

### CMake 构建选项

| 选项 | 默认值 | 说明 |
|---|---|---|
| `CKIT_BUILD_TESTS` | `ON` | 构建 12 个正确性与并发压力测试。 |
| `CKIT_BUILD_BENCHMARKS` | `ON` | 构建 8 个微基准程序（强制开启 `-O3 -DNDEBUG`）。 |
| `CKIT_SANITIZER` | 空 | 可选 `thread`、`address`、`undefined` 或 `address,undefined`。 |

```bash
# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=thread -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=address,undefined -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

## 验证矩阵

| 验证项 | 覆盖范围 | 结果 |
|---|---|---|
| GCC 11.4（Release） | 全部测试 | **12 / 12 通过** |
| Clang 14 + libstdc++ | 全部测试 | **12 / 12 通过** |
| ASan + UBSan | 全部测试 | **12 / 12 通过（零告警）** |
| TSan | 全部测试 | **12 / 12 通过（零数据竞争）** |
| GenMC 0.10 | 7 个协议模型 | **7 / 7 验证通过** |

GenMC 协议模型检验独立于 CTest 运行（需确保 `genmc` 在 `PATH` 中，或通过 `GENMC=/path/to/genmc` 指定）：

```bash
bash tests/genmc/run_all.sh
```

每个协议模型均附带用于反向验证的缺陷变异开关，确保 GenMC 能检出对应的内存序或校验缺失错误；详情见 [tests/README.md](tests/README.md)。

## 基准测试摘要

测试环境：Ubuntu 22.04、12 vCPU、16 GB RAM、GCC 11.4、`-O3`（共享虚拟化环境，绝对数值仅作同环境纵向对照参考）。完整数据与方法见 [benchmarks/README.md](benchmarks/README.md)。

| 基准项 | 关键指标 |
|---|---|
| SPSC（1P/1C，4096 槽） | rigtorp **1.90 ns/op**，boost **3.11 ns/op**，folly **3.27 ns/op**（5 次中位数） |
| MPMC（4P/4C，4096 槽） | Vyukov **8.8 Mops/s**，rigtorp **8.5 Mops/s**，xenium **7.7 Mops/s** |
| Chase-Lev（owner / +3 thief） | 单 owner 3.4–5.2 ns/op；叠加 3 thief 后 **73–86 ns/项** |
| CCIA 队列（4P/4C） | 互斥+CV **346 ns/op**，双互斥 **266 ns/op**，无锁 **610–617 ns/op** |
| CCIA 栈（4P/4C） | 互斥 **101 ns/op**，HP **131 ns/op**，`shared_ptr` **282 ns/op** |
| PMR（64 B 分配/释放） | `new/delete` **26.2 ns**，pool **10.5 ns**，monotonic **0.77 ns** |
| 类型擦除（小可调用对象） | SBO 构造+调用 **1.3–1.6 ns**，堆版本 **6.4 ns**，`std::function` **0.89 ns**；纯调用约 **1.2–1.4 ns** |
| SMR（受保护读） | 裸 atomic **1.40 ns**，HP **5.43 ns**，EBR **6.63 ns**，QSBR **8.70 ns** |
| Executor（4 worker） | 平坦图 **94.8 ns/任务**，线性链 **22.1 ns/任务** |

主要观测结论：

* **无锁不等于高吞吐：** 4P/4C 场景下基于链表的无锁队列慢于互斥队列，瓶颈在于每节点堆分配开销与高竞争下的 cache line 乒乓。
* **SBO 的收益集中在构造阶段：** 各实现单次调用均落在 1.2–1.4 ns 区间；32 字节内联缓冲区消除了小闭包的堆分配路径（构造+调用从 6.4 ns 降至 1.3–1.6 ns）。
* **SMR 存在显著读侧开销：** 受保护读操作耗时 5–9 ns，而裸 atomic load 为 1.4 ns；不同 SMR 算法的核心差异在于读侧同步成本与回收内存上界之间的权衡。

## 验证阶段缺陷修复记录

完整缺陷分析、根因定位与补丁记录见 [docs/BUGFIXES.md](docs/BUGFIXES.md)。以下为代表性修复摘要：

| 文件 | 缺陷现象 | 修复方案 |
|---|---|---|
| [`pmr_teaching.hpp`](include/ckit/pmr/pmr_teaching.hpp) | `synchronized_pool_resource::thread_exit` 释放线程池后仍执行无条件断言，导致线程退出崩溃。 | 成功销毁后立即 `return`。 |
| [`liburcu_qsbr.hpp`](include/ckit/reclamation/qsbr/liburcu_qsbr.hpp) | `thread_offline()` 误清 `registered` 标志，导致后续 `thread_online()` / `unregister_thread()` 断言失败。 | 解耦注册态与在线态，仅在 `unregister` 时清除注册标志。 |
| [`lock_free_queue_helping.hpp`](include/ckit/ccia/lock_free_queue_helping.hpp) | 4P/4C 下偶发崩溃：16 字节 `std::atomic<counted_node_ptr>` 按整对象表示比较，未初始化 padding 导致 CAS 伪失败并将 `tail` 置空。 | 显式填充结构体至 16 字节并零初始化 padding；同步修复 half 队列与 split-ref 栈。 |
| [`function_move_only.hpp`](include/ckit/type_erasure/function_move_only.hpp) | `Model::move()` 错误访问外层私有成员 `pImpl_`，实例化即编译失败。 | 移除虚 `move()` 接口，直接转移 `unique_ptr` 所有权。 |
| 四个 `function_*.hpp` | `operator()(Args&&...)` 仅匹配右值实参，传入左值编译失败。 | 按值接收 `Args...` 后完美转发，与 `std::function` 语义对齐。 |
| [`function_copyable_sbo_rtti.hpp`](include/ckit/type_erasure/function_copyable_sbo_rtti.hpp) | `target<T>()` 无条件实例化 `Model<T>`，Clang 下查询不可调用类型触发硬错误。 | 使用 `if constexpr (std::is_invocable_r_v<...>)` 约束向下转换分支。 |
| [`lock_free_stack_atomic_shared_ptr.hpp`](include/ckit/ccia/lock_free_stack_atomic_shared_ptr.hpp) | GCC 11 的 libstdc++ 尚未提供 C++20 `std::atomic<std::shared_ptr<T>>`。 | 基于特性测试宏补充 C++11 原子自由函数兼容层。 |
| [`pmr_teaching.hpp`](include/ckit/pmr/pmr_teaching.hpp) | `ResourceAllocator` 缺失 `size_type` 类型别名。 | 补充 `using size_type = std::size_t;`。 |
| [`hp_v2.hpp`](include/ckit/reclamation/hp/hp_v2.hpp)、[`chase_taskflow.hpp`](include/ckit/chase_lev/chase_taskflow.hpp) | TSan 无法将 `relaxed` load 或 `release fence + relaxed store` 识别为有效 happens-before 边。 | 发布后改用 `acquire` load，BoundedWSQ 槽位写入改用 `release` store，保持原算法语义不变。 |

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── LICENSES.md
├── docs/BUGFIXES.md              # 缺陷分析与修复记录
├── include/ckit/
│   ├── ccia/  type_erasure/  pmr/  spsc/  mpmc/  chase_lev/
│   ├── reclamation/{ebr,hp,qsbr}/
│   └── threadpool/
├── tests/
│   ├── test_utils.hpp            # 断言宏、线程辅助与看门狗
│   ├── ccia/ ... threadpool/     # 分模块测试程序
│   └── {reclamation,threadpool}/genmc/   # GenMC 协议模型
└── benchmarks/
    ├── bench_util.hpp
    └── bench_{ccia,type_erasure,pmr,spsc,mpmc,chase_lev,reclamation,executor}.cpp
```

## 约束与边界说明

* **类型约束：** 部分实现存在编译期前提（如 `mpmc::rigtorp` 要求 `noexcept` 移动/拷贝赋值，`chase::paper` 要求可平凡复制，`pmr` 要求内存资源生命周期长于分配对象），均已通过 `static_assert` 在编译期强制校验。
* **头文件互斥：** `hp.hpp` 与 `hp_v2.hpp` 是同一套回收协议，接口接近但不完全兼容，不可在同一翻译单元中同时包含；`hp_v2` 采用 `deletable_obj_with_new_delete` 与裸 `std::atomic<T*>`（详见 [docs/BUGFIXES.md](docs/BUGFIXES.md) C3）。
* **符号约定：** liburcu 风格 QSBR 导出全局 `rcu_*` 符号以保持与上游接口一致。
* **架构假设：** 默认 cache line 大小为 64 字节（x86-64）；跨架构迁移需调整各模块对应的 `*_config.hpp`。
* **定位声明：** 本仓库为并发机制验证与性能对照的实验性实现，不承诺与标准库或上游开源项目的 ABI 兼容。

## 许可证

本仓库采用混合许可证以遵循各上游派生实现的协议要求，每个头文件首行均附带 SPDX 标识，完整映射见 [LICENSES.md](LICENSES.md)。其中 `include/ckit/pmr` 遵循 `GPL-3.0-or-later WITH GCC-exception-3.1`（派生自 libstdc++），liburcu 风格 QSBR 遵循 `LGPL-2.1-or-later`。