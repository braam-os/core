# One binary per program, each its own wasm module (Concept.md §4.1). The recipe
# itself is cmake/BraamProgram.cmake, which the SDK installs — so a program built
# out of tree is built the way these are.
set(BRAAM_BIN_LIST cat chat clear curl date df echo edit export false grep head hog
                   import less ls mkdir mount pbcopy pbpaste pwd rm sh sleep spin tail
                   timeout touch true version watch wc)
set(BRAAM_BIN_PROGRAMS ${BRAAM_BIN_LIST} PARENT_SCOPE)

# Every one of them takes a worker of its own, the shell included, which is what
# the recipe gives a program and what nothing here asks out of (doc/TODO.md T8).

# A program that is more than one translation unit names the library that holds
# the rest of it. Only the shell is.
set(BRAAM_BIN_LIB_sh braam_sh)

# An undefined LIBS expands to nothing, which is the recipe's default.
foreach(prog IN LISTS BRAAM_BIN_LIST)
    braam_add_program(NAME ${prog}
                      SOURCES ${prog}.cpp
                      LIBS ${BRAAM_BIN_LIB_${prog}})
endforeach()
