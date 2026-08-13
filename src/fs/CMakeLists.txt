add_library(braam_fs STATIC
    bundlefs.cpp
    hostfs.cpp
    memfs.cpp
    opfsfs.cpp
    path.cpp
    vfs.cpp)

target_link_libraries(braam_fs PUBLIC braam_core braam_flags)
