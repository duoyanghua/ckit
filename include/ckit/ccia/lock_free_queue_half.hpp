// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace ckit::ccia {

alignas(64) static unsigned char g_sentinel_half[64];

struct node_counter_half
{
    unsigned internal_count : 30;
    unsigned external_counters : 2;
};

template <typename T>
class lock_free_queue_half
{
    struct node;
    struct counted_node_ptr
    {
        int external_count;
        int padding;
        node *ptr;
        counted_node_ptr() noexcept : external_count(0), padding(0), ptr(nullptr) {}
        counted_node_ptr(int count, node *p) noexcept
            : external_count(count), padding(0), ptr(p) {}
    };
    std::atomic<counted_node_ptr> head;
    std::atomic<counted_node_ptr> tail;
    static T *const sentinel() { return reinterpret_cast<T *>(g_sentinel_half); }

    struct node
    {
        std::atomic<T *> data;
        std::atomic<node_counter_half> count;
        counted_node_ptr next;

        node() : data(nullptr)
        {
            node_counter_half new_count;
            new_count.internal_count = 0;
            new_count.external_counters = 2;
            count.store(new_count);

            next.ptr = nullptr;
            next.external_count = 0;

        }
        void release_ref()
        {
            node_counter_half old_counter = count.load(std::memory_order_relaxed);
            node_counter_half new_counter;
            do
            {
                new_counter = old_counter;
                --new_counter.internal_count;
            } while (!count.compare_exchange_strong(old_counter, new_counter,
                                                    std::memory_order_acq_rel, std::memory_order_acquire));

            if (!new_counter.internal_count && !new_counter.external_counters)
            {
                delete this;
            }
        }
    };

    static void increase_external_count(
        std::atomic<counted_node_ptr> &node_ptr, counted_node_ptr &old_node_ptr)
    {
        counted_node_ptr new_node_ptr;
        do
        {
            new_node_ptr = old_node_ptr;
            ++new_node_ptr.external_count;
        } while (!node_ptr.compare_exchange_strong(old_node_ptr, new_node_ptr,
                                                   std::memory_order_seq_cst, std::memory_order_relaxed));

        old_node_ptr.external_count = new_node_ptr.external_count;
    }

    static void free_external_counter(counted_node_ptr &old_node_ptr)
    {
        node *const ptr = old_node_ptr.ptr;
        int const count_increase = old_node_ptr.external_count - 2;
        node_counter_half old_counter = ptr->count.load(std::memory_order_relaxed);
        node_counter_half new_counter;
        do
        {
            new_counter = old_counter;
            --new_counter.external_counters;
            new_counter.internal_count += count_increase;
        } while (!ptr->count.compare_exchange_strong(old_counter, new_counter,
                                                     std::memory_order_acq_rel, std::memory_order_acquire));

        if (!new_counter.internal_count && !new_counter.external_counters)
        {
            delete ptr;
        }
    }

public:
    lock_free_queue_half()
    {
        counted_node_ptr end;
        end.ptr = new node;
        end.external_count = 1;
        head.store(end);
        tail.store(end);
    }
    lock_free_queue_half(const lock_free_queue_half &other) = delete;
    lock_free_queue_half &operator=(const lock_free_queue_half &other) = delete;

    void push(const T& data) requires std::is_copy_constructible_v<T> {
        std::unique_ptr<T> new_data(new T(data));
        push_impl(std::move(new_data));
    }
    void push(T&& data) requires std::move_constructible<T> {
        std::unique_ptr<T> new_data(new T(std::move(data)));
        push_impl(std::move(new_data));
    }
private:
    void push_impl(std::unique_ptr<T> new_data)
    {
        counted_node_ptr new_next;
        new_next.ptr = new node;
        new_next.external_count = 1;

        counted_node_ptr old_tail = tail.load(std::memory_order_relaxed);
        for (;;)
        {
            increase_external_count(tail, old_tail);

            T *old_data = nullptr;

            if (old_tail.ptr->data.compare_exchange_strong(old_data, new_data.get(),
                                                           std::memory_order_relaxed, std::memory_order_relaxed))
            {

                old_tail.ptr->next = new_next;
                old_tail = tail.exchange(new_next, std::memory_order_seq_cst);
                free_external_counter(old_tail);

                new_data.release();
                break;
            }
            old_tail.ptr->release_ref();
        }
    }
public:
    std::unique_ptr<T> pop()
    {

        counted_node_ptr old_head = head.load(std::memory_order_relaxed);
        if (old_head.ptr == tail.load(std::memory_order_relaxed).ptr)
            return std::unique_ptr<T>();

        for (;;)
        {
            increase_external_count(head, old_head);
            node *const ptr = old_head.ptr;

            if (ptr == tail.load(std::memory_order_seq_cst).ptr)
            {
                ptr->release_ref();
                return std::unique_ptr<T>();
            }
            if (head.compare_exchange_strong(old_head, ptr->next,
                                             std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                T *const res = ptr->data.exchange(sentinel(), std::memory_order_relaxed);
                free_external_counter(old_head);
                return std::unique_ptr<T>(res);
            }
            ptr->release_ref();
        }
    }

    ~lock_free_queue_half()
    {
        while (pop())
            ;
        node *const terminal = head.load(std::memory_order_relaxed).ptr;
        if (terminal)
        {
            node *const tail_ptr = tail.load(std::memory_order_relaxed).ptr;
            if (terminal == tail_ptr)
            {
                delete terminal;
            }
            else
            {
                counted_node_ptr last = head.load(std::memory_order_relaxed);
                terminal->data.exchange(sentinel());
                free_external_counter(last);
            }
        }
    }
};

}  // namespace ckit::ccia
