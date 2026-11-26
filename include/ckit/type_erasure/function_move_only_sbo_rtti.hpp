// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <stdexcept>

#include <new>
#include <memory>
#include <functional>
#include <utility>
#include <type_traits>
#include <typeinfo>

namespace ckit::te::move_only_sbo_rtti {

template <class T>
inline const void *token_of() noexcept {
    static const char tag = 0;
    return &tag;
}

template <typename Signature>
class MyFunction;

template <typename R, typename... Args>
class MyFunction<R(Args...)>
{
    struct Concept
    {
        virtual ~Concept() = default;
        virtual R invoke(Args &&...args) const = 0;

        virtual void move(void *dst) = 0;
        virtual const void *address_token() const noexcept = 0;
#if __cpp_rtti
        virtual const std::type_info *type_token() const noexcept = 0;
#endif
    };
    template <typename F>
    struct Model final : Concept
    {
        mutable F f_;

        template <typename FF>
        explicit Model(FF &&f) : f_(std::forward<FF>(f)) {}

        R invoke(Args &&...args) const override
        {
            if constexpr (std::is_void_v<R>)
                std::invoke(f_, std::forward<Args>(args)...);
            else
                return std::invoke(f_, std::forward<Args>(args)...);
        }

        void move(void *dst) override { ::new (dst) Model<F>(std::move(f_)); }
        const void *address_token() const noexcept override { return token_of<F>(); }
#if __cpp_rtti
        const std::type_info *type_token() const noexcept override { return &typeid(F); }
#endif
    };

    static constexpr std::size_t kBufferSize = 32;
    bool is_sbo_ = false;
    union Storage
    {
        alignas(std::max_align_t) std::byte sbo_buf_[kBufferSize];
        Concept *heap_ptr_;
        Storage() noexcept : heap_ptr_(nullptr) {}
    };
    Storage storage_;

    Concept *get() noexcept
    {
        return is_sbo_ ? reinterpret_cast<Concept*>(&storage_.sbo_buf_) : storage_.heap_ptr_;
    }
    const Concept *get() const noexcept
    {
        return is_sbo_ ? reinterpret_cast<const Concept *>(&storage_.sbo_buf_)
                       : storage_.heap_ptr_;
    }

    void destroy() noexcept
    {
        if (get())
        {
            if (is_sbo_)
                get()->~Concept();
            else
                delete storage_.heap_ptr_;
        }
        storage_.heap_ptr_ = nullptr;
        is_sbo_ = false;
    }
    template <typename F>
    void construct_from(F &&f)
    {
        using ModelType = Model<std::decay_t<F>>;
        if constexpr (sizeof(ModelType) <= kBufferSize && alignof(ModelType) <= alignof(std::max_align_t))
        {
            ::new (&storage_.sbo_buf_) ModelType(std::forward<F>(f));
            is_sbo_ = true;
        }
        else
        {
            storage_.heap_ptr_ = new ModelType(std::forward<F>(f));
            is_sbo_ = false;
        }
    }

public:

    MyFunction() noexcept = default;
    ~MyFunction() { destroy(); }

    MyFunction(const MyFunction &other) = delete;
    MyFunction &operator=(const MyFunction &other) = delete;

    MyFunction(MyFunction &&other) noexcept
    {
        if (other.is_sbo_)
        {

            other.get()->move(&storage_.sbo_buf_);
            other.get()->~Concept();
            other.storage_.heap_ptr_ = nullptr;
            other.is_sbo_ = false;
            is_sbo_ = true;
        }
        else
        {
            if(other.get()) storage_.heap_ptr_ = other.storage_.heap_ptr_;
            else storage_.heap_ptr_ = nullptr;

            other.storage_.heap_ptr_ = nullptr;
            is_sbo_ = false;
        }
    }
    MyFunction &operator=(MyFunction &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            if (other.is_sbo_)
            {
                other.get()->move(&storage_.sbo_buf_);
                other.get()->~Concept();
                other.storage_.heap_ptr_ = nullptr;
                other.is_sbo_ = false;
                is_sbo_ = true;
            }
            else
            {
                if(other.get()) storage_.heap_ptr_ = other.storage_.heap_ptr_;
                else storage_.heap_ptr_ = nullptr;

                other.storage_.heap_ptr_ = nullptr;
                is_sbo_ = false;
            }
        }
        return *this;
    }

    template <typename F,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<F>, MyFunction> &&
                  std::is_invocable_r_v<R, F, Args...>
                  >>
    MyFunction(F &&f)
    {
        construct_from(std::forward<F>(f));
    }

    template <typename F,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<F>, MyFunction> &&
                  std::is_invocable_r_v<R, F, Args...>
                  >>
    MyFunction &operator=(F &&f)
    {
        destroy();
        construct_from(std::forward<F>(f));
        return *this;
    }

    R operator()(Args... args) const
    {
        if (!get())
            throw std::bad_function_call();
        return get()->invoke(std::forward<Args>(args)...);
    }

    template <class T>
    T *target() noexcept
    {
        if (!get() || get()->address_token() != token_of<T>())
            return nullptr;
        if constexpr (std::is_invocable_r_v<R, T, Args...>)
            return &static_cast<Model<T> *>(get())->f_;
        else
            return nullptr;
    }
    template <class T>
    const T *target() const noexcept
    {
        if (!get() || get()->address_token() != token_of<T>())
            return nullptr;
        if constexpr (std::is_invocable_r_v<R, T, Args...>)
            return &static_cast<const Model<T> *>(get())->f_;
        else
            return nullptr;
    }

    explicit operator bool() const noexcept { return get() != nullptr; }
    bool empty() const noexcept { return get() == nullptr; }
};

}  // namespace ckit::te::move_only_sbo_rtti
