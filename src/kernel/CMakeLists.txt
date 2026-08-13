add_library(braam_core STATIC
    alloc.cpp
    key.cpp
    result.cpp
    sched.cpp
    screen.cpp)

target_link_libraries(braam_core PUBLIC braam_flags)
