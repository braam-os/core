add_library(braam_fs STATIC
    hostfs.cpp
    opfsfs.cpp
    path.cpp
    vfs.cpp)

target_link_libraries(braam_fs PUBLIC braam_core braam_flags)
