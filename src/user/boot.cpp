#include "boot.h"

#include "exec.h"
#include "fs/bundlefs.h"
#include "fs/hostfs.h"
#include "fs/memfs.h"
#include "fs/opfsfs.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "procfs.h"
#include "tty.h"

namespace {

void say(Str s)
{
    screen_write(s);
    screen_newline();
}

// Mounts the archive the host loaded beside kernel.wasm as /bin and /share —
// two views of the one bundle, since the programs and the files that ship
// beside them are one download and two directories.
//
// Without it there is no /bin at all, and therefore no shell to run. That
// was ordinary when programs were in-kernel; now it is worth saying out loud.
Task<void> mount_bundle()
{
    Task<Result<String>> t = storage_bundle();
    if (!t)
        co_return;
    Result<String> r = co_await t;
    if (r.is_err()) {
        say("braam: no boot bundle — /bin is empty and there is no shell to run");
        co_return;
    }

    Result<BundleFs *> b = bundlefs_create(move(r.value()));
    if (b.is_err()) {
        say("braam: the boot bundle is malformed");
        co_return;
    }

    Result<BundleFs *> bin   = bundlefs_at(*b.value(), "bin");
    Result<BundleFs *> share = bundlefs_at(*b.value(), "share");

    // The whole archive is not itself something to mount: the two views hold
    // the bytes between them, so the object that parsed them can go.
    heap_delete(b.value());

    // vfs_mount refuses a null and takes ownership of anything else, so a
    // failed view and a failed mount are the same line.
    if (vfs_mount("/bin", bin.is_ok() ? bin.value() : nullptr).is_err())
        say("braam: /bin would not mount");
    if (vfs_mount("/share", share.is_ok() ? share.value() : nullptr).is_err())
        say("braam: /share would not mount");
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

// The greeting, on the grid the prompt is about to appear in. A coroutine of
// its own rather than four lines of init_task, for the reason boot_filesystem
// is one: it holds the whole file, and init's frame is on the boot path.
//
// Silent when there is no motd. A boot archive without a greeting is not a
// broken one, and the line saying so would itself be noise at every boot.
Task<void> show_motd()
{
    Task<Result<String>> t = read_file(MOTD);
    if (!t)
        co_return;
    Result<String> r = co_await t;
    if (r.is_err() || r.value().empty())
        co_return;

    // Green, and back to the default afterwards: the style is sticky grid
    // state, so what the prompt writes next would inherit it. The defaults are
    // named here rather than saved, because nothing has changed them — this is
    // the kernel's only use of colour, and boot is the only writer at boot.
    //
    // screen_write turns the file's own newlines into rows (screen.cpp), so
    // the only thing left to arrange is that the prompt starts on a line of
    // its own — which a file not ending in a newline would take away.
    screen_style(COLOR_GREEN, COLOR_BLACK, 0);
    screen_write(r.value().str());
    if (r.value().str()[r.value().size() - 1] != '\n')
        screen_newline();
    screen_style(COLOR_WHITE, COLOR_BLACK, 0);
}

// Why /bin/sh would not resolve, on one line. Three different repairs hide
// behind one Result: an archive that never had a shell, one built against
// another kernel, and one whose bytes are damaged.
void no_shell(Error e)
{
    Buf<160> line;
    line.put("braam: ").put(SHELL).put(": ");
    switch (e) {
    case Error::NotFound:
        line.put("not in the boot archive");
        break;
    case Error::Unsupported:
        line.put("built for another process ABI — this kernel speaks ")
            .put(u32(PROC_ABI))
            .put(", so the boot archive is stale");
        break;
    case Error::Invalid:
        line.put("not a program — no braam section, or a damaged image");
        break;
    case Error::NoMemory:
        line.put("out of memory reading the image");
        break;
    default:
        line.put(error_name(e));
        break;
    }
    say(line.str());
    say("braam: there is no prompt — reload once the boot archive is repaired");
}

} // namespace

Task<void> boot_filesystem()
{
    // Idempotent. A second boot in a test finds the mounts already there and
    // leaves them, rather than reporting every one of them as an error.
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

    // The scheduler and the heap, readable with cat and grep. The job table is
    // not here any more: it is the shell's own memory, and the shell is a
    // process like any other (Concept.md §5.1).
    if (Fs *proc = procfs_create())
        if (vfs_mount("/proc", proc).is_err())
            say("braam: /proc would not mount");

    if (Task<void> t = mount_bundle())
        co_await t;
    if (Task<void> t = mount_home())
        co_await t;

    // Landing in /home is what makes `echo hi > notes` persist by default. It
    // is the kernel's own directory now, and what /bin/sh inherits at exec.
    if (Task<Result<void>> t = vfs_chdir("/home"))
        co_await t;
}

Task<i32> init_task()
{
    // The mounts come first, and they always did — but the shell used to do
    // them. It cannot any more: it is a file in /bin, and /bin is one of them.
    if (Task<void> t = boot_filesystem())
        co_await t;

    Executable exe;
    Result<void> found = Err(Error::NoMemory);
    if (Task<Result<void>> t = exec_resolve(SHELL, exe))
        found = co_await t;
    if (found.is_err()) {
        // Said on the grid rather than through a Stream, because there is
        // nobody left to print it: a bundle that will not give up /bin/sh is a
        // system with no prompt, and the reason has to reach the screen.
        no_shell(found.error());
        co_return 1;
    }

    // Only now, with a shell that is going to run: a system with no prompt
    // coming should show why, not a greeting above a dead terminal.
    if (Task<void> t = show_motd())
        co_await t;

    Args args{ Span<const Str>(&SHELL, 1) };
    Task<i32> t = exec_process(exe, args, stdio_console());
    if (!t) {
        say("braam: /bin/sh would not start");
        co_return 1;
    }
    i32 status = co_await t;

    // init spawns the shell and nothing else, so there is no getty to start
    // another. Say so rather than leave a prompt that never comes back, and
    // carry the status: a shell that died on its first step and one the user
    // typed `exit` at look the same from here otherwise.
    Buf<96> line;
    line.put("braam: the shell exited (status ").put(status).put(") — reload to start again");
    say(line.str());
    co_return status;
}
