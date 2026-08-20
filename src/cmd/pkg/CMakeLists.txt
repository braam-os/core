# The package manager, as a library its binary links. main.cpp stays out of it.
add_library(braam_pkg STATIC
    encode.cpp
    pkg.cpp
    sha256.cpp)

target_link_libraries(braam_pkg PUBLIC braam_proc braam_flags)
