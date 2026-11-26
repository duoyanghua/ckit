# 许可证与上游来源

本仓库采用**混合许可证**，各头文件首行均标注对应的 SPDX 许可证标识符或上游来源说明。下表列出各目录及核心文件的许可证归属与参考来源。

| 路径 | SPDX / 许可状态 | 上游来源 |
|---|---|---|
| `include/ckit/ccia/` | 未声明许可证（基于原著示例改编） | A. Williams, *C++ Concurrency in Action*, 2nd ed., Manning |
| `include/ckit/type_erasure/` | MIT | 原创实现 |
| `include/ckit/pmr/` | GPL-3.0-or-later WITH GCC-exception-3.1 | libstdc++ `<memory_resource>`（FSF） |
| `include/ckit/spsc/spsc_rigtorp.hpp` | MIT | https://github.com/rigtorp/SPSCQueue |
| `include/ckit/spsc/spsc_boost.hpp` | BSL-1.0 | https://github.com/boostorg/lockfree（Boost 1.85） |
| `include/ckit/spsc/spsc_folly.hpp` | Apache-2.0 | https://github.com/facebook/folly（`ProducerConsumerQueue.h`） |
| `include/ckit/spsc/spsc_config.hpp` | MIT | 原创实现 |
| `include/ckit/mpmc/mpmc_rigtorp.hpp` | MIT | https://github.com/rigtorp/MPMCQueue |
| `include/ckit/mpmc/mpmc_vyukov.hpp` | BSD-2-Clause | D. Vyukov，有界 MPMC 队列（1024cores） |
| `include/ckit/mpmc/mpmc_xenium.hpp` | MIT | https://github.com/mpoeter/xenium |
| `include/ckit/mpmc/mpmc_config.hpp` | MIT | 原创实现 |
| `include/ckit/chase_lev/chase_paper.hpp` | 未声明许可证（依据论文 Figure 1 实现） | Lê, Pop, Cohen, Zappa Nardelli, PPoPP 2013 |
| `include/ckit/chase_lev/chase_taskflow.hpp` | MIT | https://github.com/taskflow/work-stealing-queue |
| `include/ckit/chase_lev/chase_charlietsao.hpp` | MIT | https://github.com/charlietsao/work-stealing-queue |
| `include/ckit/chase_lev/chase_xenium.hpp` | MIT | https://github.com/mpoeter/xenium |
| `include/ckit/chase_lev/chase_config.hpp` | MIT | 原创实现 |
| `include/ckit/reclamation/ebr/` | MIT | https://github.com/mpoeter/xenium（`generic_epoch_based`） |
| `include/ckit/reclamation/hp/` | MIT | https://github.com/mpoeter/xenium（`hazard_pointer`） |
| `include/ckit/reclamation/qsbr/xenium_qsbr.hpp` | MIT | https://github.com/mpoeter/xenium（`quiescent_state_based`） |
| `include/ckit/reclamation/qsbr/liburcu_qsbr.hpp` | LGPL-2.1-or-later | https://github.com/urcu/userspace-rcu |
| `include/ckit/threadpool/` | MIT | https://github.com/taskflow/taskflow（v4.1.0 executor） |
| `tests/`、`benchmarks/` | MIT | 原创测试与微基准（GenMC 模型沿用对应模块协议） |

## 许可边界说明

* **受版权保护的教材与论文实现：** `include/ckit/ccia/` 与 `include/ckit/chase_lev/chase_paper.hpp` 分别改编自技术专著与学术论文，未附带开源许可证声明，仅作机制研究与性能对照用途。
* **强/弱 Copyleft 模块：** `include/ckit/pmr/` 派生自 libstdc++，遵循 `GPL-3.0-or-later WITH GCC-exception-3.1`；`include/ckit/reclamation/qsbr/liburcu_qsbr.hpp` 派生自 userspace-rcu，遵循 `LGPL-2.1-or-later`。二次分发或整体重授权时须遵守相应条款并保留原始版权声明。
* **宽松许可模块：** 其余标注 MIT / BSD-2-Clause / Apache-2.0 / BSL-1.0 的文件均保留上游协议约束，文件首行 SPDX 标识为对应许可证的简写引用。