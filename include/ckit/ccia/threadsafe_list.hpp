// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <algorithm>
#include <list>
#include <memory>
#include <mutex>

namespace ckit::ccia {

template <typename T>
class threadsafe_list
{
    struct node
    {
        mutable std::mutex m;
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
        node() {}
        node(T const &value) : data(std::make_shared<T>(value)) {}
        node(T &&value) : data(std::make_shared<T>(std::move(value))) {}
    };
    node head;
public:
    threadsafe_list() {}
    ~threadsafe_list()
    {
        remove_if([](T const &)
                  { return true; });
    }
    threadsafe_list(threadsafe_list const &other) = delete;
    threadsafe_list &operator=(threadsafe_list const &other) = delete;

    void push_front(T const &value) requires std::is_copy_constructible_v<T>
    {
        std::unique_ptr<node> new_node(new node(value));
        push_front_impl(std::move(new_node));
    }
    void push_front(T &&value) requires std::move_constructible<T>
    {
        std::unique_ptr<node> new_node(new node(std::move(value)));
        push_front_impl(std::move(new_node));
    }
private:
    void push_front_impl(std::unique_ptr<node> new_node)
    {
        std::lock_guard<std::mutex> lk(head.m);
        new_node->next = std::move(head.next);
        head.next = std::move(new_node);
    }
public:
    template <typename Function>
    void for_each(Function f)
    {

        node *current = &head;
        std::unique_lock<std::mutex> lk(head.m);

        while (node *const next = current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            lk.unlock();
            f(*next->data);

            current = next;
            lk = std::move(next_lk);

        }
    }
    template <typename Predicate>
    std::shared_ptr<T> find_first_if(Predicate &&p)
    {
        node *current = &head;
        std::unique_lock<std::mutex> lk(head.m);

        while (node *const next = current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            lk.unlock();
            if (std::forward<Predicate>(p)(*next->data))
            {
                return next->data;
            }
            current = next;
            lk = std::move(next_lk);
        }
        return std::shared_ptr<T>();
    }
    template <typename Predicate>
    void remove_if(Predicate &&p)
    {

        node *current = &head;
        std::unique_lock<std::mutex> lk(head.m);

        while (node *const next = current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            if (std::forward<Predicate>(p)(*next->data))
            {
                std::unique_ptr<node> old_next = std::move(current->next);
                current->next = std::move(next->next);
                next_lk.unlock();

            }
            else
            {
                lk.unlock();
                current = next;
                lk = std::move(next_lk);
            }
        }
    }
    template <typename Predicate>
    void insert_first_if(T value, Predicate &&p)
    {

        std::unique_ptr<node> new_node = std::make_unique<node>(std::move(value));

        node *current = &head;
        std::unique_lock<std::mutex> lk(head.m);
        while (node *const next = current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);

            if (std::forward<Predicate>(p)(*next->data))
            {

                new_node->next = std::move(current->next);
                current->next = std::move(new_node);
                next_lk.unlock();
                lk.unlock();

                return;
            }
            lk.unlock();
            current = next;
            lk = std::move(next_lk);
        }

    }
};

}  // namespace ckit::ccia
