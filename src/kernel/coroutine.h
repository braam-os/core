// Freestanding replacement for <coroutine>. libc++'s header pulls in <cstring>
// and <cmath>, which need a sysroot wasm32-unknown-unknown does not have.
// Rationale in doc/Release_Notes.md; see also Concept.md Appendix C.
#pragma once

#include "types.h"

namespace std {
template <class R, class...>
struct coroutine_traits {
    using promise_type = typename R::promise_type;
};

template <class Promise = void>
struct coroutine_handle;

template <>
struct coroutine_handle<void> {
    constexpr coroutine_handle() noexcept = default;

    constexpr coroutine_handle(nullptr_t) noexcept {}

    coroutine_handle &operator=(nullptr_t) noexcept
    {
        frame = nullptr;
        return *this;
    }

    constexpr void *address() const noexcept { return frame; }

    static constexpr coroutine_handle from_address(void *a) noexcept
    {
        coroutine_handle h;
        h.frame = a;
        return h;
    }

    constexpr explicit operator bool() const noexcept { return frame != nullptr; }

    bool done() const { return __builtin_coro_done(frame); }

    void operator()() const { resume(); }

    void resume() const { __builtin_coro_resume(frame); }

    void destroy() const { __builtin_coro_destroy(frame); }

    friend constexpr bool operator==(coroutine_handle a, coroutine_handle b) noexcept
    {
        return a.frame == b.frame;
    }

protected:
    void *frame = nullptr;
};

template <class Promise>
struct coroutine_handle : coroutine_handle<> {
    using coroutine_handle<>::coroutine_handle;

    static coroutine_handle from_promise(Promise &p)
    {
        coroutine_handle h;
        h.frame = __builtin_coro_promise(reinterpret_cast<char *>(&p), alignof(Promise), true);
        return h;
    }

    static constexpr coroutine_handle from_address(void *a) noexcept
    {
        coroutine_handle h;
        h.frame = a;
        return h;
    }

    // The conversion to coroutine_handle<> comes from the public base.
    Promise &promise() const
    {
        return *static_cast<Promise *>(__builtin_coro_promise(frame, alignof(Promise), false));
    }
};

struct noop_coroutine_promise {};

template <>
struct coroutine_handle<noop_coroutine_promise> : coroutine_handle<> {
    constexpr explicit operator bool() const noexcept { return true; }

    constexpr bool done() const noexcept { return false; }

    void operator()() const noexcept {}

    void resume() const noexcept {}

    void destroy() const noexcept {}

private:
    friend coroutine_handle<noop_coroutine_promise> noop_coroutine() noexcept;

    coroutine_handle() noexcept { frame = __builtin_coro_noop(); }
};

using noop_coroutine_handle = coroutine_handle<noop_coroutine_promise>;

inline noop_coroutine_handle noop_coroutine() noexcept
{
    return {};
}

struct suspend_always {
    constexpr bool await_ready() const noexcept { return false; }

    constexpr void await_suspend(coroutine_handle<>) const noexcept {}

    constexpr void await_resume() const noexcept {}
};

struct suspend_never {
    constexpr bool await_ready() const noexcept { return true; }

    constexpr void await_suspend(coroutine_handle<>) const noexcept {}

    constexpr void await_resume() const noexcept {}
};
} // namespace std
