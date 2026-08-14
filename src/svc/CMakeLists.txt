add_library(braam_svc STATIC
    net.cpp
    proc.cpp
    svc.cpp
    xfer.cpp)

target_link_libraries(braam_svc PUBLIC braam_core braam_flags)
