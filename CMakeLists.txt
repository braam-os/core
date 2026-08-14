cmake_minimum_required(VERSION 3.24)

project(braam LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
    message(FATAL_ERROR
        "braam builds only for freestanding wasm32. Use `make`, or configure with:\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-unknown-unknown.cmake")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

option(BRAAM_WERROR "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # statement expressions, used by TRY()

set(BRAAM_STACK_SIZE 131072 CACHE STRING "Shadow stack size in bytes")

# Flags shared by every wasm target.
add_library(braam_flags INTERFACE)
target_compile_options(braam_flags INTERFACE
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -ffunction-sections -fdata-sections
    -Wall -Wextra)
if(BRAAM_WERROR)
    target_compile_options(braam_flags INTERFACE -Werror)
endif()

# No --allow-undefined: an accidental libc dependency must fail at link.
# No --export-dynamic: exports are named individually with export_name().
# --stack-first puts the shadow stack below the data segment, so overflow traps.
target_link_options(braam_flags INTERFACE
    -Wl,--no-entry
    -Wl,--gc-sections
    -Wl,--stack-first
    -Wl,-z,stack-size=${BRAAM_STACK_SIZE})

target_include_directories(braam_flags INTERFACE ${CMAKE_SOURCE_DIR}/src)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(src/kernel)
add_subdirectory(src/fs)
add_subdirectory(src/svc)
add_subdirectory(src/ui)
add_subdirectory(src/user)
add_subdirectory(src/proc)
add_subdirectory(src/cmd)

# kernel.wasm — main.cpp holds the exports, so it links into the executable
# directly; --gc-sections would not pull it out of an archive.
add_executable(kernel src/kernel/main.cpp)
target_link_libraries(kernel PRIVATE
    braam_user braam_fs braam_svc braam_core braam_flags)

# bundle/ is packed rather than copied: BundleFs reads one archive, so the
# worker loads one file instead of fetching a tree file by file. The process
# binaries are staged into a copy of it first, since they are build outputs and
# /usr/bin/echo must be a plain name rather than echo.wasm.
set(BRAAM_BUNDLE_STAGE ${CMAKE_BINARY_DIR}/bundle)
set(BRAAM_PACK_BUNDLE
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/pack.py
    ${BRAAM_BUNDLE_STAGE} ${CMAKE_BINARY_DIR}/web/bundle.bin)

foreach(prog IN LISTS BRAAM_BIN_PROGRAMS)
    list(APPEND BRAAM_BIN_TARGETS bin_${prog})
    list(APPEND BRAAM_BINARIES $<TARGET_FILE:bin_${prog}>)
    list(APPEND BRAAM_STAGE_BINARIES
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bin_${prog}> ${BRAAM_BUNDLE_STAGE}/bin/${prog})
endforeach()

set(BRAAM_SIZE_BUDGET_CHECK
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/size_budget.py
    --budgets ${CMAKE_SOURCE_DIR}/tools/size_budget.txt
    $<TARGET_FILE:kernel> ${BRAAM_BINARIES} ${CMAKE_BINARY_DIR}/web/bundle.bin)

# Only the kernel here: the binaries are checked by their own POST_BUILD, and
# BRAAM_SIZE_BUDGET_CHECK covers everything from the `size` test.
add_custom_command(TARGET kernel POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/size_budget.py
            --budgets ${CMAKE_SOURCE_DIR}/tools/size_budget.txt $<TARGET_FILE:kernel>
    VERBATIM)

# Assembling build/web/ is its own target, and it always runs: hanging it off
# the kernel's POST_BUILD meant that editing only a file under web/ left the
# served copy stale, since the kernel had nothing to relink.
add_custom_target(web ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:kernel> ${CMAKE_BINARY_DIR}/web/kernel.wasm
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/bundle ${BRAAM_BUNDLE_STAGE}
    ${BRAAM_STAGE_BINARIES}
    COMMAND ${BRAAM_PACK_BUNDLE}
    DEPENDS kernel ${BRAAM_BIN_TARGETS}
    VERBATIM)

add_custom_target(serve
    COMMAND ${Python3_EXECUTABLE} -m http.server --directory ${CMAKE_BINARY_DIR}/web 8080
    DEPENDS web
    USES_TERMINAL
    COMMENT "Serving ${CMAKE_BINARY_DIR}/web at http://localhost:8080/")

# The deployable archive: build/web/ as it stands, under one versioned
# directory. Not in ALL — an ordinary build should not repack it.
add_custom_target(release
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/release.py
            ${CMAKE_BINARY_DIR}/web ${CMAKE_BINARY_DIR}
            --version-file ${CMAKE_SOURCE_DIR}/src/kernel/version.h
            --extra ${CMAKE_SOURCE_DIR}/LICENSE
    DEPENDS web
    VERBATIM
    COMMENT "Packing ${CMAKE_BINARY_DIR}/web into a deployable archive")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
