add_library(braam_user STATIC
    edit.cpp
    prog.cpp
    shell.cpp
    tokenize.cpp
    tty.cpp)

target_link_libraries(braam_user PUBLIC braam_core braam_flags)
