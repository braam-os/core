add_library(braam_ui STATIC
    full.cpp
    pane.cpp
    textbuf.cpp
    view.cpp)

target_link_libraries(braam_ui PUBLIC braam_core braam_flags)
