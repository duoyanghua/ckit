// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <ckit/chase_lev/chase_taskflow.hpp>

namespace tp {

inline constexpr std::size_t kCacheLineSize = 64;

class Executor;
class Taskflow;
class Topology;
class Worker;
struct Node;

class Notifier {

public:

  struct Waiter {
    alignas(2 * kCacheLineSize) std::uint32_t epoch {0};
  };
private:
  static constexpr std::uint64_t kWaiterInc  {1};
  static constexpr std::uint64_t kEpochShift {32};
  static constexpr std::uint64_t kEpochInc   {std::uint64_t{1} << kEpochShift};
  static constexpr std::uint64_t kWaiterMask {kEpochInc - 1};
  std::atomic<std::uint64_t> _state;
  std::vector<Waiter> _waiters;

public:
  explicit Notifier(std::size_t n) : _state(0), _waiters(n) {}

  ~Notifier() {
    assert((_state.load(std::memory_order_relaxed) & kWaiterMask) == 0);
  }
  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;

  void notify_one() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (std::uint64_t s = _state.load(std::memory_order_relaxed); s & kWaiterMask;) {
      if (_state.compare_exchange_weak(s, s + kEpochInc, std::memory_order_relaxed)) {
        _state.notify_one();
        break;
      }
    }
  }
  void notify_all() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (std::uint64_t s = _state.load(std::memory_order_relaxed); s & kWaiterMask;) {
      if (_state.compare_exchange_weak(s, s + kEpochInc, std::memory_order_relaxed)) {
        _state.notify_all();
        break;
      }
    }
  }
  void notify_n(std::size_t n) noexcept {
    if (n >= _waiters.size()) {
      notify_all();
    } else {
      for (std::size_t k = 0; k < n; ++k) notify_one();
    }
  }

  void prepare_wait(std::size_t w) noexcept {
    const std::uint64_t prev = _state.fetch_add(kWaiterInc, std::memory_order_relaxed);
    _waiters[w].epoch = static_cast<std::uint32_t>(prev >> kEpochShift);
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  void cancel_wait(std::size_t) noexcept {
    _state.fetch_sub(kWaiterInc, std::memory_order_relaxed);
  }

  void commit_wait(std::size_t w) noexcept {
    std::uint64_t prev = _state.load(std::memory_order_relaxed);
    while ((prev >> kEpochShift) == _waiters[w].epoch) {
      _state.wait(prev, std::memory_order_relaxed);
      prev = _state.load(std::memory_order_relaxed);
    }
    _state.fetch_sub(kWaiterInc, std::memory_order_relaxed);
  }

  std::size_t size() const noexcept { return _waiters.size(); }

  std::size_t num_waiters() const noexcept {
    return static_cast<std::size_t>(_state.load(std::memory_order_relaxed) & kWaiterMask);
  }
};

struct Node {
  std::function<void()> work;
  std::vector<Node*> successors;
  std::size_t index {0};
  std::size_t num_predecessors {0};
  std::atomic<std::size_t> join_counter {0};
  std::atomic<std::size_t>* parent_counter {nullptr};
  Topology* topology {nullptr};

};

class Task {
private:
  friend class Taskflow;
  Node* _node {nullptr};
private:
  explicit Task(Node* n) : _node(n) {}
public:
  Task() = default;
  Node* node() const noexcept { return _node; }

  Task& precede(Task t) {
    _node->successors.push_back(t._node);
    return *this;
  }

  Task& succeed(Task t) {
    t._node->successors.push_back(_node);
    return *this;
  }
};

class Topology {
private:
  friend class Executor;
  Taskflow& _taskflow;
  std::atomic<std::size_t> _join_counter {0};
  std::atomic<bool> _done {false};
public:
  explicit Topology(Taskflow& f) : _taskflow(f) {}

  void wait() {
    if (!_done.load(std::memory_order_acquire)) {
      _done.wait(false, std::memory_order_acquire);
    }
  }
};

class Future {
private:
  std::shared_ptr<Topology> _t;
public:
  Future() = default;
  explicit Future(std::shared_ptr<Topology> t) : _t(std::move(t)) {}

  void wait() const {
    if (_t) _t->wait();
  }
  bool valid() const noexcept { return static_cast<bool>(_t); }
};

class Taskflow {
private:
  friend class Executor;
  std::deque<Node> _nodes;
  std::vector<Node*> _graph;
  std::size_t _num_sources {0};
  bool _dirty {true};

  std::mutex _mutex;
  std::deque<std::shared_ptr<Topology>> _topologies;

private:

  void _ensure_graph() {
    if (!_dirty) return;
    _graph.clear();
    _num_sources = 0;
    std::vector<std::size_t> indeg(_nodes.size(), 0);
    for (const Node& n : _nodes) {
      for (const Node* s : n.successors) indeg[s->index]++;
    }
    std::deque<std::size_t> q;
    for (Node& n : _nodes) {
      n.num_predecessors = indeg[n.index];
      if (indeg[n.index] == 0) q.push_back(n.index);
    }
    _num_sources = q.size();
    while (!q.empty()) {
      const std::size_t i = q.front();
      q.pop_front();
      Node& n = _nodes[i];
      _graph.push_back(&n);
      for (Node* s : n.successors) {
        if (--indeg[s->index] == 0) q.push_back(s->index);
      }
    }
    if (_graph.size() != _nodes.size()) {
      throw std::runtime_error("tp::Taskflow: cycle detected (learning version requires a DAG)");
    }
    _dirty = false;
  }

  std::size_t _fetch_enqueue(std::shared_ptr<Topology> t) {
    std::lock_guard<std::mutex> lock(_mutex);
    const std::size_t n = _topologies.size();
    _topologies.push_back(std::move(t));
    return n;
  }

public:
  Taskflow() = default;
  Taskflow(const Taskflow&) = delete;
  Taskflow& operator=(const Taskflow&) = delete;

  template <typename F>
  Task emplace(F&& f) {
    _nodes.emplace_back();
    Node& n = _nodes.back();
    n.index = _nodes.size() - 1;
    n.work = std::forward<F>(f);
    _dirty = true;
    return Task{&n};
  }

  bool empty() const noexcept { return _nodes.empty(); }
  std::size_t num_nodes() const noexcept { return _nodes.size(); }
};

struct Xorshift {
  std::uint32_t state {2463534242u};

  void seed(std::uint32_t s) {
    state = s ? s : 2463534242u;
  }
  std::uint32_t operator()() noexcept {
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state = x;
  }
};

class Worker {
private:
  friend class Executor;
  using wsq_type = chase::taskflow::BoundedWSQ<Node*>;

  alignas(kCacheLineSize) std::atomic_flag _done = ATOMIC_FLAG_INIT;
  std::size_t _id {0};
  std::size_t _sticky_victim {0};
  Xorshift _rdgen;
  std::thread _thread;
  wsq_type _wsq;

public:
  Worker() = default;
  std::size_t id() const noexcept { return _id; }
  std::size_t queue_size() const noexcept { return _wsq.size(); }
  std::size_t queue_capacity() const noexcept { return _wsq.capacity(); }
};

class Executor {

  struct Buffer {
    std::mutex mutex;
    chase::taskflow::UnboundedWSQ<Node*> queue;
  };
  inline static thread_local Executor* _tls_executor = nullptr;
  inline static thread_local Worker* _tls_worker = nullptr;

  std::vector<Worker> _workers;
  std::vector<Buffer> _buffers;

  alignas(kCacheLineSize) Notifier _notifier;
  alignas(kCacheLineSize) std::atomic<std::size_t> _num_topologies {0};

public:
  explicit Executor(std::size_t N = std::thread::hardware_concurrency());
  ~Executor();

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  Future run(Taskflow& f);

  void wait_for_all();

  void corun(Taskflow& f);

  std::size_t num_workers() const noexcept { return _workers.size(); }
  std::size_t num_waiters() noexcept { return _notifier.num_waiters(); }
  std::size_t num_queues() const noexcept { return _workers.size() + _buffers.size(); }
  std::size_t num_topologies() const noexcept {
    return _num_topologies.load(std::memory_order_relaxed);
  }

  Worker* this_worker() noexcept;
  int this_worker_id() noexcept;

private:
  void _shutdown();
  void _spawn(std::size_t N);

  void _exploit_task(Worker&, Node*&);
  bool _explore_task(Worker&, Node*&);
  bool _wait_for_task(Worker&, Node*&);

  void _schedule(Worker&, Node*);
  void _bulk_schedule(Worker&, Node**, std::size_t);
  void _bulk_schedule(Node**, std::size_t);
  void _spill(Node*);
  void _bulk_spill(Node**, std::size_t);

  void _invoke(Worker&, Node*);

  void _set_up_topology(Worker*, Topology*);
  std::size_t _set_up_graph(Taskflow&, Topology*, std::atomic<std::size_t>*);
  void _tear_down_topology(Worker&, Topology*);
  void _increment_topology();
  void _decrement_topology();

  void _corun_graph(Worker&, Taskflow&);
  void _corun_until(Worker&, const std::function<bool()>&);
};

inline Executor::Executor(std::size_t N)
  : _workers(N),  _buffers(std::bit_width(N)),  _notifier(N) {
  if (N == 0) {
    throw std::invalid_argument("tp::Executor needs at least one worker");
  }
  _spawn(N);
}

inline Executor::~Executor() {
  _shutdown();
}

inline void Executor::_spawn(std::size_t N) {
  for (std::size_t id = 0; id < N; ++id) {
    _workers[id]._thread = std::thread([this, id] {
      Worker& w = _workers[id];
      w._id = id;
      w._sticky_victim = id;
      w._rdgen.seed(static_cast<std::uint32_t>(std::hash<std::thread::id>()(std::this_thread::get_id())));
      _tls_executor = this;
      _tls_worker = &w;
      Node* t = nullptr;
      for (;;) {
        _exploit_task(w, t);
        if (!_wait_for_task(w, t)) break;
      }
      _tls_worker = nullptr;
      _tls_executor = nullptr;
    });
  }
}

inline void Executor::_shutdown() {
  wait_for_all();
  for (auto& w : _workers) {
    w._done.test_and_set(std::memory_order_relaxed);
  }
  _notifier.notify_all();
  for (auto& w : _workers) {
    if (w._thread.joinable()) w._thread.join();
  }
}

inline Future Executor::run(Taskflow& f) {
  {
    std::lock_guard<std::mutex> lock(f._mutex);
    f._ensure_graph();
  }
  if (f.empty()) return Future{};
  _increment_topology();
  auto tpg = std::make_shared<Topology>(f);
  Future fu(tpg);
  if (f._fetch_enqueue(tpg) == 0) {
    _set_up_topology(this_worker(), tpg.get());
  }
  return fu;
}

inline Worker* Executor::this_worker() noexcept {
  return _tls_executor == this ? _tls_worker : nullptr;
}

inline int Executor::this_worker_id() noexcept {
  auto* w = this_worker();
  return w ? static_cast<int>(w->_id) : -1;
}

inline void Executor::_exploit_task(Worker& w, Node*& t) {
  while (t) {
    _invoke(w, t);
    t = w._wsq.pop();
  }
}

inline bool Executor::_explore_task(Worker& w, Node*& t) {
  if (_num_topologies.load(std::memory_order_relaxed) == 0) {
    return true;
  }
  const std::size_t MAX_VICTIM = num_queues();
  const std::size_t MAX_STEALS = ((MAX_VICTIM + 1) << 1);
  std::size_t num_steals = 0;
  std::size_t vtm = w._sticky_victim;
  while (true) {

    t = (vtm < _workers.size()) ? _workers[vtm]._wsq.steal() : _buffers[vtm - _workers.size()].queue.steal();
    if (t) {
      w._sticky_victim = vtm;
      break;
    }

    vtm = w._rdgen() % (MAX_VICTIM - 1);
    if (vtm >= w._id) vtm++;
    if (++num_steals > MAX_STEALS) {
      std::this_thread::yield();
      if (num_steals > 150 + MAX_STEALS) {
        break;
      }
    }
    if (w._done.test(std::memory_order_relaxed)) {
      return false;
    }
  }
  return true;
}

inline bool Executor::_wait_for_task(Worker& w, Node*& t) {
explore_task:
  if (_explore_task(w, t) == false) {
    return false;
  }
  if (t) {
    return true;
  }

  _notifier.prepare_wait(w._id);

  if (_num_topologies.load(std::memory_order_relaxed) == 0) {
    if (w._done.test(std::memory_order_relaxed)) {
      _notifier.cancel_wait(w._id);
      return false;
    }
    _notifier.commit_wait(w._id);
    goto explore_task;
  }

  for (std::size_t b = 0; b < _buffers.size(); ++b) {
    if (!_buffers[b].queue.empty()) {
      _notifier.cancel_wait(w._id);
      w._sticky_victim = b + _workers.size();
      goto explore_task;
    }
  }

  for (std::size_t k = 0; k + 1 < _workers.size(); ++k) {
    const std::size_t vtm = k + (k >= w._id);
    if (!_workers[vtm]._wsq.empty()) {
      _notifier.cancel_wait(w._id);
      w._sticky_victim = vtm;
      goto explore_task;
    }
  }

  if (w._done.test(std::memory_order_relaxed)) {
    _notifier.cancel_wait(w._id);
    return false;
  }
  _notifier.commit_wait(w._id);
  goto explore_task;
}

inline void Executor::_schedule(Worker& w, Node* node) {
  if (!w._wsq.try_push(node)) {
    _spill(node);
  }
  _notifier.notify_one();
}

inline void Executor::_spill(Node* node) {
  const std::size_t b = (reinterpret_cast<std::uintptr_t>(node) >> 16) % _buffers.size();
  std::lock_guard<std::mutex> lock(_buffers[b].mutex);
  _buffers[b].queue.push(node);
}

inline void Executor::_bulk_spill(Node** first, std::size_t n) {
  if (n == 0) return;
  const std::size_t b = ((reinterpret_cast<std::uintptr_t>(*first) * 2654435761ULL) >> 32) % _buffers.size();
  std::lock_guard<std::mutex> lock(_buffers[b].mutex);
  _buffers[b].queue.bulk_push(first, n);
}

inline void Executor::_bulk_schedule(Worker& w, Node** first, std::size_t n) {
  if (n == 0) return;
  const std::size_t pushed = w._wsq.try_bulk_push(first, n);
  if (pushed != n) {
    _bulk_spill(first + pushed, n - pushed);
  }
  _notifier.notify_n(n);
}

inline void Executor::_bulk_schedule(Node** first, std::size_t n) {
  if (n == 0) return;
  _bulk_spill(first, n);
  _notifier.notify_n(n);
}

inline void Executor::_invoke(Worker& w, Node* node) {
  Node* cache = nullptr;
  for (;;) {
    node->work();
    for (Node* s : node->successors) {

      if (s->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        node->parent_counter->fetch_add(1, std::memory_order_relaxed);
        if (cache != nullptr) {
          _schedule(w, cache);
        }
        cache = s;
      }
    }

    if (node->parent_counter->fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (node->topology != nullptr) {
        assert(cache == nullptr && "a cached successor must keep the topology alive");
        _tear_down_topology(w, node->topology);
      }

    }
    if (cache == nullptr) break;
    node = cache;
    cache = nullptr;
  }
}

inline void Executor::_increment_topology() {
  _num_topologies.fetch_add(1, std::memory_order_relaxed);
}
inline void Executor::_decrement_topology() {
  if (_num_topologies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    _num_topologies.notify_all();
  }
}
inline void Executor::wait_for_all() {
  std::size_t n = _num_topologies.load(std::memory_order_acquire);
  while (n != 0) {
    _num_topologies.wait(n, std::memory_order_acquire);
    n = _num_topologies.load(std::memory_order_acquire);
  }
}

inline std::size_t Executor::_set_up_graph(Taskflow& f, Topology* tpg, std::atomic<std::size_t>* parent) {
  f._ensure_graph();
  for (Node* n : f._graph) {
    n->topology = tpg;
    n->parent_counter = parent;
    n->join_counter.store(n->num_predecessors, std::memory_order_relaxed);
  }
  return f._num_sources;
}

inline void Executor::_set_up_topology(Worker* w, Topology* tpg) {
  auto& f = tpg->_taskflow;
  const std::size_t num_srcs = _set_up_graph(f, tpg, &tpg->_join_counter);
  tpg->_join_counter.fetch_add(num_srcs, std::memory_order_relaxed);
  if (w != nullptr) {
    _bulk_schedule(*w, f._graph.data(), num_srcs);
  } else {
    _bulk_schedule(f._graph.data(), num_srcs);
  }
}

inline void Executor::_tear_down_topology(Worker& w, Topology* tpg) {
  auto& f = tpg->_taskflow;
  std::shared_ptr<Topology> keep;
  std::shared_ptr<Topology> next;
  {
    std::lock_guard<std::mutex> lock(f._mutex);

    assert(!f._topologies.empty() && f._topologies.front().get() == tpg);
    keep = std::move(f._topologies.front());
    f._topologies.pop_front();
    if (!f._topologies.empty()) {
      next = f._topologies.front();
    }
  }

  keep->_done.store(true, std::memory_order_release);
  keep->_done.notify_all();
  _decrement_topology();
  if (next) {
    _set_up_topology(&w, next.get());
  }
}

inline void Executor::corun(Taskflow& f) {
  Worker* w = this_worker();
  if (w == nullptr) {
    throw std::runtime_error("tp::Executor::corun must be called by a worker");
  }
  _corun_graph(*w, f);
}

inline void Executor::_corun_graph(Worker& w, Taskflow& f) {
  if (f.empty()) return;

  struct Anchor {
    std::atomic<std::size_t> join_counter {0};
  } anchor;
  const std::size_t num_srcs = _set_up_graph(f, nullptr, &anchor.join_counter);
  anchor.join_counter.store(num_srcs, std::memory_order_relaxed);
  _bulk_schedule(w, f._graph.data(), num_srcs);
  _corun_until(w, [&anchor] {
      return anchor.join_counter.load(std::memory_order_acquire) == 0;
    }
  );
}

inline void Executor::_corun_until(Worker& w, const std::function<bool()>& stop) {
  const std::size_t MAX_VICTIM = num_queues();
  const std::size_t MAX_STEALS = ((MAX_VICTIM + 1) << 1);
  while (!stop()) {

    if (Node* t = w._wsq.pop(); t != nullptr) {
      _invoke(w, t);
      continue;
    }
    std::size_t num_steals = 0;
    std::size_t vtm = w._sticky_victim;
    while (true) {
      if (stop()) return;
      Node* t = (vtm < _workers.size())? _workers[vtm]._wsq.steal(): _buffers[vtm - _workers.size()].queue.steal();
      if (t != nullptr) {
        _invoke(w, t);
        w._sticky_victim = vtm;
        break;
      }
      if (++num_steals > MAX_STEALS) {
        std::this_thread::yield();
      }
      vtm = w._rdgen() % MAX_VICTIM;
    }
  }
}

}
