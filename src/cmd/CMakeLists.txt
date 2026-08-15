# One binary per program, each its own wasm module (Concept.md §4.1). The recipe
# itself is cmake/BraamProgram.cmake, which the SDK installs — so a program built
# out of tree is built the way these are.
set(BRAAM_BIN_LIST cat chat clear curl date df echo edit export false grep head hog
                   import less ls mkdir mount pbcopy pbpaste pwd rm sh sleep spin tail
                   timeout touch true version watch wc)
set(BRAAM_BIN_PROGRAMS ${BRAAM_BIN_LIST} PARENT_SCOPE)

# Tier 3 is a worker of its own, and what a program has to be to earn one is
# that it may not come back. Everything else runs at tier 2, where a syscall is
# a call rather than two messages.
set(BRAAM_BIN_TIER_spin 3)
set(BRAAM_BIN_TIER_tail 3)

# A program that is more than one translation unit names the library that holds
# the rest of it. Only the shell is, and it stays at tier 2: a tier-3 prompt
# would pay two postMessage hops for every keystroke.
set(BRAAM_BIN_LIB_sh braam_sh)

foreach(prog IN LISTS BRAAM_BIN_LIST)
    set(tier 2)
    if(DEFINED BRAAM_BIN_TIER_${prog})
        set(tier ${BRAAM_BIN_TIER_${prog}})
    endif()
    braam_add_program(NAME ${prog}
                      SOURCES ${prog}.cpp
                      TIER ${tier}
                      LIBS ${BRAAM_BIN_LIB_${prog}})
endforeach()
