# The layout layer, and the one library both sides of the process ABI link.
# It links braam_flags alone: the consumer supplies the allocator, and nothing
# here reaches a host import — Grid is a rectangle of cells and kernel/screen.h
# is a header with no code behind it. The exact-import assertion in
# test/run.mjs, over every binary, is what keeps that true.
add_library(braam_ui STATIC
    grid.cpp
    pane.cpp
    textbuf.cpp
    view.cpp)

target_link_libraries(braam_ui PUBLIC braam_flags)
