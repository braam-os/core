# The shell, as a library a binary links (Concept.md §4). Every other program in
# src/cmd/ is one translation unit; this one is a grammar, a line editor, a job
# runtime and six builtins, so it is a library and src/cmd/sh.cpp is its entry
# point.
#
# parse.cpp and tokenize.cpp are also compiled into braam_user while the
# resident shell still exists: it is the same code either side of the process
# boundary, since it touches nothing but Str, String and Vec.
add_library(braam_sh STATIC
    builtin/cd.cpp
    builtin/exit.cpp
    builtin/fg.cpp
    builtin/help.cpp
    builtin/jobs.cpp
    builtin/kill.cpp
    builtin/table.cpp
    edit.cpp
    job.cpp
    parse.cpp
    shell.cpp
    tokenize.cpp)

target_link_libraries(braam_sh PUBLIC braam_proc braam_flags)
