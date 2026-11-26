// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <memory>
#include <mutex>
#include <condition_variable>

namespace ckit::ccia {

template <typename T>
class threadsafe_queue_fine_grained
{
private:
    struct node{  std::shared_ptr<T> data;  std::unique_ptr<node> next;  };
    mutable std::mutex head_mutex;
    std::unique_ptr<node> head;
    mutable std::mutex tail_mutex;
    node *tail;
    std::condition_variable data_cond;

public:
    threadsafe_queue_fine_grained() : head(new node), tail(head.get()) {}
    threadsafe_queue_fine_grained(const threadsafe_queue_fine_grained &other) = delete;
    threadsafe_queue_fine_grained &operator=(const threadsafe_queue_fine_grained &other) = delete;

private:
    node *get_tail() const
    {
        std::lock_guard<std::mutex> tail_lock(tail_mutex);
        return tail;
    }
    std::unique_ptr<node> pop_head()
    {
        std::unique_ptr<node> old_head = std::move(head);
        head = std::move(old_head->next);
        return old_head;
    }

    std::unique_lock<std::mutex> wait_for_data()
    {
        std::unique_lock<std::mutex> head_lock(head_mutex);

        data_cond.wait(head_lock, [&]
                       { return head.get() != get_tail(); });

        return std::move(head_lock);
    }
    std::unique_ptr<node> wait_pop_head()
    {
        std::unique_lock<std::mutex> head_lock(wait_for_data());
        return pop_head();
    }
    std::unique_ptr<node> wait_pop_head(T &value)
    {
        std::unique_lock<std::mutex> head_lock(wait_for_data());
        value = std::move(*head->data);
        return pop_head();
    }

    std::unique_ptr<node> try_pop_head()
    {
        std::lock_guard<std::mutex> head_lock(head_mutex);
        if (head.get() == get_tail())
        {
            return std::unique_ptr<node>();
        }

        return pop_head();
    }
    std::unique_ptr<node> try_pop_head(T &value)
    {
        std::lock_guard<std::mutex> head_lock(head_mutex);
        if (head.get() == get_tail())
        {
            return std::unique_ptr<node>();
        }
        value = std::move(*head->data);
        return pop_head();
    }

public:
    void push(const T& data) requires std::is_copy_constructible_v<T> {

        std::shared_ptr<T> new_data(std::make_shared<T>(data));
        push_impl(std::move(new_data));
    }
    void push(T && data) requires std::move_constructible<T>  {

        std::shared_ptr<T> new_data(std::make_shared<T>(std::move(data)));
        push_impl(std::move(new_data));
    }
private:
    void push_impl(std::shared_ptr<T> new_data)
    {
        std::unique_ptr<node> p(new node);
        node *const new_tail = p.get();
        {
            std::lock_guard<std::mutex> tail_lock(tail_mutex);
            tail->data = new_data;
            tail->next = std::move(p);
            tail = new_tail;
        }
        data_cond.notify_one();
    }

public:
    std::shared_ptr<T> wait_and_pop()
    {
        std::unique_ptr<node> const old_head = wait_pop_head();
        return old_head->data;
    }
    void wait_and_pop(T &value)
    {
        std::unique_ptr<node> const old_head = wait_pop_head(value);
    }
    std::shared_ptr<T> try_pop()
    {
        std::unique_ptr<node> const old_head = try_pop_head();
        return old_head ? old_head->data : std::shared_ptr<T>();
    }
    bool try_pop(T &value)
    {
        std::unique_ptr<node> const old_head = try_pop_head(value);
        return old_head != nullptr;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> head_lock(head_mutex);
        return (head.get() == get_tail());
    }
};

}  // namespace ckit::ccia
