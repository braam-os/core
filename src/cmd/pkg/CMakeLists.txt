# The package manager, as a library its binary links. main.cpp stays out of it.
add_library(braam_pkg STATIC
    clean.cpp
    db.cpp
    dep.cpp
    encode.cpp
    host.cpp
    index.cpp
    install.cpp
    pkg.cpp
    plan.cpp
    query.cpp
    script.cpp
    sha256.cpp
    solve.cpp
    stanza.cpp
    store.cpp
    trust.cpp
    unzip.cpp
    update.cpp
    verify.cpp
    version.cpp
    zip.cpp

    # `pkg search` globs, and the matcher is Str and Span and nothing else.
    # Compiled rather than linked: braam_sh would bring the whole shell in for
    # one pure file, which is what src/cmd/CMakeLists.txt does for /bin/test.
    ../sh/match.cpp)

target_link_libraries(braam_pkg PUBLIC braam_proc braam_flags)
