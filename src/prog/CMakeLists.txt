# An OBJECT library, not STATIC: nothing references these translation units by
# name — they reach the link only through their static-init registrars, and
# --gc-sections never extracts an unreferenced archive member.
add_library(braam_prog OBJECT
    cat.cpp
    clear.cpp
    echo.cpp
    false.cpp
    grep.cpp
    head.cpp
    help.cpp
    ls.cpp
    sleep.cpp
    tail.cpp
    true.cpp
    version.cpp
    wc.cpp)

target_link_libraries(braam_prog PUBLIC braam_user braam_flags)
