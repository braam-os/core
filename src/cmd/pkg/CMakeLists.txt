# The package manager, as a library its binary links. main.cpp stays out of it.
add_library(braam_pkg STATIC
    dep.cpp
    encode.cpp
    pkg.cpp
    sha256.cpp
    stanza.cpp
    version.cpp)

target_link_libraries(braam_pkg PUBLIC braam_proc braam_flags)
