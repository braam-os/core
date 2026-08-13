add_library(braam_core STATIC
    alloc.cpp
    result.cpp
    sched.cpp)

target_link_libraries(braam_core PUBLIC braam_flags)
