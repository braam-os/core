// Fixed-width integers, placement new, and the wasm ABI attributes.
#pragma once

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;
using f32 = float;
using f64 = double;

using usize = __SIZE_TYPE__;
using isize = __PTRDIFF_TYPE__;

static_assert(sizeof(usize) == 4, "wasm32");

namespace std {
using size_t = usize;
using nullptr_t = decltype(nullptr);
} // namespace std

inline void *operator new(usize, void *p) noexcept {
    return p;
}

inline void *operator new[](usize, void *p) noexcept {
    return p;
}

inline void operator delete(void *, void *) noexcept {}

inline void operator delete[](void *, void *) noexcept {}

// Names the wasm export. Without `used` the linker's --gc-sections drops it.
#define BRAAM_EXPORT(name) extern "C" __attribute__((export_name(name), used))

// Names the wasm import. Every import lands in the "host" module.
#define BRAAM_IMPORT(name) extern "C" __attribute__((import_module("host"), import_name(name)))

#define BRAAM_UNREACHABLE() __builtin_trap()
