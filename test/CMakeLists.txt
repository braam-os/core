add_executable(tests
    unit/main.cpp
    unit/harness.cpp
    unit/test_alloc.cpp
    unit/test_channel.cpp
    unit/test_cond.cpp
    unit/test_console.cpp
    unit/test_coroutine.cpp
    unit/test_encode.cpp
    unit/test_fmt.cpp
    unit/test_hash.cpp
    unit/test_hostfs.cpp
    unit/test_jsref.cpp
    unit/test_io.cpp
    unit/test_match.cpp
    unit/test_opt.cpp
    unit/test_pane.cpp
    unit/test_path.cpp
    unit/test_parse.cpp
    unit/test_procfs.cpp
    unit/test_result.cpp
    unit/test_sched.cpp
    unit/test_screen.cpp
    unit/test_sha256.cpp
    unit/test_expand.cpp
    unit/test_str.cpp
    unit/test_tokenize.cpp
    unit/test_string.cpp
    unit/test_svc.cpp
    unit/test_sysabi.cpp
    unit/test_task.cpp
    unit/test_text.cpp
    unit/test_textbuf.cpp
    unit/test_time.cpp
    unit/test_tty.cpp
    unit/test_vec.cpp
    unit/test_version.cpp
    unit/test_vfs.cpp

    # The grammar, the word expander, the pattern matcher and `test`'s
    # expression are the shell binary's, and pure: they touch nothing but Str,
    # String and Vec, so they are compiled in here rather than linked from
    # braam_sh, which would drag the process runtime's imports in with them.
    # glob.cpp is the walk over the store and does not belong here, and neither
    # does condrun.cpp, which is what answers cond.cpp's probes.
    ../src/cmd/sh/cond.cpp
    ../src/cmd/sh/expand.cpp
    ../src/cmd/sh/match.cpp
    ../src/cmd/sh/parse.cpp
    ../src/cmd/sh/tokenize.cpp

    # The option parser and the calendar are the process runtime's, and pure
    # for the same reason: Args, Str and Result and nothing else. braam_proc is
    # not linked here.
    ../src/proc/opt.cpp
    ../src/proc/time.cpp

    # pkg's digest, its encodings and the version grammar, pure and
    # allocation-free, so braam_pkg is not linked here either.
    ../src/cmd/pkg/encode.cpp
    ../src/cmd/pkg/sha256.cpp
    ../src/cmd/pkg/version.cpp)

target_link_libraries(tests PRIVATE
    braam_user braam_fs braam_svc braam_ui braam_core braam_flags)

find_program(BRAAM_NODE node REQUIRED)

# The archive comes along so that the packer and the unpacker in web/fs.js are
# checked against each other, not each against its own reading of the format.
# Every binary follows: the surface and the metadata are an ABI of their own,
# and the exact-import assertion is what proves none picked up a host import.
foreach(prog IN LISTS BRAAM_BIN_PROGRAMS)
    list(APPEND BRAAM_SMOKE_BINARIES $<TARGET_FILE:bin_${prog}>)
endforeach()

add_test(NAME smoke
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --kernel $<TARGET_FILE:kernel>
            ${CMAKE_BINARY_DIR}/web/rootfs.zip ${BRAAM_SMOKE_BINARIES})

add_test(NAME unit
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --tests $<TARGET_FILE:tests>)

add_test(NAME size COMMAND ${BRAAM_SIZE_BUDGET_CHECK})
