add_executable(tests
    unit/main.cpp
    unit/harness.cpp
    unit/test_alloc.cpp
    unit/test_bundlefs.cpp
    unit/test_channel.cpp
    unit/test_coroutine.cpp
    unit/test_edit.cpp
    unit/test_fmt.cpp
    unit/test_hash.cpp
    unit/test_hostfs.cpp
    unit/test_jsref.cpp
    unit/test_io.cpp
    unit/test_jobs.cpp
    unit/test_memfs.cpp
    unit/test_pane.cpp
    unit/test_path.cpp
    unit/test_parse.cpp
    unit/test_procfs.cpp
    unit/test_prog.cpp
    unit/test_result.cpp
    unit/test_sched.cpp
    unit/test_screen.cpp
    unit/test_shell.cpp
    unit/test_str.cpp
    unit/test_tokenize.cpp
    unit/test_string.cpp
    unit/test_svc.cpp
    unit/test_sysabi.cpp
    unit/test_task.cpp
    unit/test_text.cpp
    unit/test_textbuf.cpp
    unit/test_vec.cpp
    unit/test_vfs.cpp)

target_link_libraries(tests PRIVATE
    braam_prog braam_user braam_fs braam_svc braam_ui braam_core braam_flags)

find_program(BRAAM_NODE node REQUIRED)

# The bundle comes along so that the packer and BundleFs are checked against
# each other, not each against its own reading of the format. One binary per
# tier follows, since the surface and the metadata are an ABI of their own.
add_test(NAME smoke
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --kernel $<TARGET_FILE:kernel>
            ${CMAKE_BINARY_DIR}/web/bundle.bin $<TARGET_FILE:bin_wc> $<TARGET_FILE:bin_spin>)

add_test(NAME unit
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --tests $<TARGET_FILE:tests>)

add_test(NAME size COMMAND ${BRAAM_SIZE_BUDGET_CHECK})
