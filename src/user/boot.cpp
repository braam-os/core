#include "boot.h"

#include "binfs.h"
#include "fs/bundlefs.h"
#include "fs/hostfs.h"
#include "fs/memfs.h"
#include "fs/opfsfs.h"
#include "fs/vfs.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "procfs.h"

namespace {

void say(Str s)
{
    screen_write(s);
    screen_newline();
}

// Mounts the archive the host loaded beside kernel.wasm, if there is one. A
// missing bundle is ordinary — the page may be served without it.
Task<void> mount_bundle()
{
    Task<Result<String>> t = storage_bundle();
    if (!t)
        co_return;
    Result<String> r = co_await t;
    if (r.is_err())
        co_return;

    Result<BundleFs *> b = bundlefs_create(move(r.value()));
    if (b.is_err()) {
        say("braam: the boot bundle is malformed");
        co_return;
    }
    if (vfs_mount("/usr", b.value()).is_err())
        say("braam: /usr would not mount");
}

// Concept.md §5.2: OPFS is absent in Safari private browsing, and the sync
// access handles the fast path needs exist only in a worker. Either missing
// means /home is memory, and the user is told so rather than left to discover
// it after a reload.
Task<void> mount_home()
{
    StorageBackend b;
    Task<Result<StorageBackend>> t = storage_info();
    if (t) {
        Result<StorageBackend> r = co_await t;
        if (r.is_ok())
            b = r.value();
    }

    if (b.opfs && b.sync) {
        Fs *home = heap_new<OpfsFs>();
        if (home && vfs_mount("/home", home).is_ok())
            co_return;
        say("braam: /home would not mount on OPFS");
    }

    Fs *home = heap_new<MemFs>();
    if (!home || vfs_mount("/home", home).is_err()) {
        say("braam: no /home at all");
        co_return;
    }
    say("braam: no OPFS — /home is in memory and will not survive a reload");
}

} // namespace

Task<void> boot_filesystem()
{
    // Idempotent. A second shell — or a second boot in a test — finds the
    // mounts already there and leaves them, rather than reporting every one of
    // them as an error.
    if (!vfs_mounts().empty())
        co_return;

    Fs *root = heap_new<MemFs>();
    if (!root || vfs_mount("/", root).is_err()) {
        say("braam: no root filesystem");
        co_return;
    }

    if (Task<Result<void>> t = vfs_mkdir("/tmp"))
        co_await t;

    // Where `import` puts what the file picker hands over (Concept.md §5.4).
    // A directory on the root MemFs rather than a mount of its own: the files
    // arrive as bytes, and nothing about them is a filesystem.
    if (Task<Result<void>> t = vfs_mkdir("/mnt"))
        co_await t;
    if (Task<Result<void>> t = vfs_mkdir("/mnt/import"))
        co_await t;

    if (Fs *bin = binfs_create())
        if (vfs_mount("/bin", bin).is_err())
            say("braam: /bin would not mount");

    // The scheduler, the heap and the job table, readable with cat and grep.
    if (Fs *proc = procfs_create())
        if (vfs_mount("/proc", proc).is_err())
            say("braam: /proc would not mount");

    if (Task<void> t = mount_bundle())
        co_await t;
    if (Task<void> t = mount_home())
        co_await t;

    // Landing in /home is what makes `echo hi > notes` persist by default.
    if (Task<Result<void>> t = vfs_chdir("/home"))
        co_await t;
}
