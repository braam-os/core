add_executable(tests
    unit/main.cpp
    unit/harness.cpp
    unit/test_alloc.cpp
    unit/test_coroutine.cpp
    unit/test_fmt.cpp
    unit/test_hash.cpp
    unit/test_result.cpp
    unit/test_sched.cpp
    unit/test_str.cpp
    unit/test_string.cpp
    unit/test_task.cpp
    unit/test_vec.cpp)

target_link_libraries(tests PRIVATE braam_core braam_flags)

find_program(BRAAM_NODE node REQUIRED)

add_test(NAME smoke
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --kernel $<TARGET_FILE:kernel>)

add_test(NAME unit
    COMMAND ${BRAAM_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/run.mjs --tests $<TARGET_FILE:tests>)

add_test(NAME size COMMAND ${BRAAM_SIZE_BUDGET_CHECK})
