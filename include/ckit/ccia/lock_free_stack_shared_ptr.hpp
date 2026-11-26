// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <atomic>
#include <memory>

namespace ckit::ccia {

template <typename T>
class lock_free_stack_shared_ptr
{
private:
    struct node
    {
        std::shared_ptr<T> data;
        std::shared_ptr<node> next;

        node(T const &data_) : data(std::make_shared<T>(data_)) {}
        node(T &&data_) : data(std::make_shared<T>(std::move(data_))) {}
    };
    std::shared_ptr<node> head;

public:
    void push(const T& data) requires std::is_copy_constructible_v<T> {
        std::shared_ptr<node> const new_node = std::make_shared<node>(data);
        push_impl(new_node);
    }
    void push(T&& data) requires std::move_constructible<T> {
        std::shared_ptr<node> const new_node = std::make_shared<node>(std::move(data));
        push_impl(new_node);
    }
private:
    void push_impl(std::shared_ptr<node> new_node)
    {
        new_node->next = std::atomic_load(&head);

        while (!std::atomic_compare_exchange_weak(&head, &new_node->next, new_node));

    }
public:
    std::shared_ptr<T> pop()
    {
        std::shared_ptr<node> old_head = std::atomic_load(&head);
        while (old_head && !std::atomic_compare_exchange_weak(&head, &old_head,std::atomic_load(&old_head->next)));

        if (old_head)
        {
            std::atomic_store(&old_head->next, std::shared_ptr<node>());

            return old_head->data;
        }
        return std::shared_ptr<T>();
    }
    ~lock_free_stack_shared_ptr()
    {
        while (pop());
    }
};

}  // namespace ckit::ccia
