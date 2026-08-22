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

# A package zip, Package_Formats.md §5. BRAAM_MKPKG names tools/mkpkg.py.
#
#   braam_add_package(NAME hello VERSION 1.0-r0
#                     [FIELD T=a greeting] [FIELD D=libz]...
#                     FILES $<TARGET_FILE:bin_hello>=bin/hi ...)
#
# FILES takes §10's `<src>=<entry>` pairs. `bin/` is what reaches PATH: every
# flat entry becomes a link in the installed generation's bin/ (§8.3) and a
# `cmd:<entry>` provide (§6.1), so the entry's leaf is the command's name.
#
# Defines target pkg_<name> producing <name>-<version>.zip, and it is not in
# ALL: packing is not part of an ordinary build.
function(braam_add_package)
    cmake_parse_arguments(P "" "NAME;VERSION" "FIELD;FILES" ${ARGN})
    if("${P_NAME}" STREQUAL "" OR "${P_VERSION}" STREQUAL "" OR
       "${P_FILES}" STREQUAL "")
        message(FATAL_ERROR "braam_add_package: NAME, VERSION and FILES are required")
    endif()

    set(zip ${CMAKE_CURRENT_BINARY_DIR}/${P_NAME}-${P_VERSION}.zip)

    # A version bump leaves the old zip in the build tree, where whatever
    # collects packages would find it and publish a version the tree can no
    # longer build. Configure time is the moment to drop it: a bump edits this
    # call, which reconfigures.
    file(GLOB stale ${CMAKE_CURRENT_BINARY_DIR}/${P_NAME}-*.zip)
    foreach(f IN LISTS stale)
        if(NOT f STREQUAL zip)
            file(REMOVE ${f})
        endif()
    endforeach()

    # A value with a space in it has to be quoted at the call, or CMake splits
    # it into arguments of its own and mkpkg.py drops the remainder as a letter
    # it does not know. Refuse what does not look like <L>=<value> rather than
    # packing a truncated description.
    set(fields "")
    foreach(f IN LISTS P_FIELD)
        if(NOT f MATCHES "^[A-Za-z]=")
            message(FATAL_ERROR
                "braam_add_package: FIELD ${f} is not <letter>=<value>"
                " (quote a value that has a space in it)")
        endif()
        list(APPEND fields --field ${f})
    endforeach()

    # The sources of the <src>=<entry> pairs, so that rebuilding a payload file
    # repacks. A generator expression is a dependency CMake already tracks.
    set(sources "")
    foreach(pair IN LISTS P_FILES)
        string(FIND "${pair}" "=" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "braam_add_package: ${pair} is not <src>=<entry>")
        endif()
        string(SUBSTRING "${pair}" 0 ${at} src)
        list(APPEND sources ${src})
    endforeach()

    add_custom_command(OUTPUT ${zip}
        COMMAND ${Python3_EXECUTABLE} ${BRAAM_MKPKG}
                --out ${zip} --name ${P_NAME} --version ${P_VERSION}
                ${fields} ${P_FILES}
        DEPENDS ${sources}
        COMMENT "Packing ${P_NAME}-${P_VERSION}.zip"
        VERBATIM)
    add_custom_target(pkg_${P_NAME} DEPENDS ${zip})
endfunction()
