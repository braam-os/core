# One binary per program, each its own wasm module (Concept.md §4.1). The recipe
# itself is cmake/BraamProgram.cmake, which the SDK installs — so a program built
# out of tree is built the way these are.

# Two programs have a directory of their own, and the library each builds is
# linked below like any other.
add_subdirectory(pkg)
add_subdirectory(sh)

set(BRAAM_BIN_LIST cat chat clear curl date df echo edit env false fexport fimport grep head hog
                   less ln ls mkdir mount mv pbcopy pbpaste pkg ps pwd rm sh sleep spin tail
                   test timeout touch true uname unzip vmstat watch wc)
set(BRAAM_BIN_PROGRAMS ${BRAAM_BIN_LIST} PARENT_SCOPE)

# Every one of them takes a worker of its own, the shell included. There is
# nothing to ask for: it is what a program is (Concept.md §4).

# A program that is more than one translation unit names the library that holds
# the rest of it, and where its own source is when that is not <name>.cpp.
set(BRAAM_BIN_LIB_pkg braam_pkg)
set(BRAAM_BIN_SRC_pkg pkg/main.cpp)

set(BRAAM_BIN_LIB_sh braam_sh)
set(BRAAM_BIN_SRC_sh sh/main.cpp)

# ...or names the extra sources outright. /bin/test is the shell's expression
# evaluator over a proc_main, and linking braam_sh for it would drag the whole
# shell in for two files.
set(BRAAM_BIN_SRC_test test.cpp sh/cond.cpp sh/condrun.cpp)

# /bin/unzip is the package manager's zip reader over an archive that need not
# be a package. The two files rather than braam_pkg, for the same reason.
set(BRAAM_BIN_SRC_unzip unzip.cpp pkg/zip.cpp pkg/unzip.cpp)

# An undefined LIBS expands to nothing, which is the recipe's default.
foreach(prog IN LISTS BRAAM_BIN_LIST)
    set(src ${BRAAM_BIN_SRC_${prog}})
    if(NOT src)
        set(src ${prog}.cpp)
    endif()
    braam_add_program(NAME ${prog}
                      SOURCES ${src}
                      LIBS ${BRAAM_BIN_LIB_${prog}})
endforeach()
