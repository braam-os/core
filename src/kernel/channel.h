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
//
// A channel has two ends and either may quit. close() is the writer saying
// there will be no more values: the receiver drains what is queued and then
// reads EOF. hangup() is the reader saying it will take no more: senders get
// Err(Closed), which is what stops a producer feeding a pipe nobody reads.
template <class T, usize N = 64>
struct Channel {
    static_assert(N > 0 && (N & (N - 1)) == 0, "the capacity is a power of two");

    Channel() = default;

    Channel(const Channel &)            = delete;
    Channel &operator=(const Channel &) = delete;

    usize size() const { return n_; }

    bool empty() const { return n_ == 0; }

    bool full() const { return n_ == N; }

    bool closed() const { return closed_; }

    bool hung_up() const { return hup_; }

    // The writer is done. A parked receiver has to be woken to see it, since
    // no value is coming to wake it.
    void close()
    {
        if (closed_)
            return;
        closed_ = true;
        wake_recv();
        wake_send();
    }

    // The reader is gone. Anything parked in send() resumes with Err(Closed).
    void hangup()
    {
        if (hup_)
            return;
        hup_ = true;
        wake_send();
        wake_recv();
    }

    // Non-blocking: the fast path a JS export calls, since it is not in a
    // coroutine. A full ring drops rather than blocking. Only queues the
    // receiver, so it can never re-enter the scheduler.
    bool try_send(T v)
    {
        if (closed_ || hup_ || n_ == N)
            return false;
        put(move(v));
        return true;
    }

    // Suspends until a value arrives, or until the task is cancelled.
    struct Recv {
        explicit Recv(Channel &c) : c_(c) {}

        Recv(const Recv &)            = delete;
        Recv &operator=(const Recv &) = delete;

        // The equality guard is what makes handover between two receivers
        // safe: a receiver whose token has already been displaced must not
        // clear the token belonging to the one that displaced it.
        ~Recv()
        {
            if (c_.recv_token_ == w_.token)
                c_.recv_token_ = 0;
            sched_unwait(&w_);
        }

        bool await_ready() const noexcept { return !c_.empty() || c_.closed_; }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> h)
        {
            w_.h      = h;
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

        Result<T> await_resume()
        {
            if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
                return Err(Error::Cancelled);
            if (w_.failed)
                return Err(Error::NoMemory);
            if (!c_.empty())
                return c_.take(); // drain before reporting EOF
            if (c_.closed_)
                return Err(Error::Closed);
            return Err(Error::Again); // a stray wake on this token
        }

    private:
        Channel &c_;
        Waiter w_;
    };

    Recv recv() { return Recv(*this); }

    // Suspends until the ring has room, the reader hangs up, or the task is
    // cancelled. A cancelled sender delivers nothing: the value stays here and
    // dies with the awaitable, because the ring is only ever written by put().
    struct Send {
        Send(Channel &c, T v) : c_(c), v_(move(v)) {}

        Send(const Send &)            = delete;
        Send &operator=(const Send &) = delete;

        // Clearing the token is not optional: it is what disarms the
        // second-sender tripwire below when a send is cancelled.
        ~Send()
        {
            if (c_.send_token_ == w_.token)
                c_.send_token_ = 0;
            sched_unwait(&w_);
        }

        bool await_ready() const noexcept { return false; }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> h)
        {
            w_.h      = h;
            w_.cancel = h.promise().cancel;
            if (w_.cancel && w_.cancel->cancelled) {
                w_.cancelled = true;
                return false;
            }
            if (c_.closed_ || c_.hup_)
                return false;
            if (c_.n_ < N) {
                c_.put(move(v_));
                sent_ = true;
                return false;
            }
            // One token holds one sender. Every pipe in the system has a
            // single writer, so this cannot fire today; an operator that
            // joins two writers onto one channel needs the intrusive waiter
            // queue first, and this is what says so out loud.
            if (c_.send_token_)
                panic("channel: a second sender blocked on one channel");
            w_.token = sched_token();
            if (!sched_wait_token(&w_)) {
                w_.failed = true;
                return false;
            }
            c_.send_token_ = w_.token;
            return true;
        }

        Result<void> await_resume()
        {
            if (sent_)
                return {};
            if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
                return Err(Error::Cancelled);
            if (w_.failed)
                return Err(Error::NoMemory);
            if (c_.closed_ || c_.hup_)
                return Err(Error::Closed);
            if (c_.n_ < N) {
                c_.put(move(v_));
                return {};
            }
            return Err(Error::Again); // a stray wake on this token
        }

    private:
        Channel &c_;
        T v_;
        Waiter w_;
        bool sent_ = false;
    };

    Send send(T v) { return Send(*this, move(v)); }

    // Drops everything queued. For tests, so a case starts from nothing.
    void clear()
    {
        while (n_)
            take();
    }

    // The two ends, for an awaiter outside this class that parks on the same
    // wakeups Send and Recv use — the stream layer, which has to convert and
    // count bytes and so cannot simply be a Send. `on` arms, and disarming
    // compares, so a displaced waiter cannot clear its successor's token.
    void park_sender(u32 token, bool on)
    {
        if (!on) {
            if (send_token_ == token)
                send_token_ = 0;
            return;
        }
        if (send_token_)
            panic("channel: a second sender blocked on one channel");
        send_token_ = token;
    }

    void park_receiver(u32 token, bool on)
    {
        if (!on) {
            if (recv_token_ == token)
                recv_token_ = 0;
            return;
        }
        recv_token_ = token;
    }

    // Non-blocking take, for a reader that has already established the ring is
    // not empty. Wakes a parked sender, exactly as recv() does.
    Option<T> try_recv()
    {
        if (n_ == 0)
            return None;
        return take();
    }

private:
    void wake_recv()
    {
        u32 token   = recv_token_;
        recv_token_ = 0;
        if (token)
            sched_wake(token, 0, 0);
    }

    void wake_send()
    {
        u32 token   = send_token_;
        send_token_ = 0;
        if (token)
            sched_wake(token, 0, 0);
    }

    void put(T v)
    {
        slots_[(head_ + n_) & (N - 1)] = move(v);
        n_++;
        wake_recv();
    }

    // The sender is woken from here rather than from Recv::await_resume,
    // because a receiver that finds the ring non-empty never suspends and is
    // never woken — which is the ordinary case for a pipe. Waking on the
    // resume path would hang a full ring against a fast reader.
    T take()
    {
        T v   = move(slots_[head_]);
        head_ = (head_ + 1) & (N - 1);
        n_--;
        wake_send();
        return v;
    }

    T slots_[N];
    usize head_     = 0;
    usize n_        = 0;
    u32 recv_token_ = 0; // 0 when nobody is suspended on this channel
    u32 send_token_ = 0;
    bool closed_    = false; // the writer is done
    bool hup_       = false; // the reader is gone
};
