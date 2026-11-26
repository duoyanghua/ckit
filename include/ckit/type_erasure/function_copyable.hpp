// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <stdexcept>

#include <new>
#include <memory>
#include <functional>
#include <utility>
#include <type_traits>

namespace ckit::te::copyable {

template <typename Signature>
class MyFunction;

template <typename R, typename... Args>
class MyFunction<R(Args...)>
{
    struct Concept
    {
        virtual ~Concept() = default;
        virtual R invoke(Args&&...args) const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;

    };
    template <typename F>
    struct Model final : Concept
    {
        mutable F f_;

        template <typename FF>
        Model(FF &&f) : f_(std::forward<FF>(f)){}

        R invoke(Args&&...args) const override
        {

            if constexpr (std::is_void_v<R>)
                std::invoke(f_, std::forward<Args>(args)...);
            else
                return std::invoke(f_, std::forward<Args>(args)...);

        }

        std::unique_ptr<Concept> clone() const override {  return std::make_unique<Model<F>>(f_);  }

    };
    std::unique_ptr<Concept> pImpl_;
public:
    MyFunction() noexcept = default;
    ~MyFunction() = default;
    MyFunction(const MyFunction &other){ if (other.pImpl_) pImpl_ = other.pImpl_->clone(); }
    MyFunction(MyFunction &&other) noexcept { if (other.pImpl_) pImpl_ = std::move(other.pImpl_); }

    MyFunction &operator=(const MyFunction &other)
    {
        if (this != &other)
        {
            if(pImpl_)
                pImpl_.reset();
            pImpl_ = other.pImpl_ ? other.pImpl_->clone() : nullptr;

        }
        return *this;
    }
    MyFunction &operator=(MyFunction &&other) noexcept
    {
        if (this != &other)
        {
            if(pImpl_)
                pImpl_.reset();
            pImpl_ = other.pImpl_ ? std::move(other.pImpl_) : nullptr;

        }
        return *this;
    }

    template <typename F,
        typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MyFunction> &&
        std::is_invocable_r_v<R, F, Args...>>
        >
    MyFunction(F &&f): pImpl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))){}

    R operator()(Args... args) const
    {
        if (!pImpl_)
            throw std::bad_function_call();
        return pImpl_->invoke(std::forward<Args>(args)...);
    }
    explicit operator bool() const noexcept
    {

        return pImpl_ != nullptr;
    }

};

}  // namespace ckit::te::copyable
