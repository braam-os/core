// The slice of <type_traits> and <utility> the kernel uses. There is no
// standard library here; these are thin wrappers over clang builtins.
#pragma once

#include "types.h"

template <class T>
using RemoveRef = __remove_reference_t(T);
template <class T>
using RemoveCv = __remove_cv(T);

template <bool B, class T, class F>
struct Cond_ {
    using type = F;
};

template <class T, class F>
struct Cond_<true, T, F> {
    using type = T;
};
template <bool B, class T, class F>
using Cond = typename Cond_<B, T, F>::type;

template <bool B, class T = void>
struct EnableIf_ {};

template <class T>
struct EnableIf_<true, T> {
    using type = T;
};
template <bool B, class T = void>
using EnableIf = typename EnableIf_<B, T>::type;

template <class A, class B>
inline constexpr bool is_same = __is_same(A, B);

template <class T>
inline constexpr bool is_trivially_destructible = __is_trivially_destructible(T);
template <class T>
inline constexpr bool is_trivially_copyable = __is_trivially_copyable(T);

template <class T>
constexpr RemoveRef<T> &&move(T &&v) noexcept
{
    return static_cast<RemoveRef<T> &&>(v);
}

template <class T>
constexpr T &&forward(RemoveRef<T> &v) noexcept
{
    return static_cast<T &&>(v);
}

template <class T>
constexpr T &&forward(RemoveRef<T> &&v) noexcept
{
    return static_cast<T &&>(v);
}

template <class T>
RemoveRef<T> &&declval() noexcept;

template <class T>
constexpr void swap(T &a, T &b) noexcept
{
    T t = move(a);
    a   = move(b);
    b   = move(t);
}

template <class T>
constexpr const T &min(const T &a, const T &b)
{
    return b < a ? b : a;
}

template <class T>
constexpr const T &max(const T &a, const T &b)
{
    return a < b ? b : a;
}
