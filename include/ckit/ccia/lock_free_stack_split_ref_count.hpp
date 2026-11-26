// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <atomic>
#include <memory>

namespace ckit::ccia {

template <typename T>
class lock_free_stack_split_ref_count
{
private:
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
    struct node
    {
        std::shared_ptr<T> data;
        std::atomic<int> internal_count;
        counted_node_ptr next;
        node(T const &data_) : data(std::make_shared<T>(data_)), internal_count(0) {}
        node(T &&data_) : data(std::make_shared<T>(std::move(data_))), internal_count(0) {}
    };

    std::atomic<counted_node_ptr> head{counted_node_ptr{0, nullptr}};

public:
    void push(const T& data) requires std::is_copy_constructible_v<T> {
        push_impl(new node(data));
    }
    void push(T&& data) requires std::move_constructible<T> {
        push_impl(new node(std::move(data)));
    }
private:
    void push_impl(node* node_ptr)
    {
        counted_node_ptr new_node;
        new_node.ptr = node_ptr;
        new_node.external_count = 1;
        new_node.ptr->next = head.load(std::memory_order_relaxed);

        while (!head.compare_exchange_weak(new_node.ptr->next, new_node,
                                           std::memory_order_release, std::memory_order_relaxed))
            ;
    }

    void increase_head_count(counted_node_ptr &old_fat_ptr)
    {
        counted_node_ptr new_fat_ptr;
        do
        {
            new_fat_ptr = old_fat_ptr;
            ++new_fat_ptr.external_count;
        } while (!head.compare_exchange_strong(old_fat_ptr, new_fat_ptr,
                                               std::memory_order_acquire, std::memory_order_relaxed));
        old_fat_ptr.external_count = new_fat_ptr.external_count;

    }

public:
    std::shared_ptr<T> pop()
    {

        counted_node_ptr old_head = head.load(std::memory_order_relaxed);

        for (;;)
        {

            increase_head_count(old_head);

            node *const ptr = old_head.ptr;

            if (!ptr)
                return std::shared_ptr<T>();

            if (head.compare_exchange_strong(old_head, ptr->next,
                                             std::memory_order_relaxed, std::memory_order_relaxed))
            {

                std::shared_ptr<T> res;
                res.swap(ptr->data);

                int const count_increase = old_head.external_count - 2;

                if (ptr->internal_count.fetch_add(count_increase, std::memory_order_acq_rel) == -count_increase)
                {

                    delete ptr;
                }
                return res;
            }
            else if (ptr->internal_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {

                ptr->internal_count.load(std::memory_order_acquire);
                delete ptr;
            }
        }
    }
    ~lock_free_stack_split_ref_count()
    {
        while (pop())
            ;
    }
};

}  // namespace ckit::ccia
