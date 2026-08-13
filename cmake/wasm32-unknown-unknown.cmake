# Toolchain: freestanding wasm32. BRAAM_LLVM is a clang distribution only —
# none of its runtime and none of its headers are linked or included, so any
# LLVM with the wasm32 target and wasm-ld on PATH will do.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# BRAAM_WASI_SDK is the former name, kept as an alias.
if(DEFINED BRAAM_WASI_SDK AND NOT DEFINED BRAAM_LLVM)
    message(STATUS "BRAAM_WASI_SDK is an alias for BRAAM_LLVM; prefer BRAAM_LLVM")
    set(BRAAM_LLVM "${BRAAM_WASI_SDK}" CACHE PATH "clang distribution; nothing is linked from it")
endif()

# Homebrew first, then the wasi-sdk that CI installs. The last entry is also the
# fallback, so a machine with neither names it in the error.
set(_braam_llvm_candidates /usr/local/opt/llvm /opt/homebrew/opt/llvm /opt/wasi-sdk-33.0)
foreach(_prefix IN LISTS _braam_llvm_candidates)
    set(_braam_llvm_default "${_prefix}")
    if(EXISTS "${_prefix}/bin/clang++")
        break()
    endif()
endforeach()
set(BRAAM_LLVM "${_braam_llvm_default}" CACHE PATH "clang distribution; nothing is linked from it")

# Homebrew's llvm ships no linker; wasm-ld comes from its separate lld formula.
if(NOT EXISTS "${BRAAM_LLVM}/bin/wasm-ld")
    find_program(BRAAM_WASM_LD wasm-ld)
    if(NOT BRAAM_WASM_LD)
        message(FATAL_ERROR "no wasm-ld in ${BRAAM_LLVM}/bin or on PATH (brew install lld)")
    endif()
endif()

set(CMAKE_C_COMPILER   "${BRAAM_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "${BRAAM_LLVM}/bin/clang++")
set(CMAKE_AR           "${BRAAM_LLVM}/bin/llvm-ar")
set(CMAKE_RANLIB       "${BRAAM_LLVM}/bin/llvm-ranlib")

set(CMAKE_C_COMPILER_TARGET   wasm32-unknown-unknown)
set(CMAKE_CXX_COMPILER_TARGET wasm32-unknown-unknown)

# The compiler probe must build a static library; linking an executable needs
# flags that only the project sets.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXECUTABLE_SUFFIX     ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".wasm")

# --no-default-config suppresses bin/clang++.cfg, which in wasi-sdk injects a sysroot.
set(CMAKE_C_FLAGS_INIT          "--no-default-config -nostdlib")
set(CMAKE_CXX_FLAGS_INIT        "--no-default-config -nostdlib -nostdinc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--no-default-config -nostdlib")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
