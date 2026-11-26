# 测试与形式化验证

测试套件无第三方框架依赖，通过轻量级 [`test_utils.hpp`](test_utils.hpp) 提供断言宏（`CKIT_CHECK`、`CKIT_CHECK_EQ`）、线程同步辅助与死锁看门狗。各模块编译为独立可执行文件，可通过 CTest 统一调度。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

单独运行指定测试目标可查看各用例明细：

```bash
./build/tests/ckit_test_ccia
```

## 测试覆盖范围

| 测试程序 | 源文件 | 覆盖内容 |
|---|---|---|
| `ckit_test_ccia` | [`ccia/test_ccia.cpp`](ccia/test_ccia.cpp) | 互斥栈、双互斥队列、查找表、链表并发读写；4 种无锁栈与 2 种无锁队列 4P/4C 压测；细粒度队列 move-only 载荷。 |
| `ckit_test_type_erasure` | [`type_erasure/test_type_erasure.cpp`](type_erasure/test_type_erasure.cpp) | 4 种 `MyFunction` 变体：空调用抛 `bad_function_call`、lambda / 函数指针 / 成员指针 / mutable lambda、`void` 返回、拷贝/移动语义、SBO 与堆路径切换、`target<T>()` 命中与失配。 |
| `ckit_test_pmr` | [`pmr/test_pmr.cpp`](pmr/test_pmr.cpp) | 默认 resource 切换、null resource、monotonic 对齐与 `release()`、pool 复用与上游分配计数守恒、8 线程 `synchronized_pool_resource` 及跨线程释放、`polymorphic_allocator`（`vector`、`new_object`、`allocate_bytes`）。 |
| `ckit_test_spsc` | [`spsc/test_spsc.cpp`](spsc/test_spsc.cpp) | 容量边界、FIFO 顺序、环形回绕、批量 push/pop、`consume_all`、`frontPtr`，以及 3 种实现各 20 万项 1P/1C 并发校验。 |
| `ckit_test_mpmc` | [`mpmc/test_mpmc.cpp`](mpmc/test_mpmc.cpp) | 空/满边界、单线程 FIFO、3 种实现各 4P/4C（每生产者 2 万项）完整性与去重检测。 |
| `ckit_test_chase_lev` | [`chase_lev/test_chase_lev.cpp`](chase_lev/test_chase_lev.cpp) | owner 端 LIFO pop、thief 端 FIFO steal、动态扩容、空/满边界及 owner + 3 thief 不丢不重验证（支持 `-DCKIT_CHASE_N=...` 调整规模）。 |
| `ckit_test_ebr` | [`reclamation/test_ebr.cpp`](reclamation/test_ebr.cpp) | Treiber 栈 + `ebr::guard_ptr`：受保护 pop、`reclaim()`、4 线程混合压测、纪元推进后残留对象有界性校验。 |
| `ckit_test_hp` | [`reclamation/test_hp.cpp`](reclamation/test_hp.cpp) | 同款 Treiber 栈切换 `hp::hazard_pointer<>` / `hp::concurrent_ptr` / `hp::guard_ptr` 并发压测。 |
| `ckit_test_hp_v2` | [`reclamation/test_hp_v2.cpp`](reclamation/test_hp_v2.cpp) | 偏特化 HP 实现（裸 `std::atomic<T*>` + `deletable_obj_with_new_delete`）同等栈压测。 |
| `ckit_test_qsbr_xenium` | [`reclamation/test_qsbr_xenium.cpp`](reclamation/test_qsbr_xenium.cpp) | xenium 风格 QSBR guard 与 `region_guard`、4 线程混合压测及静默态发布校验。 |
| `ckit_test_qsbr_liburcu` | [`reclamation/test_qsbr_liburcu.cpp`](reclamation/test_qsbr_liburcu.cpp) | liburcu 风格 `rcu_register_thread` / `rcu_read_lock` / `rcu_quiescent_state`、`call_rcu` + `rcu_barrier`、4 并发读者围绕 `synchronize_rcu()` 宽限期校验。 |
| `ckit_test_executor` | [`threadpool/test_executor.cpp`](threadpool/test_executor.cpp) | 链式图、菱形图、随机 DAG、2 万平坦任务、1500 路 spill 溢出、重复/并发 `run`、嵌套 `corun`、休眠/唤醒、析构顺序及多 Executor 共存。 |

> **关于 SMR 泄漏检测说明：** 内存回收测试断言绝大部分 retired节点已及时回收，同时允许有界数量的对象滞留至下一纪元（由 SMR 延迟释放语义决定），不强求退出瞬间 `alive == 0`。因此 CTest 为相关测试配置了 `ASAN_OPTIONS=detect_leaks=0`。

## Sanitizer 动态检测

```bash
# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=thread -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=address,undefined -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

为消除 TSan 误报并确保严格满足其 happens-before 建模，在保持原算法语义不变的前提下进行了四处内存序/布局加固：

* 为 16 字节 `counted_node_ptr` 补充显式零初始化 padding，避免 `std::atomic<T>` 整对象表示比较时引入未定义字节；
* split-ref 栈及两个无锁队列在释放最后一个引用计数的 RMW 操作上使用 `acq_rel`；
* `hp_v2` 在发布风险指针后使用 `acquire` 语义重读被保护指针；
* BoundedWSQ 槽位发布改用 `release` store，替代 TSan 无法建模为 release 边的 `release fence + relaxed store`。

## GenMC 协议模型检验

[`reclamation/genmc`](reclamation/genmc) 与 [`threadpool/genmc`](threadpool/genmc) 目录包含 7 个自包含并发协议模型，用于通过 [GenMC](https://plv.mpi-sws.org/genmc/) 0.10 进行无状态模型检验，验证核心同步协议在弱内存序下的正确性（运行时实现仍由单元测试与 Sanitizer 覆盖，两者互补）。

```bash
bash tests/genmc/run_all.sh
# 或运行单个模型
genmc -- tests/reclamation/genmc/ebr_model.cpp
```

全部 7 个模型均内置**反向变异开关**（注入内存序降级或校验缺失缺陷），用于验证模型本身具备检错能力：

| 模型文件 | 必须触发 GenMC 报错的变异宏 |
|---|---|
| `ebr_model.cpp` | `-DBUG_SCAN`（scan 忽略 `local_epoch`，导致读者仍持有旧纪元时提前释放节点） |
| `hp_model.cpp` | `-DBUG_NO_VALIDATE`、`-DBUG_NO_SCAN`、`-DBUG_NO_READER_FENCE`、`-DBUG_NO_WRITER_FENCE` |
| `ccia_stack_model.cpp` | `-DBUG_NO_VALIDATE`、`-DBUG_NO_SCAN`、`-DBUG_SCAN_NO_FENCE`（`-DFIX_ACQUIRE_FENCE` 为修复对照组） |
| `qsbr_liburcu_model.cpp` | `-DSCAN_RELAXED`、`-DQS_STORE_RELAXED` |
| `qsbr_xenium_model.cpp` | `-DBUG_NO_OLD_EPOCH_CHECK` |
| `notifier_model.cpp` | `-DWAITER_NO_FENCE`、`-DNOTIFIER_NO_FENCE` |
| `inflight_model.cpp` | `-DADD_AFTER_SUB` |

验证结果：7 个基准模型全部输出 `Verification complete. No errors were detected`；上述 14 个缺陷变异宏全部被 GenMC 检出并报告反例。

## 扩展测试

在对应模块目录下新增 `test_<topic>.cpp`（`test_utils.hpp` 已纳入默认 include path），并在 [`CMakeLists.txt`](CMakeLists.txt) 中注册：

```cmake
ckit_add_test(ckit_test_<topic> <dir>/test_<topic>.cpp)
```