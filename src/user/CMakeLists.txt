# What is left above the kernel once the shell is a program: the syscall
# dispatcher every process talks to, the console the terminal is behind, the
# pipes a stage's stdio stands on, /proc, and the boot that runs /bin/sh.
add_library(braam_user STATIC
    boot.cpp
    console.cpp
    exec.cpp
    io.cpp
    procfs.cpp
    proctab.cpp
    syscall.cpp
    tty.cpp)

target_link_libraries(braam_user PUBLIC braam_svc braam_fs braam_core braam_flags)
