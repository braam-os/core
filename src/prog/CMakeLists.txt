# An OBJECT library, not STATIC: nothing references these translation units by
# name — they reach the link only through their static-init registrars, and
# --gc-sections never extracts an unreferenced archive member.
add_library(braam_prog OBJECT
    cat.cpp
    cd.cpp
    chat.cpp
    clear.cpp
    curl.cpp
    date.cpp
    df.cpp
    echo.cpp
    export.cpp
    false.cpp
    grep.cpp
    head.cpp
    help.cpp
    import.cpp
    ls.cpp
    mkdir.cpp
    mount.cpp
    pbcopy.cpp
    pbpaste.cpp
    pwd.cpp
    rm.cpp
    sleep.cpp
    tail.cpp
    touch.cpp
    true.cpp
    version.cpp
    wc.cpp)

target_link_libraries(braam_prog PUBLIC braam_user braam_flags)
