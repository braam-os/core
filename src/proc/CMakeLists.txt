# The runtime every process binary carries a copy of (Concept.md §4.4). It
# links four of the kernel's own translation units rather than braam_core or
# braam_fs: alloc.cpp is the same allocator, and result.cpp, text.cpp and
# path.cpp are pure. Anything else in either would drag a host import in, and
# there is no host here — which is what the exact-import assertion in
# test/run.mjs checks, for every binary.
add_library(braam_proc STATIC
    io.cpp
    opt.cpp
    rt.cpp
    screen.cpp
    time.cpp
    ../fs/path.cpp
    ../kernel/alloc.cpp
    ../kernel/result.cpp
    ../kernel/text.cpp)

# braam_ui is the layout layer, and it links no host import either — a
# process paints a Grid of its own and blits the damage across.
target_link_libraries(braam_proc PUBLIC braam_ui braam_flags)
