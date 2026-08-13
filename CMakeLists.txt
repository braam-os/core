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

add_subdirectory(src/kernel)

# kernel.wasm — main.cpp holds the exports, so it links into the executable
# directly; --gc-sections would not pull it out of an archive.
add_executable(kernel src/kernel/main.cpp)
target_link_libraries(kernel PRIVATE braam_core braam_flags)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(BRAAM_SIZE_BUDGET_CHECK
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/size_budget.py
    --budgets ${CMAKE_SOURCE_DIR}/tools/size_budget.txt
    $<TARGET_FILE:kernel>)

add_custom_command(TARGET kernel POST_BUILD
    COMMAND ${BRAAM_SIZE_BUDGET_CHECK}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:kernel> ${CMAKE_BINARY_DIR}/web/kernel.wasm
    VERBATIM)

add_custom_target(serve
    COMMAND ${Python3_EXECUTABLE} -m http.server --directory ${CMAKE_BINARY_DIR}/web 8080
    DEPENDS kernel
    USES_TERMINAL
    COMMENT "Serving ${CMAKE_BINARY_DIR}/web at http://localhost:8080/")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
