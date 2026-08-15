# Toolchain: freestanding wasm32. Plain clang with the wasm32 target and wasm-ld,
# used as a compiler only — none of its runtime and none of its headers are linked
# or included. BRAAM_LLVM names a distribution's prefix; without it, PATH is used.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Homebrew keeps its llvm keg off PATH, so name the two prefixes it uses.
if(NOT BRAAM_LLVM)
    foreach(_prefix IN ITEMS /usr/local/opt/llvm /opt/homebrew/opt/llvm)
        if(EXISTS "${_prefix}/bin/clang++")
            set(BRAAM_LLVM "${_prefix}")
            break()
        endif()
    endforeach()
endif()
set(BRAAM_LLVM "${BRAAM_LLVM}" CACHE PATH "clang distribution; nothing is linked from it")

find_program(BRAAM_CC      NAMES clang       HINTS "${BRAAM_LLVM}/bin")
find_program(BRAAM_CXX     NAMES clang++     HINTS "${BRAAM_LLVM}/bin")
find_program(BRAAM_AR      NAMES llvm-ar     HINTS "${BRAAM_LLVM}/bin")
find_program(BRAAM_RANLIB  NAMES llvm-ranlib HINTS "${BRAAM_LLVM}/bin")
# clang finds the linker itself; this is only so a missing one is named at
# configure time. Homebrew's llvm ships none — it comes from its lld formula.
find_program(BRAAM_WASM_LD NAMES wasm-ld     HINTS "${BRAAM_LLVM}/bin")

foreach(_tool BRAAM_CC BRAAM_CXX BRAAM_AR BRAAM_RANLIB BRAAM_WASM_LD)
    if(NOT ${_tool})
        message(FATAL_ERROR "clang toolchain incomplete: ${_tool} not found "
                            "(brew install llvm lld, or apt install clang lld llvm)")
    endif()
endforeach()

set(CMAKE_C_COMPILER   "${BRAAM_CC}")
set(CMAKE_CXX_COMPILER "${BRAAM_CXX}")
set(CMAKE_AR           "${BRAAM_AR}")
set(CMAKE_RANLIB       "${BRAAM_RANLIB}")

set(CMAKE_C_COMPILER_TARGET   wasm32-unknown-unknown)
set(CMAKE_CXX_COMPILER_TARGET wasm32-unknown-unknown)

# Optimised by default: -O0 emits the libcalls the optimiser folds away
# (__builtin_strlen, an outlined memcpy), and nothing provides one. A cache
# entry from the command line still wins.
set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "")

# The compiler probe must build a static library; linking an executable needs
# flags that only the project sets.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXECUTABLE_SUFFIX     ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".wasm")

# The wasm features the code needs, named rather than left to the compiler's
# default CPU, which differs between clang versions: reference-types for
# __externref_t, bulk-memory for inline memcpy/memset, the rest for parity with
# what a recent clang enables anyway.
set(BRAAM_WASM_FEATURES "-mreference-types -mbulk-memory -msign-ext -mmutable-globals \
-mnontrapping-fptoint")

# --no-default-config suppresses bin/clang++.cfg, which some distributions use
# to inject a sysroot.
set(CMAKE_C_FLAGS_INIT          "--no-default-config -nostdlib ${BRAAM_WASM_FEATURES}")
set(CMAKE_CXX_FLAGS_INIT        "--no-default-config -nostdlib -nostdinc++ ${BRAAM_WASM_FEATURES}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--no-default-config -nostdlib")

# The installed copy sits beside braamConfig.cmake; the source tree's does not.
# Named rather than searched for: PACKAGE mode above is ONLY, with no root path.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/braamConfig.cmake")
    set(braam_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH "the Braam SDK")
endif()
