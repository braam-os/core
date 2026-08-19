// The mount table and the open-file table (Concept.md §5.1, §5.2). Everything
// above this line works in whole paths; everything below it works in one
// filesystem's own namespace.
//
// The tables live behind a pointer built on first use, like the scheduler: a
// namespace-scope global with a non-trivial destructor would need __cxa_atexit,
// which nothing provides.
#pragma once

#include "fs.h"
#include "kernel/span.h"

struct Mount {
    String prefix; // absolute, normalised; "/" for the root
    Fs *fs = nullptr;
};

// The table takes ownership of `fs`; vfs_reset() destroys every mount.
Result<void> vfs_mount(Str prefix, Fs *fs);

Span<const Mount> vfs_mounts();

// The mount a path lands in, and its path within that filesystem. `sub` views
// `abs`, so it lives exactly as long.
const Mount *vfs_lookup(Str abs, Str &sub);

Str vfs_cwd();

Task<Result<void>> vfs_chdir(Str path);

// Resolves against the cwd and normalises (path.h). Lexical: '..' pops a
// component textually and never sees a symbolic link, which is `cd -L` and is
// what keeps this synchronous.
Result<void> vfs_abs(Str path, String &out);

// The physical path of an already-absolute one, with its symbolic links
// followed. `follow_final` follows a link named by the last component too; the
// operations acting on the link itself pass false. `out` is filled whatever
// happens, so a caller creating the leaf may use it after Err(NotFound). The
// Stat returned is the leaf's.
Task<Result<Stat>> vfs_resolve(Str abs, bool follow_final, String &out);

// `follow` false reports a final symbolic link itself, which is lstat.
Task<Result<Stat>> vfs_stat(Str path, bool follow = true);

// Directory entries, sorted by name, with any mount point directly beneath the
// listed directory folded in — that is what makes /home visible in `ls /`.
Task<Result<Vec<Entry>>> vfs_list(Str path);

// A file descriptor, or an error. The offset is the caller's business; nothing
// here is seekable, because nothing here is stateful — which is what lets
// several descriptors share one backend handle. Readers do; a writer is refused
// while anyone holds the file, and refused the file anyone else holds (§5.2).
Task<Result<i32>> vfs_open(Str path, u32 flags);

Task<Result<void>> vfs_mkdir(Str path);

Task<Result<void>> vfs_remove(Str path, bool all);

// Moves an existing file's mtime to now.
Task<Result<void>> vfs_touch(Str path);

// Creates `path` as a symbolic link to `target`. The target is stored as given,
// so it may dangle; a relative one reads against the link's own directory.
Task<Result<void>> vfs_symlink(Str target, Str path);

// The target of a symbolic link, unresolved.
Task<Result<String>> vfs_readlink(Str path);

// On an open descriptor, and therefore synchronous (Concept.md §5.2).
Result<usize> vfs_read(i32 fd, u64 off, u8 *buf, usize n);
Result<usize> vfs_write(i32 fd, u64 off, const u8 *buf, usize n);
Result<u64> vfs_size(i32 fd);
Result<void> vfs_truncate(i32 fd, u64 n);
void vfs_close(i32 fd);

// Drops every mount and every open file. For tests, and for a reboot.
void vfs_reset();
