add_library(braam_user STATIC
    binfs.cpp
    boot.cpp
    edit.cpp
    io.cpp
    job.cpp
    parse.cpp
    prog.cpp
    shell.cpp
    tokenize.cpp
    tty.cpp)

target_link_libraries(braam_user PUBLIC braam_fs braam_core braam_flags)
