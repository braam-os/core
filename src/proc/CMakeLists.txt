# The runtime every process binary carries a copy of (Concept.md §4.4). It
# links three of the kernel's own translation units rather than braam_core:
# alloc.cpp is the same allocator, result.cpp and text.cpp are pure. Anything
# else in braam_core would drag a host import in, and there is no host here.
add_library(braam_proc STATIC
    io.cpp
    rt.cpp
    ../kernel/alloc.cpp
    ../kernel/result.cpp
    ../kernel/text.cpp)

target_link_libraries(braam_proc PUBLIC braam_flags)
