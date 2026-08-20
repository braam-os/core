// A PkgHost for the suite: the clock and the local files are fields, a fetch
// answers from a table, and verify is real Ed25519 through test/fakesvc.mjs.
//
// A body is handed out SYS_CHUNK at a time, so a read loop really loops.
#pragma once

#include "cmd/pkg/host.h"
#include "kernel/vec.h"

struct FakeHost : PkgHost {
    u64 clock       = 0;
    bool no_ed25519 = false; // a browser without the algorithm
    usize opened    = 0;
    usize closed    = 0;

    bool file(Str path, Str text);
    bool route(Str url, Str body, u32 status = 200);

    Task<Result<u64>> now() override;
    Task<Result<String>> load(Str path) override;
    Task<Result<i32>> open(Str url, u32 &status) override;
    Task<Result<String>> read(i32 body) override;
    Task<void> close(i32 body) override;
    Task<Result<bool>> verify(Str key, Str sig, Str bytes) override;

private:
    struct Entry {
        String key;
        String text;
        u32 status = 200;
    };

    struct Body {
        usize entry = 0;
        usize at    = 0;
        bool live   = false;
    };

    Vec<Entry> files_;
    Vec<Entry> routes_;
    Vec<Body> bodies_;
};
