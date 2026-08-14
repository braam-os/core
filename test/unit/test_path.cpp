#include "fs/path.h"
#include "harness.h"

namespace {

Str resolved(Str cwd, Str p, String &out)
{
    CHECK(path_resolve(cwd, p, out).is_ok());
    return out.str();
}

} // namespace

void test_path()
{
    test_begin("path");

    String out;

    // Absolute paths ignore the cwd; relative ones start from it. The first
    // half is what lets a process resolve against its own working directory
    // (Concept.md §5.1) and hand the VFS an absolute path: the VFS resolves it
    // a second time against the *shell's* cwd, and that pass has to be a no-op.
    CHECK(resolved("/home", "/etc/hosts", out) == "/etc/hosts");
    {
        String twice;
        CHECK(resolved("/anywhere/else", resolved("/home", "notes", out), twice) == "/home/notes");
    }
    CHECK(resolved("/home", "notes", out) == "/home/notes");
    CHECK(resolved("/", "notes", out) == "/notes");
    CHECK(resolved("/home", "", out) == "/home");

    // '.' drops, '..' pops, and repeated or trailing slashes go.
    CHECK(resolved("/home", ".", out) == "/home");
    CHECK(resolved("/home", "..", out) == "/");
    CHECK(resolved("/home/work", "../notes", out) == "/home/notes");
    CHECK(resolved("/", "a//b/", out) == "/a/b");
    CHECK(resolved("/", "./a/./b/..", out) == "/a");

    // '..' at the root stays at the root rather than escaping it.
    CHECK(resolved("/", "../../etc", out) == "/etc");
    CHECK(resolved("/a", "../../..", out) == "/");

    CHECK(path_dirname("/a/b/c") == "/a/b");
    CHECK(path_dirname("/a") == "/");
    CHECK(path_dirname("/") == "/");

    CHECK(path_basename("/a/b/c") == "c");
    CHECK(path_basename("/a") == "a");
    CHECK(path_basename("/") == "/");

    CHECK(path_join("/a", "b", out).is_ok());
    CHECK(out == "/a/b");
    CHECK(path_join("/", "b", out).is_ok());
    CHECK(out == "/b");

    // The prefix test the mount table uses: a component boundary, not a
    // substring, so /home is not a prefix of /homer.
    CHECK(path_under("/", "/anything"));
    CHECK(path_under("/home", "/home"));
    CHECK(path_under("/home", "/home/notes"));
    CHECK(!path_under("/home", "/homer"));
    CHECK(!path_under("/home", "/"));
}
