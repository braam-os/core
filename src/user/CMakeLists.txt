add_library(braam_user STATIC
    boot.cpp
    builtin/cd.cpp
    builtin/exit.cpp
    builtin/fg.cpp
    builtin/help.cpp
    builtin/jobs.cpp
    builtin/kill.cpp
    builtin/table.cpp
    edit.cpp
    exec.cpp
    io.cpp
    job.cpp
    parse.cpp
    procfs.cpp
    shell.cpp
    tokenize.cpp
    tty.cpp)

target_link_libraries(braam_user PUBLIC braam_svc braam_fs braam_core braam_flags)
