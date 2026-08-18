# The shell, as a library a binary links (Concept.md §4). Every other program in
# src/cmd/ is one translation unit; this one is a grammar, an expander, a line
# editor, a job runtime and sixteen builtins, so it is a library and
# src/cmd/sh.cpp is its entry point.
#
# expand.cpp, match.cpp, parse.cpp and tokenize.cpp are pure, and test/ compiles
# them into tests.wasm directly rather than linking this.
add_library(braam_sh STATIC
    builtin/break.cpp
    builtin/cd.cpp
    builtin/dot.cpp
    builtin/eval.cpp
    builtin/exit.cpp
    builtin/export.cpp
    builtin/fg.cpp
    builtin/help.cpp
    builtin/jobs.cpp
    builtin/kill.cpp
    builtin/return.cpp
    builtin/set.cpp
    builtin/table.cpp
    builtin/unset.cpp
    edit.cpp
    expand.cpp
    glob.cpp
    job.cpp
    match.cpp
    parse.cpp
    shell.cpp
    tokenize.cpp
    var.cpp)

target_link_libraries(braam_sh PUBLIC braam_proc braam_flags)
