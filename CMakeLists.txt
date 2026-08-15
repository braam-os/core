cmake_minimum_required(VERSION 3.24)

project(braam LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
    message(FATAL_ERROR
        "braam builds only for freestanding wasm32. Use `make`, or configure with:\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-unknown-unknown.cmake")
endif()

# The build type comes from the toolchain file, which defaults it to MinSizeRel
# for everyone who uses it: an unoptimised freestanding build does not link.

option(BRAAM_WERROR "Treat warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # statement expressions, used by TRY()

set(BRAAM_STACK_SIZE 131072 CACHE STRING "Shadow stack size in bytes")

# Where `make install` puts the SDK, relative to the prefix. Fixed paths, not
# GNUInstallDirs: the tree is zipped and unpacked anywhere, and the package
# finds itself by walking up from lib/cmake/braam.
set(BRAAM_INSTALL_INCLUDEDIR  include/braam)
set(BRAAM_INSTALL_LIBDIR      lib/braam)
set(BRAAM_INSTALL_CMAKEDIR    lib/cmake/braam)
set(BRAAM_INSTALL_LIBEXECDIR  libexec/braam)
set(BRAAM_INSTALL_EXAMPLESDIR share/braam/examples)
set(BRAAM_INSTALL_DOCDIR      share/doc/braam)

# Flags shared by every wasm target.
add_library(braam_flags INTERFACE)

# On the interface, not just the project: Task is a coroutine type, so a
# consumer at the compiler's default would fail inside task.h.
target_compile_features(braam_flags INTERFACE cxx_std_20)

target_compile_options(braam_flags INTERFACE
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -ffunction-sections -fdata-sections
    -Wall -Wextra -Wshadow)
# Build-interface only: the SDK must not impose this tree's warning policy on
# a consumer's code.
if(BRAAM_WERROR)
    target_compile_options(braam_flags INTERFACE $<BUILD_INTERFACE:-Werror>)
endif()

# No --allow-undefined: an accidental libc dependency must fail at link.
# No --export-dynamic: exports are named individually with export_name().
# --stack-first puts the shadow stack below the data segment, so overflow traps.
target_link_options(braam_flags INTERFACE
    -Wl,--no-entry
    -Wl,--gc-sections
    -Wl,--stack-first
    -Wl,-z,stack-size=${BRAAM_STACK_SIZE})

target_include_directories(braam_flags INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/gen>
    $<INSTALL_INTERFACE:${BRAAM_INSTALL_INCLUDEDIR}>)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The version ends in the commit count and hash, so it must be taken at build
# time: a commit does not re-run cmake, and a configure-time count would name a
# release after whatever the tree was when it was configured. The target always
# runs and the script rewrites the header only when the revision moves.
set(BRAAM_VERSION_SOURCE ${CMAKE_SOURCE_DIR}/src/kernel/version.h)
set(BRAAM_REVISION_HEADER ${CMAKE_BINARY_DIR}/gen/kernel/revision.h)
add_custom_target(revision ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/version.py
            --source ${BRAAM_VERSION_SOURCE} --header ${BRAAM_REVISION_HEADER}
    BYPRODUCTS ${BRAAM_REVISION_HEADER}
    VERBATIM)

add_subdirectory(src/kernel)
add_subdirectory(src/fs)
add_subdirectory(src/svc)
add_subdirectory(src/ui)
add_subdirectory(src/user)
add_subdirectory(src/proc)
add_subdirectory(src/sh)

# The program recipe, shared with the SDK: it names the targets a consumer
# imports, so this tree aliases them to the same names.
add_library(braam::proc  ALIAS braam_proc)
add_library(braam::ui    ALIAS braam_ui)
add_library(braam::flags ALIAS braam_flags)
set(BRAAM_STAMP ${CMAKE_SOURCE_DIR}/tools/stamp.py)
set(BRAAM_SYSABI ${CMAKE_SOURCE_DIR}/src/kernel/sysabi.h)
include(cmake/BraamProgram.cmake)

add_subdirectory(src/cmd)

# Built like any other program, and not packed into the bundle: it is the SDK's
# worked example, and compiling it here is what keeps it from rotting.
add_subdirectory(examples/hello)

# kernel.wasm — main.cpp holds the exports, so it links into the executable
# directly; --gc-sections would not pull it out of an archive.
add_executable(kernel src/kernel/main.cpp)
target_link_libraries(kernel PRIVATE
    braam_user braam_fs braam_svc braam_core braam_flags)

# Every target with a translation unit that includes version.h, named one by
# one: linking braam_core would order the two under Makefiles and not under
# Ninja, which is free to compile a dependent's objects early.
add_dependencies(kernel revision)
add_dependencies(braam_user revision)
add_dependencies(bin_version revision)

# bundle/ is packed rather than copied: BundleFs reads one archive, so the
# worker loads one file instead of fetching a tree file by file. The process
# binaries are staged into a copy of it first, since they are build outputs and
# /bin/echo must be a plain name rather than echo.wasm.
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

# The kernel and the boot archive. A program is measured but not bounded: what
# one costs is §4.4's duplication, which the archive's number already shows.
set(BRAAM_SIZE_BUDGET_CHECK
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/size_budget.py
    --budgets ${CMAKE_SOURCE_DIR}/tools/size_budget.txt
    $<TARGET_FILE:kernel> ${CMAKE_BINARY_DIR}/web/bundle.bin)

# Only the kernel here: BRAAM_SIZE_BUDGET_CHECK covers the archive too, from
# the `size` test, and it cannot run before the archive is packed.
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

# The SDK — doc/Programming_Manual.md. Two libraries and no more: braam_core,
# braam_fs, braam_svc and braam_user each carry a host import no binary may have.
install(TARGETS braam_proc braam_ui braam_flags
        EXPORT braam-targets
        ARCHIVE DESTINATION ${BRAAM_INSTALL_LIBDIR})
set_target_properties(braam_proc  PROPERTIES EXPORT_NAME proc)
set_target_properties(braam_ui    PROPERTIES EXPORT_NAME ui)
set_target_properties(braam_flags PROPERTIES EXPORT_NAME flags)
install(EXPORT braam-targets
        NAMESPACE braam::
        DESTINATION ${BRAAM_INSTALL_CMAKEDIR})

# Whole directories rather than proc/io.h's include closure, which would fall
# behind. Layout preserved: an include within a directory is a bare name.
foreach(dir kernel fs proc ui)
    install(DIRECTORY src/${dir}/
            DESTINATION ${BRAAM_INSTALL_INCLUDEDIR}/${dir}
            FILES_MATCHING PATTERN "*.h")
endforeach()
# Generated, and kernel/version.h includes it: the SDK names the revision it
# was cut from.
install(FILES ${BRAAM_REVISION_HEADER} DESTINATION ${BRAAM_INSTALL_INCLUDEDIR}/kernel)

include(CMakePackageConfigHelpers)
set(BRAAM_CONFIG ${CMAKE_BINARY_DIR}/gen/cmake/braamConfig.cmake)
configure_package_config_file(
    cmake/braamConfig.cmake.in ${BRAAM_CONFIG}
    INSTALL_DESTINATION ${BRAAM_INSTALL_CMAKEDIR}
    PATH_VARS BRAAM_INSTALL_INCLUDEDIR BRAAM_INSTALL_LIBEXECDIR BRAAM_INSTALL_EXAMPLESDIR)
install(FILES ${BRAAM_CONFIG}
              cmake/BraamProgram.cmake
              cmake/wasm32-unknown-unknown.cmake
        DESTINATION ${BRAAM_INSTALL_CMAKEDIR})

install(PROGRAMS tools/stamp.py DESTINATION ${BRAAM_INSTALL_LIBEXECDIR})
install(DIRECTORY examples/hello DESTINATION ${BRAAM_INSTALL_EXAMPLESDIR})
install(FILES doc/Programming_Manual.md LICENSE DESTINATION ${BRAAM_INSTALL_DOCDIR})

# Last, and said where it is read: the toolchain file is the one thing a
# consumer has to name, and only on the first configure.
install(CODE "message(STATUS \"Braam SDK installed. Build a program with:\")")
install(CODE "message(STATUS \"  cmake -B build --toolchain \
\${CMAKE_INSTALL_PREFIX}/${BRAAM_INSTALL_CMAKEDIR}/wasm32-unknown-unknown.cmake\")")

# Two archives, each under one versioned directory: the site, and the SDK. Not
# in ALL — an ordinary build should not repack them. The SDK is staged by
# installing into build/sdk, so the zip and `make install` cannot disagree; like
# build/web/, that stage is never emptied, so release from a clean tree.
add_custom_target(release
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/release.py
            ${CMAKE_BINARY_DIR}/web ${CMAKE_BINARY_DIR}
            --version-file ${BRAAM_VERSION_SOURCE}
            --extra ${CMAKE_SOURCE_DIR}/LICENSE
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
            --prefix ${CMAKE_BINARY_DIR}/sdk
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/release.py
            ${CMAKE_BINARY_DIR}/sdk ${CMAKE_BINARY_DIR}
            --version-file ${BRAAM_VERSION_SOURCE}
            --name braam-sdk
            --require ${BRAAM_INSTALL_CMAKEDIR}/braamConfig.cmake
            --require ${BRAAM_INSTALL_LIBDIR}/libbraam_proc.a
            --require ${BRAAM_INSTALL_INCLUDEDIR}/proc/io.h
    DEPENDS web
    VERBATIM
    COMMENT "Packing the site and the SDK into deployable archives")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
