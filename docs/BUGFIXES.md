# 缺陷分析与修复记录

本文档记录在整合 header-only 库及运行 **GCC / Clang 双编译器构建、ASan / UBSan / TSan 动态检测与 GenMC 模型检验** 过程中暴露的缺陷、根因定位、修复方案及对应回归测试。

目录：

* [A. 正确性、崩溃与编译缺陷](#a-正确性崩溃与编译缺陷)
* [B. TSan 数据竞争加固（算法不变，加强内存序）](#b-tsan-数据竞争加固算法不变加强内存序)
* [C. Header-only 封装与 API 一致性修复](#c-header-only-封装与-api-一致性修复)
* [D. 已知设计约束与边界说明](#d-已知设计约束与边界说明)

---

## A. 正确性、崩溃与编译缺陷

### A1. PMR：`synchronized_pool_resource::thread_exit` 销毁后触发无条件断言

**涉及文件：** `include/ckit/pmr/pmr_teaching.hpp`

**现象：** 线程首次使用 `synchronized_pool_resource` 后退出时，`tls_guard` 调用 `thread_exit()`；即使已成功查找到并销毁 `tp`，函数末尾仍执行 `assert(false && "thread_exit: tp not found ...")` 导致进程中止。

**根因：** 命中目标节点后仅 `break` 跳出循环而非返回，误触循环外的错误断言。

```cpp
  void thread_exit(detail::thread_pools* tp) {
    if (tp == nullptr || tp == shared_) return;
    for (detail::thread_pools** link = &detail::tls_head; *link != nullptr;
         link = &(*link)->thread_next) {
      if ((*link) == tp) {
        *link = (*link)->thread_next;
        std::unique_lock lock(mutex_);
        destroy_thread_pools_locked(tp, true);
-       break;
+       return;
      }
    }
    assert(false && "thread_exit: tp not found in this thread's tls_head");
  }
```

**回归测试：** `tests/pmr/test_pmr.cpp`（`test_synchronized_pool`：8 线程并发分配、跨线程释放及 `release()` 后上游计数归零校验）。

---

### A2. liburcu QSBR：`thread_offline()` 误清 `registered` 标志

**涉及文件：** `include/ckit/reclamation/qsbr/liburcu_qsbr.hpp`

**现象：**
1. 连续调用 `rcu_thread_offline()` 与 `rcu_thread_online()` 时，`thread_online()` 触发 `assert(tls_reader.registered)`；
2. 先 `rcu_thread_offline()` 再 `rcu_unregister_thread()` 触发相同断言；
3. `rcu_barrier()` 内部执行 offline/online 切换时同样失败。

**根因：** 将“线程已注册在链表中（`registered`）”与“线程处于离线态（`ctr == 0`）”耦合，在 `thread_offline()` 中错误清除了 `registered`。

```cpp
  inline void domain::thread_offline() {
    assert(detail::tls_reader.registered);
-   detail::tls_reader.registered = false;
    detail::tls_reader.ctr.store(0, std::memory_order_release);
    notify_quiescent();
  }

  inline void domain::unregister_thread() {
    assert(detail::tls_reader.registered);
    thread_offline();
    std::lock_guard<std::mutex> lock(registry_mutex_);
    ...
    detail::tls_reader.prev = nullptr;
    detail::tls_reader.next = nullptr;
+   detail::tls_reader.registered = false;
  }
```

**回归测试：** `tests/reclamation/test_qsbr_liburcu.cpp`（`test_single_thread`、`test_readers`）。

---

### A3. 16 字节 `std::atomic<counted_node_ptr>` 未初始化 padding 导致 CAS 伪失败

**涉及文件：**
* `include/ckit/ccia/lock_free_queue_half.hpp`
* `include/ckit/ccia/lock_free_queue_helping.hpp`
* `include/ckit/ccia/lock_free_stack_split_ref_count.hpp`

**现象：**
* `lock_free_queue_helping` 在 4P/4C 下概率性触发 SIGSEGV：`push_impl()` 中对空指针 `old_tail.ptr->data` 解引用（`tail` 被覆写为 `{0, nullptr}`）；
* `lock_free_queue_half` 与 `lock_free_stack_split_ref_count` 在 TSan 下暴露 `operator delete` 与原子计数的竞争；
* 部分平台上 16 字节 CAS 在逻辑值相等时持续失败。

**根因：** x86-64 下 `counted_node_ptr { int external_count; node* ptr; }` 占 16 字节，其中包含 4 字节隐式 padding。`std::atomic<T>::compare_exchange_*` 按完整对象表示（含 padding）逐位比较：未初始化或残留脏数据的 padding 会导致逻辑相等的两个值比较失败；在 `helping` 队列的失败回写路径中，`old_next` 可能携带空指针被 `set_new_tail` 写入 `tail`。

**修复方案：** 显式填充 4 字节成员使结构体无隐式 padding，并在所有构造函数中零初始化：

```cpp
struct counted_node_ptr {
  int external_count;
  int padding;
  node* ptr;

  counted_node_ptr() noexcept
      : external_count(0), padding(0), ptr(nullptr) {}
  counted_node_ptr(int count, node* p) noexcept
      : external_count(count), padding(0), ptr(p) {}
};
```

> 注：保留双参构造函数而非默认成员初始化的聚合体，确保原有 `{0, nullptr}` 列表初始化语义不发生字段错位。

**回归测试：** `tests/ccia/test_ccia.cpp`（4P/4C 并发压测，ASan 与 TSan 双清零）。

---

### A4. move-only `MyFunction`：`Model::move()` 非法访问外层成员

**涉及文件：** `include/ckit/type_erasure/function_move_only.hpp`

**现象：** 实例化移动构造或移动赋值时编译失败：`invalid use of non-static data member 'MyFunction<...>::pImpl_'`。

**根因：** `pImpl_` 属于外层 `MyFunction` 而非内嵌 `Model<F>`。对于堆存储的 move-only 版本，直接转移 `std::unique_ptr<Concept>` 即可，无需虚函数 `move()`（SBO 版本因需在缓冲区之间迁移对象，仍保留 `move(void* dst)`）。

```cpp
struct Concept {
  virtual ~Concept() = default;
  virtual R invoke(Args&&... args) const = 0;
};

MyFunction(MyFunction&& other) noexcept
    : pImpl_(std::move(other.pImpl_)) {}

MyFunction& operator=(MyFunction&& other) noexcept {
  if (this != &other) pImpl_ = std::move(other.pImpl_);
  return *this;
}
```

**回归测试：** `tests/type_erasure/test_type_erasure.cpp`（`test_move_only`）。

---

### A5. `MyFunction::operator()` 仅接受右值参数

**涉及文件：** `include/ckit/type_erasure/function_*.hpp`（全部 4 个变体）

**现象：** 对 `MyFunction<int(int)>` 传入左值实参 `f(x)` 时编译报错 `cannot bind rvalue reference 'int&&' to lvalue 'int'`。

**根因：** 类模板参数 `Args...` 来自签名 `R(Args...)`，在 `operator()(Args&&... args)` 中不构成转发引用；当 `Args` 为值类型 `int` 时，形参退化为右值引用 `int&&`。

**修复方案：** 对齐 `std::function`，按签名原样接收 `Args...` 后再转发给虚函数 `invoke`：

```cpp
- R operator()(Args&&... args) const
+ R operator()(Args... args) const
  {
      if (!pImpl_) throw std::bad_function_call();
      return pImpl_->invoke(std::forward<Args>(args)...);
  }
```

**回归测试：** `tests/type_erasure/test_type_erasure.cpp`（`test_basic` 与左值对象调用 `test_member_pointer`）。

---

### A6. `target<T>()` 在 Clang 下查询不可调用类型触发硬错误

**涉及文件：**
* `include/ckit/type_erasure/function_copyable_sbo_rtti.hpp`
* `include/ckit/type_erasure/function_move_only_sbo_rtti.hpp`

**现象：** 调用 `f.target<int>()` 时，Clang 报错 `no matching function for call to 'invoke'`（源于实例化 `Model<int>::invoke`）。

**根因：** 尽管运行期类型 token 校验会提前返回 `nullptr`，但 `static_cast<Model<T>*>` 要求完整类型，迫使编译器无条件实例化 `Model<T>`。

**修复方案：** 使用 `if constexpr` 约束向下转换分支：

```cpp
template <class T>
T* target() noexcept {
  if (!has_value() || get()->address_token() != token_of<T>())
    return nullptr;
  if constexpr (std::is_invocable_r_v<R, T, Args...>)
    return &static_cast<Model<T>*>(get())->f_;
  else
    return nullptr;
}
```

**回归测试：** `tests/type_erasure/test_type_erasure.cpp`（`test_rtti` 中显式断言 `f.target<int>() == nullptr`，GCC / Clang 双编译验证）。

---

### A7. PMR：`ResourceAllocator` 缺失 `size_type` 定义

**涉及文件：** `include/ckit/pmr/pmr_teaching.hpp`

**现象：** 实例化 `ResourceAllocator::allocate()` 时报错 `'size_type' does not name a type`。

**修复方案：** 在 `ResourceAllocator` 中补充 `using size_type = std::size_t;`。

**回归测试：** `tests/pmr/test_pmr.cpp`（`PoolArray` 实例化路径）。

---

### A8. GCC 11 libstdc++ 缺失 C++20 `std::atomic<std::shared_ptr<T>>`

**涉及文件：** `include/ckit/ccia/lock_free_stack_atomic_shared_ptr.hpp`

**现象：** GCC 11.4 未定义 `__cpp_lib_atomic_shared_ptr`，`std::atomic<std::shared_ptr<node>>` 回退至主模板并触发 `trivially copyable` 静态断言失败。

**修复方案：** 在 `detail::atomic_shared_ptr` 中基于 `__cpp_lib_atomic_shared_ptr` 提供条件封装：支持标准特化时直接委托 `std::atomic<std::shared_ptr<T>>`，否则桥接至 C++11 `std::atomic_load/store/compare_exchange_*` 自由函数。

**回归测试：** `tests/ccia/test_ccia.cpp`（`lock_free_stack_atomic_shared_ptr` 4P/4C 压测）。

---

### A9. PMR：Clang + libstdc++ 下 sized aligned delete 不可用

**涉及文件：** `include/ckit/pmr/pmr_teaching.hpp`

**现象：** Clang 14 + libstdc++ 构建报找不到匹配的 `::operator delete(p, bytes, std::align_val_t(alignment))`。

**修复方案：** 改为不带 `bytes` 的标准对齐释放接口：

```cpp
- ::operator delete(p, bytes, std::align_val_t(alignment));
+ ::operator delete(p, std::align_val_t(alignment));
```

**回归测试：** Clang 14 构建并运行 `tests/pmr/test_pmr.cpp`。

---

## B. TSan 数据竞争加固（算法不变，加强内存序）

以下问题源于 TSan 的 happens-before 模型无法追踪独立 `atomic_thread_fence` 或隐式修改序，而非原算法逻辑缺陷。修复均为严格加强内存序，不改变算法语义。

### B1. `hp_v2`：风险指针发布后的二次校验使用 `relaxed`

**涉及文件：** `include/ckit/reclamation/hp/hp_v2.hpp`

**现象：** TSan 报告 `node::next` 的并发读写竞争，因 `set_hp()` 后的 `seq_cst` fence 在 TSan 中不建立同步边。

**修复方案：** 将 `acquire()` 与 `acquire_if_equal()` 中的第二次重读由 `memory_order_relaxed` 加强为 `memory_order_acquire`（与 `hp.hpp` 保持一致）。

---

### B2. Taskflow `BoundedWSQ`：`release fence + relaxed store` 未被 TSan 建模为 release 边

**涉及文件：** `include/ckit/chase_lev/chase_taskflow.hpp`

**现象：** `Executor` 在 TSan 下报告 `Node::work` 构造与窃取读取之间存在数据竞争。

**修复方案：** 将 `_bottom` 发布直接改为 `memory_order_release` store，使 steal 侧的 `acquire` load 显式建立 happens-before 边：

```cpp
_buffer[b & BufferMask].store(std::forward<O>(o), std::memory_order_relaxed);
-_bottom.store(b + 1, std::memory_order_relaxed);
+_bottom.store(b + 1, std::memory_order_release);
```

**回归测试：** `tests/threadpool/test_executor.cpp`（TSan 全部通过）。

---

### B3. 引用计数交接路径使用 `relaxed` RMW

**涉及文件：**
* `include/ckit/ccia/lock_free_stack_split_ref_count.hpp`
* `include/ckit/ccia/lock_free_queue_half.hpp`
* `include/ckit/ccia/lock_free_queue_helping.hpp`

**现象：** TSan 报告节点 `operator delete` 与其他线程对 `internal_count` / `external_count` 的原子写竞争。

**修复方案：** 将决定“最后一个引用者执行释放”的 RMW 操作加强为 `memory_order_acq_rel`（CAS 失败路径使用 `memory_order_acquire`）：

```cpp
// split-ref 栈
ptr->internal_count.fetch_add(count_increase, std::memory_order_acq_rel);
ptr->internal_count.fetch_sub(1, std::memory_order_acq_rel);

// half / helping 队列
count.compare_exchange_strong(old_counter, new_counter,
                              std::memory_order_acq_rel,
                              std::memory_order_acquire);
ptr->count.compare_exchange_strong(old_counter, new_counter,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire);
```

---

## C. Header-only 封装与 API 一致性修复

### C1. `half` 与 `helping` 无锁队列符号冲突

**涉及文件：** `include/ckit/ccia/lock_free_queue_half.hpp`、`include/ckit/ccia/lock_free_queue_helping.hpp`

**现象：** 两者均在全局作用域定义了 `node_counter` 与 `g_sentinel_object`，无法在同一翻译单元内同时包含。

**修复方案：** 拆分为独立命名符号 `node_counter_half` / `node_counter_helping` 与 `g_sentinel_half` / `g_sentinel_helping`。

---

### C2. 多变体 `MyFunction` 全局命名空间冲突

**涉及文件：** `include/ckit/type_erasure/function_*.hpp`

**现象：** 各变体共用全局类名 `MyFunction` 及辅助模板 `token_of`，同翻译单元包含导致重定义。

**修复方案：** 合并为 4 个独立头文件并分别隔离至独立命名空间：
* `ckit::te::copyable`
* `ckit::te::copyable_sbo_rtti`
* `ckit::te::move_only`
* `ckit::te::move_only_sbo_rtti`

---

### C3. `hp.hpp` 与 `hp_v2.hpp` 接口差异梳理

**涉及文件：** `include/ckit/reclamation/hp/hp.hpp`、`include/ckit/reclamation/hp/hp_v2.hpp`

两套实现面向相同算法但 API 形态不同，不可在同一翻译单元混用，已在文档及测试（`test_hp` / `test_hp_v2`）中显式隔离：

| `hp.hpp` | `hp_v2.hpp` |
|---|---|
| `hp::enable_concurrent_ptr<T>` | `hp::deletable_obj_with_new_delete<T>` |
| `hp::concurrent_ptr<T, Reclaimer>` | 直接操作裸 `std::atomic<T*>` |
| `hp::hazard_pointer<>` | `hp::hazard_pointer<hp::strategy<3,2,100,false>>` |
| `guard.acquire(concurrent_ptr)` | `guard.acquire(std::atomic<T*>&)` |

---

### C4. 头文件内 `static` 哨兵变量的每 TU 副本问题

**涉及文件：** `include/ckit/ccia/lock_free_queue_half.hpp`、`include/ckit/ccia/lock_free_queue_helping.hpp`

**现象：** 头文件中声明 `static unsigned char g_sentinel_object[64]` 会导致每个翻译单元生成独立地址副本，触发 `-Wunused-variable` 告警并存在跨 TU 比较隐患。

**修复方案：** 改为 C++17 `inline` 变量，确保全程序唯一链接地址。

---

## D. 已知设计约束与边界说明

以下行为属于上游算法或标准设计的既定边界，保留原语义并在使用约束中明示：

1. **`threadsafe_queue::wait_and_pop(T&)` 异常安全边界：** 出队后在持锁状态下执行移动赋值；若 `T::operator=` 抛出异常，已弹出的元素将丢失。需保证 `T` 具备 `noexcept` 移动赋值，或改用返回 `std::shared_ptr<T>` 的重载。
2. **`threadsafe_lookup_table::get_map()` 读锁并发：** 采用 `std::shared_lock` 替代原书 `unique_lock` 以支持多读者并发快照（`std::vector<std::shared_lock>` 扩容依赖移动语义，安全可用）。
3. **16 字节原子链接依赖：** x86-64 下部分平台链接 16 字节 `std::atomic`（如 split-ref 栈）需要 `-latomic`，不保证硬件无锁（lock-free）。
4. **编译期类型约束：** `chase::paper` 要求可平凡复制，`mpmc::rigtorp` 要求 `noexcept` 移动/拷贝赋值与析构，`mpmc::xenium` 要求 `noexcept` 移动构造，均已通过 `static_assert` 强制检查。
5. **`data_to_reclaim` 擦除开销：** 每个待回收节点存储 `std::function<void(void*)>` 以忠实对照原书实现；纯 `delete` 场景可替换为函数指针 `void(*)(void*)` 以消除额外体积与分配开销。
6. **风险指针槽位上限：** `ccia` 风险指针实现沿用原书进程级 100 槽位固定表设计，超出并发线程上限将抛出 `std::runtime_error`。

---

## 回归验证命令

```bash
# Release 正确性（GCC / Clang）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && ctest --test-dir build --output-on-failure

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=address,undefined -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# TSan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCKIT_SANITIZER=thread -DCKIT_BUILD_BENCHMARKS=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure

# GenMC 协议模型
bash tests/genmc/run_all.sh
```

### 问题索引总表

| 编号 | 对应仓库文件 |
|---|---|
| A1 / A7 / A9 | `include/ckit/pmr/pmr_teaching.hpp` |
| A2 | `include/ckit/reclamation/qsbr/liburcu_qsbr.hpp` |
| A3 / B3 | `include/ckit/ccia/lock_free_queue_half.hpp`、`lock_free_queue_helping.hpp`、`lock_free_stack_split_ref_count.hpp` |
| A4 / A5 / A6 / C2 | `include/ckit/type_erasure/function_*.hpp` |
| A8 | `include/ckit/ccia/lock_free_stack_atomic_shared_ptr.hpp` |
| B1 | `include/ckit/reclamation/hp/hp_v2.hpp` |
| B2 | `include/ckit/chase_lev/chase_taskflow.hpp` |
| C1 / C4 | `include/ckit/ccia/lock_free_queue_half.hpp`、`lock_free_queue_helping.hpp` |
| C3 | `include/ckit/reclamation/hp/hp.hpp`、`hp_v2.hpp` |