add_library(braam_core STATIC
    alloc.cpp
    key.cpp
    result.cpp
    sched.cpp
    screen.cpp
    text.cpp)

target_link_libraries(braam_core PUBLIC braam_flags)
