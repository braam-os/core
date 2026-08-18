# The shell, as a library a binary links (Concept.md §4). Every other program in
# src/cmd/ is one translation unit; this one is a grammar, an expander, a line
# editor, a job runtime and twenty-six builtins, so it is a library and
# src/cmd/sh.cpp is its entry point.
#
# cond.cpp, expand.cpp, match.cpp, parse.cpp and tokenize.cpp are pure, and
# test/ compiles them into tests.wasm directly rather than linking this.
# condrun.cpp is not pure but reaches nothing above proc/io.h, which is what
# lets src/cmd/test.cpp build /bin/test out of the same two files.
add_library(braam_sh STATIC
    builtin/break.cpp
    builtin/cd.cpp
    builtin/dot.cpp
    builtin/echo.cpp
    builtin/eval.cpp
    builtin/exec.cpp
    builtin/exit.cpp
    builtin/export.cpp
    builtin/fg.cpp
    builtin/help.cpp
    builtin/jobs.cpp
    builtin/kill.cpp
    builtin/read.cpp
    builtin/return.cpp
    builtin/set.cpp
    builtin/status.cpp
    builtin/table.cpp
    builtin/test.cpp
    builtin/trap.cpp
    builtin/unset.cpp
    builtin/wait.cpp
    cond.cpp
    condrun.cpp
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
