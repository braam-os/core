# One binary per program, each its own wasm module (Concept.md §4.1). They are
# packed into the bundle as /usr/bin/<name>, so the target name carries a
# prefix the file does not.
#
# --import-memory is what makes the 16 MB cap the kernel's decision rather than
# the binary's claim: the module declares no maximum, and the host supplies a
# Memory with one. The initial size is stamped into the binary's metadata from
# the same number the link uses, so the two cannot disagree.
set(BRAAM_BIN_LIST cat chat clear curl date df echo edit export false grep head hog
                   import less ls mkdir mount pbcopy pbpaste pwd rm sleep spin tail
                   touch true version wc)
set(BRAAM_BIN_PROGRAMS ${BRAAM_BIN_LIST} PARENT_SCOPE)
set(BRAAM_BIN_INITIAL_PAGES 4)
set(BRAAM_BIN_MAX_PAGES 256)
math(EXPR BRAAM_BIN_INITIAL_BYTES "${BRAAM_BIN_INITIAL_PAGES} * 65536")

# The tier is the binary's own claim, stamped in beside the page counts: tier 3
# is a worker of its own, and what a program has to be to earn one is that it
# may not come back. Everything else runs at tier 2, where a syscall is a call
# rather than two messages.
set(BRAAM_BIN_TIER_spin 3)
set(BRAAM_BIN_TIER_tail 3)

foreach(prog IN LISTS BRAAM_BIN_LIST)
    set(tier 2)
    if(DEFINED BRAAM_BIN_TIER_${prog})
        set(tier ${BRAAM_BIN_TIER_${prog}})
    endif()
    add_executable(bin_${prog} ${prog}.cpp)
    set_target_properties(bin_${prog} PROPERTIES OUTPUT_NAME ${prog})
    target_link_libraries(bin_${prog} PRIVATE braam_proc braam_flags)
    target_link_options(bin_${prog} PRIVATE
        -Wl,--import-memory
        -Wl,--initial-memory=${BRAAM_BIN_INITIAL_BYTES})
    add_custom_command(TARGET bin_${prog} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/stamp.py
                $<TARGET_FILE:bin_${prog}>
                --tier ${tier}
                --initial-pages ${BRAAM_BIN_INITIAL_PAGES}
                --max-pages ${BRAAM_BIN_MAX_PAGES}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/size_budget.py
                --budgets ${CMAKE_SOURCE_DIR}/tools/size_budget.txt
                $<TARGET_FILE:bin_${prog}>
        VERBATIM)
endforeach()
