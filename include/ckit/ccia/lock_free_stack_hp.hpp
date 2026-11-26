// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#ifndef HAZARD_POINTER_STACK_HPP
#define HAZARD_POINTER_STACK_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

namespace ckit::ccia {

inline constexpr unsigned max_hazard_pointers = 100;

struct hazard_pointer
{
    std::atomic<std::thread::id> id;
    std::atomic<void *> pointer;
};
inline hazard_pointer hazard_pointers[max_hazard_pointers];

class hp_owner
{
    hazard_pointer *hp;
public:
    hp_owner(hp_owner const &) = delete;
    hp_owner &operator=(hp_owner const &) = delete;

    hp_owner() : hp(nullptr)
    {
        for (unsigned i = 0; i < max_hazard_pointers; ++i)
        {
            std::thread::id old_id;

            if (hazard_pointers[i].id.compare_exchange_strong(old_id,
                                                              std::this_thread::get_id(),
                                                              std::memory_order_acquire, std::memory_order_relaxed))
            {
                hp = &hazard_pointers[i];
                break;
            }
        }
        if (!hp)
            throw std::runtime_error("No hazard pointers available");
    }
    std::atomic<void *> &get_pointer() { return hp->pointer; }

    ~hp_owner()
    {
        hp->pointer.store(nullptr, std::memory_order_relaxed);

        hp->id.store(std::thread::id(), std::memory_order_release);

    }
};

inline std::atomic<void *> &get_hazard_pointer_for_current_thread()
{
    thread_local static hp_owner hazard;
    return hazard.get_pointer();
}

inline bool outstanding_hazard_pointers_for(void *p)
{
    for (unsigned i = 0; i < max_hazard_pointers; ++i)
        if (hazard_pointers[i].pointer.load(std::memory_order_seq_cst) == p)
            return true;
    return false;
}

template <typename T>
void do_delete(void *p) { delete static_cast<T *>(p); }

struct data_to_reclaim
{
    void *data;
    std::function<void(void *)> deleter;
    data_to_reclaim *next;

    template <typename T>
    data_to_reclaim(T *p) : data(p), deleter(&do_delete<T>), next(nullptr)
    {
    }

    ~data_to_reclaim() { deleter(data); }
};

inline std::atomic<data_to_reclaim *> nodes_to_reclaim{nullptr};

inline void add_to_reclaim_list(data_to_reclaim *node)
{
    node->next = nodes_to_reclaim.load(std::memory_order_relaxed);
    while (!nodes_to_reclaim.compare_exchange_weak(node->next, node,
                                                   std::memory_order_release, std::memory_order_relaxed))
        ;

}

template <typename T>
void reclaim_later(T *data)
{
    add_to_reclaim_list(new data_to_reclaim(data));
}

inline void delete_nodes_with_no_hazards()
{

    data_to_reclaim *current = nodes_to_reclaim.exchange(nullptr,
                                                         std::memory_order_acquire);
    while (current)
    {
        data_to_reclaim *const next = current->next;
        if (!outstanding_hazard_pointers_for(current->data))
            delete current;
        else
            add_to_reclaim_list(current);
        current = next;
    }
}

template <typename T>
class lock_free_stack_hp
{
private:
    struct node
    {
        std::shared_ptr<T> data;
        node *next;
        node(T const &data_) : data(std::make_shared<T>(data_)), next(nullptr) {}
        node(T &&data_) : data(std::make_shared<T>(std::move(data_))), next(nullptr) {}
    };
    std::atomic<node *> head{nullptr};

public:
    void push(const T& data) requires std::is_copy_constructible_v<T> {
        push_impl(new node(data));
    }
    void push(T&& data) requires std::move_constructible<T> {
        push_impl(new node(std::move(data)));
    }
private:
    void push_impl(node* new_node)
    {
        new_node->next = head.load(std::memory_order_relaxed);

        while (!head.compare_exchange_weak(new_node->next, new_node,
                                           std::memory_order_seq_cst, std::memory_order_relaxed))
            ;
    }
public:
    std::shared_ptr<T> pop()
    {
        std::atomic<void *> &hp = get_hazard_pointer_for_current_thread();
        node *old_head = head.load(std::memory_order_seq_cst);
        do
        {
            node *temp;
            do
            {
                temp = old_head;
                hp.store(old_head, std::memory_order_seq_cst);

                old_head = head.load(std::memory_order_seq_cst);

            } while (old_head != temp);
        } while (old_head && !head.compare_exchange_strong(old_head, old_head->next,
                                                           std::memory_order_seq_cst, std::memory_order_relaxed));

        hp.store(nullptr, std::memory_order_relaxed);

        std::shared_ptr<T> res;
        if (old_head)
        {
            res.swap(old_head->data);
            if (outstanding_hazard_pointers_for(old_head))
            {
                reclaim_later(old_head);
            }
            else
            {
                delete old_head;
            }
        }
        delete_nodes_with_no_hazards();

        return res;
    }
    ~lock_free_stack_hp() noexcept
    {
        try
        {
            get_hazard_pointer_for_current_thread();
        }
        catch (std::runtime_error const &)
        {
            return;
        }
        while (pop())
            ;
    }
};

#endif

}  // namespace ckit::ccia
