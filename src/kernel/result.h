// Errors are values. There are no exceptions; propagation uses TRY().
#pragma once

#include "host.h"
#include "traits.h"
#include "types.h"

enum class Error : u8 {
    Invalid = 1,
    NoMemory,
    NotFound,
    Exists,
    NotDir,
    IsDir,
    Perm,
    Io,
    Cancelled,
    Again,
    Unsupported,
    Closed, // the far end of a stream is gone: EOF to a reader, EPIPE to a writer
};

Str error_name(Error e);

// ---------------------------------------------------------------- Option<T>

struct NoneTag {};

inline constexpr NoneTag None{};

template <class T>
struct Option {
    constexpr Option() : none_{}, has_(false) {}

    constexpr Option(NoneTag) : none_{}, has_(false) {}

    constexpr Option(T v) : val_(move(v)), has_(true) {}

    Option(const Option &o) : none_{}, has_(o.has_)
    {
        if (has_)
            new (&val_) T(o.val_);
    }

    Option(Option &&o) : none_{}, has_(o.has_)
    {
        if (has_)
            new (&val_) T(move(o.val_));
    }

    Option &operator=(Option o)
    {
        reset();
        if (o.has_) {
            new (&val_) T(move(o.val_));
            has_ = true;
        }
        return *this;
    }

    ~Option() { reset(); }

    constexpr bool has_value() const { return has_; }

    constexpr explicit operator bool() const { return has_; }

    T &value()
    {
        if (!has_)
            panic("Option::value on None");
        return val_;
    }

    const T &value() const
    {
        if (!has_)
            panic("Option::value on None");
        return val_;
    }

    T value_or(T alt) const { return has_ ? val_ : move(alt); }

private:
    void reset()
    {
        if (has_) {
            val_.~T();
            has_ = false;
        }
    }

    union {
        char none_;
        T val_;
    };

    bool has_;
};

// ------------------------------------------------------------ Result<T, E>

template <class E>
struct ErrTag {
    E e;
};

template <class E>
constexpr ErrTag<E> Err(E e)
{
    return ErrTag<E>{ e };
}

template <class T, class E = Error>
struct Result {
    constexpr Result(T v) : val_(move(v)), ok_(true) {}

    constexpr Result(ErrTag<E> e) : err_(e.e), ok_(false) {}

    Result(const Result &o) : ok_(o.ok_)
    {
        if (ok_)
            new (&val_) T(o.val_);
        else
            new (&err_) E(o.err_);
    }

    Result(Result &&o) : ok_(o.ok_)
    {
        if (ok_)
            new (&val_) T(move(o.val_));
        else
            new (&err_) E(move(o.err_));
    }

    Result &operator=(Result o)
    {
        destroy();
        ok_ = o.ok_;
        if (ok_)
            new (&val_) T(move(o.val_));
        else
            new (&err_) E(move(o.err_));
        return *this;
    }

    ~Result() { destroy(); }

    constexpr bool is_ok() const { return ok_; }

    constexpr bool is_err() const { return !ok_; }

    constexpr explicit operator bool() const { return ok_; }

    T &value()
    {
        if (!ok_)
            panic("Result::value on an error");
        return val_;
    }

    const T &value() const
    {
        if (!ok_)
            panic("Result::value on an error");
        return val_;
    }

    E error() const
    {
        if (ok_)
            panic("Result::error on a value");
        return err_;
    }

    T value_or(T alt) const { return ok_ ? val_ : move(alt); }

private:
    void destroy()
    {
        if (ok_)
            val_.~T();
        else
            err_.~E();
    }

    union {
        T val_;
        E err_;
    };

    bool ok_;
};

template <class E>
struct Result<void, E> {
    constexpr Result() : ok_(true) {}

    constexpr Result(ErrTag<E> e) : err_(e.e), ok_(false) {}

    constexpr bool is_ok() const { return ok_; }

    constexpr bool is_err() const { return !ok_; }

    constexpr explicit operator bool() const { return ok_; }

    void value() const
    {
        if (!ok_)
            panic("Result::value on an error");
    }

    E error() const
    {
        if (ok_)
            panic("Result::error on a value");
        return err_;
    }

private:
    E err_{};
    bool ok_;
};

// Unwraps a Result, or returns its error from the enclosing function. The
// statement expression is a clang extension; -std=gnu++20 enables it.
#define TRY(expr)                       \
    ({                                  \
        auto _try_r = (expr);           \
        if (_try_r.is_err())            \
            return Err(_try_r.error()); \
        move(_try_r.value());           \
    })

// The same, for a Result<void, E> whose value is not wanted.
#define TRY_VOID(expr)                  \
    do {                                \
        auto _try_r = (expr);           \
        if (_try_r.is_err())            \
            return Err(_try_r.error()); \
    } while (0)

// TRY inside a coroutine. A plain `return` is ill-formed there, so these are
// the co_return forms; the enclosing Task's result type carries the error.
#define CO_TRY(expr)                       \
    ({                                     \
        auto _try_r = (expr);              \
        if (_try_r.is_err())               \
            co_return Err(_try_r.error()); \
        move(_try_r.value());              \
    })

#define CO_TRY_VOID(expr)                  \
    do {                                   \
        auto _try_r = (expr);              \
        if (_try_r.is_err())               \
            co_return Err(_try_r.error()); \
    } while (0)
