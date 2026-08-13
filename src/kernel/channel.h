// Channel<T> — an async queue with bounded capacity (Concept.md §3.6). The
// storage is inline, so a channel can be a global and sending never allocates.
#pragma once

#include "coroutine.h"
#include "result.h"
#include "sched.h"
#include "traits.h"
#include "types.h"

// N is the ring capacity. The default keeps the spelling Channel<Key> intact.
// Many senders, one receiver: a second suspended receiver displaces the first.
template <class T, usize N = 64> struct Channel {
    static_assert(N > 0 && (N & (N - 1)) == 0, "the capacity is a power of two");

    Channel() = default;

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    usize size() const { return n_; }

    bool empty() const { return n_ == 0; }

    bool full() const { return n_ == N; }

    // Non-blocking: the fast path a JS export calls, since it is not in a
    // coroutine. A full ring drops rather than blocking. Only queues the
    // receiver, so it can never re-enter the scheduler.
    bool try_send(T v) {
        if (n_ == N)
            return false;
        slots_[(head_ + n_) & (N - 1)] = move(v);
        n_++;

        u32 token = recv_token_;
        recv_token_ = 0;
        if (token)
            sched_wake(token, 0, 0);
        return true;
    }

    // Suspends until a value arrives, or until the task is cancelled.
    struct Recv {
        explicit Recv(Channel &c) : c_(c) {}

        Recv(const Recv &) = delete;
        Recv &operator=(const Recv &) = delete;

        ~Recv() {
            if (c_.recv_token_ == w_.token)
                c_.recv_token_ = 0;
            sched_unwait(&w_);
        }

        bool await_ready() const noexcept { return !c_.empty(); }

        template <class P> bool await_suspend(std::coroutine_handle<P> h) {
            w_.h = h;
            w_.cancel = h.promise().cancel;
            if (w_.cancel && w_.cancel->cancelled) {
                w_.cancelled = true;
                return false;
            }
            w_.token = sched_token();
            if (!sched_wait_token(&w_)) {
                w_.failed = true;
                return false;
            }
            c_.recv_token_ = w_.token;
            return true;
        }

        Result<T> await_resume() {
            if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
                return Err(Error::Cancelled);
            if (w_.failed)
                return Err(Error::NoMemory);
            if (c_.empty())
                return Err(Error::Again); // a stray wake on this token
            return c_.take();
        }

    private:
        Channel &c_;
        Waiter w_;
    };

    Recv recv() { return Recv(*this); }

    // Drops everything queued. For tests, so a case starts from nothing.
    void clear() {
        while (n_)
            take();
    }

private:
    T take() {
        T v = move(slots_[head_]);
        head_ = (head_ + 1) & (N - 1);
        n_--;
        return v;
    }

    T slots_[N];
    usize head_ = 0;
    usize n_ = 0;
    u32 recv_token_ = 0; // 0 when nobody is suspended on this channel
};
