// Task<T> — a lazy, move-only coroutine (Concept.md §3.3). Awaiting one enters
// it by symmetric transfer; finishing transfers straight back to its awaiter.
#pragma once

#include "coroutine.h"
#include "result.h"
#include "traits.h"
#include "types.h"

struct CancelState;

template <class T = void>
struct Task;

struct TaskPromiseBase {
    std::coroutine_handle<> continuation = nullptr;
    CancelState *cancel                  = nullptr;

    std::suspend_always initial_suspend() noexcept { return {}; }

    // Resumes the awaiter, or nobody when this task is a root.
    struct Final {
        bool await_ready() const noexcept { return false; }

        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept
        {
            std::coroutine_handle<> c = h.promise().continuation;
            if (!c)
                return std::noop_coroutine();
            return c;
        }

        void await_resume() const noexcept {}
    };

    Final final_suspend() noexcept { return {}; }

    void unhandled_exception() {}
};

// Declaring get_return_object_on_allocation_failure is what makes a frame
// allocation failure a value rather than undefined behaviour: without it the
// compiler assumes operator new never returns null, and ours does.
template <class T>
struct TaskPromise : TaskPromiseBase {
    Option<T> value;

    Task<T> get_return_object();

    static Task<T> get_return_object_on_allocation_failure();

    void return_value(T v) { value = Option<T>(move(v)); }
};

template <>
struct TaskPromise<void> : TaskPromiseBase {
    Task<void> get_return_object();

    static Task<void> get_return_object_on_allocation_failure();

    void return_void() {}
};

template <class T>
struct Task {
    using promise_type = TaskPromise<T>;
    using Handle       = std::coroutine_handle<promise_type>;

    Task() = default;

    explicit Task(Handle h) : h_(h) {}

    Task(Task &&o) noexcept : h_(o.h_) { o.h_ = Handle(); }

    Task &operator=(Task &&o) noexcept
    {
        if (this != &o) {
            if (h_)
                h_.destroy();
            h_   = o.h_;
            o.h_ = Handle();
        }
        return *this;
    }

    Task(const Task &)            = delete;
    Task &operator=(const Task &) = delete;

    ~Task()
    {
        if (h_)
            h_.destroy();
    }

    explicit operator bool() const { return bool(h_); }

    bool done() const { return !h_ || h_.done(); }

    Handle handle() const { return h_; }

    // Gives up ownership of the frame; the caller must destroy it.
    Handle release()
    {
        Handle h = h_;
        h_       = Handle();
        return h;
    }

    // The cancel state travels from the awaiting coroutine into the awaited
    // one, so cancellation propagates down the tree (Concept.md §3.6).
    struct Awaiter {
        Handle h;

        bool await_ready() const noexcept { return !h || h.done(); }

        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> caller) noexcept
        {
            h.promise().continuation = caller;
            h.promise().cancel       = caller.promise().cancel;
            return h;
        }

        // A null handle means the frame never allocated. Nothing sensible can
        // be returned for it, so it stops here rather than reading a promise
        // that does not exist; a spawned root reports the failure instead.
        T await_resume()
        {
            if (!h)
                panic("await on a coroutine whose frame failed to allocate");
            if constexpr (!is_same<T, void>)
                return move(h.promise().value.value());
        }
    };

    Awaiter operator co_await() { return Awaiter{ h_ }; }

private:
    Handle h_;
};

template <class T>
Task<T> TaskPromise<T>::get_return_object()
{
    return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}

inline Task<void> TaskPromise<void>::get_return_object()
{
    return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

template <class T>
Task<T> TaskPromise<T>::get_return_object_on_allocation_failure()
{
    return {};
}

inline Task<void> TaskPromise<void>::get_return_object_on_allocation_failure()
{
    return {};
}
