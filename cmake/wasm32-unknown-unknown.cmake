# Toolchain: freestanding wasm32. The SDK is used as a clang distribution only —
# none of its runtime and none of its headers are linked or included.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(BRAAM_WASI_SDK "/opt/wasi-sdk-33.0" CACHE PATH "clang distribution; nothing is linked from it")

set(CMAKE_C_COMPILER   "${BRAAM_WASI_SDK}/bin/clang")
set(CMAKE_CXX_COMPILER "${BRAAM_WASI_SDK}/bin/clang++")
set(CMAKE_AR           "${BRAAM_WASI_SDK}/bin/llvm-ar")
set(CMAKE_RANLIB       "${BRAAM_WASI_SDK}/bin/llvm-ranlib")

set(CMAKE_C_COMPILER_TARGET   wasm32-unknown-unknown)
set(CMAKE_CXX_COMPILER_TARGET wasm32-unknown-unknown)

# The compiler probe must build a static library; linking an executable needs
# flags that only the project sets.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXECUTABLE_SUFFIX     ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".wasm")

# --no-default-config suppresses bin/clang++.cfg, which injects a wasi sysroot.
set(CMAKE_C_FLAGS_INIT          "--no-default-config -nostdlib")
set(CMAKE_CXX_FLAGS_INIT        "--no-default-config -nostdlib -nostdinc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--no-default-config -nostdlib")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
