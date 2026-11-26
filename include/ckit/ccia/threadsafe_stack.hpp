// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include<algorithm>
#include<future>
#include<thread>
#include<vector>
#include<mutex>
#include<stack>

namespace ckit::ccia {

template<typename T>
class threadsafe_stack {
private:
    std::stack<std::shared_ptr<T>> data;
    mutable std::mutex m;
public:
    threadsafe_stack() {}
    threadsafe_stack(const threadsafe_stack& other) {
        std::lock_guard<std::mutex> lock(other.m);
        data = other.data;
    }
    threadsafe_stack& operator=(const threadsafe_stack&) = delete;

    void push(const T& data) requires std::is_copy_constructible_v<T> {

        std::shared_ptr<T> new_data(std::make_shared<T>(data));
        push_impl(std::move(new_data));
    }
    void push(T&& data) requires std::move_constructible<T> {
        std::shared_ptr<T> new_data(std::make_shared<T>(std::move(data)));
        push_impl(std::move(new_data));
    }
private:
    void push_impl(std::shared_ptr<T> new_data)
    {
        std::lock_guard<std::mutex> lock(m);
        data.push(new_data);
    }
public:
    std::shared_ptr<T> pop() {
        std::lock_guard<std::mutex> lock(m);
        if (data.empty()) return nullptr;

        std::shared_ptr<T> const res(data.top());
        data.pop();
        return res;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lock(m);
        return data.empty();
    }
};

}  // namespace ckit::ccia
