# How a Braam program is built. Used by src/cmd/ and installed with the SDK, so
# an out-of-tree program is built the way the system's own are.
#
#   braam_add_program(NAME hello SOURCES hello.cpp [LIBS ...])
#
# BRAAM_STAMP names tools/stamp.py, BRAAM_SYSABI the kernel/sysabi.h it reads
# PROC_ABI from; the includer sets both.

set(BRAAM_BIN_INITIAL_PAGES 4)
set(BRAAM_BIN_MAX_PAGES 256)
math(EXPR BRAAM_BIN_INITIAL_BYTES "${BRAAM_BIN_INITIAL_PAGES} * 65536")

function(braam_add_program)
    cmake_parse_arguments(P "" "NAME" "SOURCES;LIBS" ${ARGN})
    # Compared as strings: one of the programs is named `false`, which
    # if(NOT P_NAME) would dereference to exactly that.
    if("${P_NAME}" STREQUAL "" OR "${P_SOURCES}" STREQUAL "")
        message(FATAL_ERROR "braam_add_program: NAME and SOURCES are required")
    endif()

    # The target carries a prefix the file does not: /bin/echo is a plain name.
    add_executable(bin_${P_NAME} ${P_SOURCES})
    set_target_properties(bin_${P_NAME} PROPERTIES OUTPUT_NAME ${P_NAME})
    target_link_libraries(bin_${P_NAME} PRIVATE ${P_LIBS} braam::proc braam::flags)

    # --import-memory makes the 16 MB cap the kernel's decision rather than the
    # binary's claim. The stamp repeats the initial size the link used.
    target_link_options(bin_${P_NAME} PRIVATE
        -Wl,--import-memory
        -Wl,--initial-memory=${BRAAM_BIN_INITIAL_BYTES})

    add_custom_command(TARGET bin_${P_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${BRAAM_STAMP}
                $<TARGET_FILE:bin_${P_NAME}>
                --sysabi ${BRAAM_SYSABI}
                --initial-pages ${BRAAM_BIN_INITIAL_PAGES}
                --max-pages ${BRAAM_BIN_MAX_PAGES}
        VERBATIM)
endfunction()
