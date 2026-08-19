// The filesystem interface (Concept.md §3.6, §5). An implementation sees paths
// already resolved and made relative to its own mount point, so "/" is its root
// and it never has to know where it was mounted.
//
// Opening is asynchronous and everything on an open file is not. That is
// Concept.md §5.2's shape rather than a convenience: an OPFS sync access handle
// really does read and write synchronously once it exists, so a redirection can
// open at job setup and every write afterwards is a plain call.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "kernel/vec.h"

enum class NodeKind : u8 {
    File = 0,
    Dir  = 1,
};

// `mtime` is milliseconds since the epoch, 0 when the filesystem keeps none.
struct Stat {
    NodeKind kind = NodeKind::File;
    u64 size      = 0;
    u64 mtime     = 0;
};

struct Entry {
    String name;
    NodeKind kind = NodeKind::File;
    u64 size      = 0;
    u64 mtime     = 0;
};

// open() flags. Read and write are separate bits because the open-file table
// shares a handle between readers and refuses to share with a writer
// (Concept.md §5.2).
enum : u32 {
    O_READ   = 1,
    O_WRITE  = 2,
    O_CREATE = 4,
    O_TRUNC  = 8,
    O_APPEND = 16,
};

// Files are read and written a block at a time. 512 is the allocator's top size
// class: a byte more costs a whole 64 KiB span (Concept.md §8.2).
constexpr usize FS_BLOCK = 512;

struct Fs {
    virtual ~Fs() = default;

    // What `mount` prints, and what `df` names as the backing store.
    virtual Str kind() const = 0;

    virtual bool writable() const = 0;

    // What this filesystem holds, for `df`. Zero when it is not the kind of
    // store that knows — an OPFS mount asks the host instead.
    virtual u64 bytes() const { return 0; }

    virtual Task<Result<Stat>> stat(Str path)             = 0;
    virtual Task<Result<Vec<Entry>>> list(Str path)       = 0;
    virtual Task<Result<u32>> open(Str path, u32 flags)   = 0;
    virtual Task<Result<void>> mkdir(Str path)            = 0;
    virtual Task<Result<void>> remove(Str path, bool all) = 0;

    // Moves an existing file's mtime to now. A filesystem keeping none refuses.
    virtual Task<Result<void>> touch(Str path)
    {
        (void)path;
        co_return Err(Error::Unsupported);
    }

    // On an open handle, and therefore synchronous.
    virtual Result<usize> read(u32 h, u64 off, u8 *buf, usize n)        = 0;
    virtual Result<usize> write(u32 h, u64 off, const u8 *buf, usize n) = 0;
    virtual Result<u64> size(u32 h)                                     = 0;
    virtual Result<void> truncate(u32 h, u64 n)                         = 0;
    virtual void close(u32 h)                                           = 0;
};

// What `df` reports, and what decides where /home is mounted (Concept.md §5.3).
// `persisted` comes from the main thread: navigator.storage.persist() is not
// available in a worker (Concept.md §A.2).
struct StorageBackend {
    u64 quota      = 0;
    u64 usage      = 0;
    bool opfs      = false;
    bool sync      = false; // createSyncAccessHandle(), the fast path
    bool fsaccess  = false; // showDirectoryPicker(), still unused
    bool persisted = false;
};
