# An OBJECT library, not STATIC: nothing references these translation units by
# name — they reach the link only through their static-init registrars, and
# --gc-sections never extracts an unreferenced archive member.
add_library(braam_prog OBJECT
    clear.cpp
    echo.cpp
    false.cpp
    help.cpp
    sleep.cpp
    true.cpp
    version.cpp)

target_link_libraries(braam_prog PUBLIC braam_user braam_flags)
