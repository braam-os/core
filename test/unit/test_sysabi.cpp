#include "harness.h"
#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "user/exec.h"

namespace {

// A wasm module with nothing in it but the sections this asks for. Enough to
// exercise the walk: the parser only has to find a custom section by name.
struct Module {
    String bytes;

    Module() { bytes.append(Str("\0asm\1\0\0\0", 8)); }

    // id 0 is a custom section: a name, then the payload.
    void custom(Str name, Str body)
    {
        String s;
        s.push(char(name.size()));
        s.append(name);
        s.append(body);
        bytes.push('\0');
        bytes.push(char(s.size()));
        bytes.append(s.str());
    }

    // Any other id, with a body the parser must step over rather than read.
    void section(u8 id, usize size)
    {
        bytes.push(char(id));
        bytes.push(char(size));
        for (usize i = 0; i < size; i++)
            bytes.push('\xaa');
    }

    Str str() const { return bytes.str(); }
};

String meta_body(u32 magic, u32 abi, u32 max_pages)
{
    u8 raw[20];
    sys_put_u32(raw, magic);
    sys_put_u32(raw + 4, abi);
    sys_put_u32(raw + 8, 0);
    sys_put_u32(raw + 12, 4);
    sys_put_u32(raw + 16, max_pages);

    String s;
    s.append(Str(reinterpret_cast<const char *>(raw), sizeof(raw)));
    return s;
}

} // namespace

void test_sysabi()
{
    test_begin("sysabi");

    // The op word carries the operation's argument, so a write hands over its
    // bytes and nothing else, and an open hands over its path (Concept.md §4.3).
    CHECK_EQ(sys_op_fd(sys_op(Sys::Write, 7)), 7);
    CHECK(sys_op_code(sys_op(Sys::Write, 7)) == Sys::Write);
    CHECK_EQ(sys_op_fd(sys_op(Sys::Read)), 0);
    CHECK_EQ(sys_op_arg(sys_op(Sys::Open, SYS_O_READ | SYS_O_CREATE)), SYS_O_READ | SYS_O_CREATE);
    CHECK(sys_op_code(sys_op(Sys::Open, SYS_O_TRUNC)) == Sys::Open);

    // Wait and Kill carry a pid in that same field, which is 24 bits wide —
    // SYS_PID_MAX is the largest that survives the round trip, and Sys::Spawn
    // refuses to hand back one above it rather than let it truncate into a pid
    // belonging to somebody else.
    CHECK_EQ(sys_op_arg(sys_op(Sys::Wait, SYS_PID_MAX)), SYS_PID_MAX);
    CHECK(sys_op_code(sys_op(Sys::Wait, SYS_PID_MAX)) == Sys::Wait);
    CHECK_EQ(sys_op_arg(sys_op(Sys::Kill, SYS_PID_MAX + 1)), 0); // truncated, as advertised
    CHECK_EQ(sys_op_arg(sys_op(Sys::Wait, SYS_WAIT_ANY)), SYS_WAIT_ANY);

    // Chdir's one bit says whether it moves or only reports, and Cursor's says
    // the same about the cursor.
    CHECK_EQ(sys_op_arg(sys_op(Sys::Chdir, 1)) & 1, 1u);
    CHECK_EQ(sys_op_arg(sys_op(Sys::Chdir)) & 1, 0u);
    CHECK_EQ(sys_op_arg(sys_op(Sys::Cursor, 1)) & 1, 1u);
    CHECK(sys_op_code(sys_op(Sys::Cursor, 1)) == Sys::Cursor);

    // Fg carries a pid in the same 24-bit field Wait and Kill use, and zero is
    // "take the console back" rather than a pid.
    CHECK_EQ(sys_op_arg(sys_op(Sys::Fg, SYS_PID_MAX)), SYS_PID_MAX);
    CHECK(sys_op_code(sys_op(Sys::Fg, SYS_PID_MAX)) == Sys::Fg);

    // The numbers themselves, since a binary compiled today speaks them: the
    // terminal block runs to Cursor and the process family to Fg.
    CHECK_EQ(u32(Sys::Cursor), 69u);
    CHECK_EQ(u32(Sys::Fg), 84u);

    // A spawn request's flags word: the two page counts, in one word because
    // `aux` is the pid and nothing else may ride on that.
    ProcMeta pm{ PROC_MAGIC, PROC_ABI, 0, 4, PROC_MAX_PAGES };
    u32 flags = proc_pack(pm);
    CHECK_EQ(proc_initial(flags), 4);
    CHECK_EQ(proc_max(flags), PROC_MAX_PAGES);

    // argv crosses an address space as one blob.
    Str argv[] = { "tail", "-n", "2", "" };
    usize n    = argv_size(argv, 4);
    CHECK_EQ(n, 4 + 4 * 4 + 4 + 2 + 1);

    String blob;
    CHECK(blob.reserve(n));
    for (usize i = 0; i < n; i++)
        blob.push(0);
    argv_encode(argv, 4, reinterpret_cast<u8 *>(blob.data()));

    const u8 *p = reinterpret_cast<const u8 *>(blob.data());
    CHECK_EQ(argv_count(p, n), 4);
    CHECK(argv_at(p, n, 0) == "tail");
    CHECK(argv_at(p, n, 2) == "2");
    CHECK(argv_at(p, n, 3).empty());
    CHECK(argv_at(p, n, 4).empty()); // past the end, rather than past the buffer

    // A blob cut short reads as far as it can and no further.
    CHECK(argv_at(p, 6, 0).empty());
    CHECK_EQ(argv_count(p, 2), 0);

    // Sys::Spawn puts three descriptor words in front of that same blob, so the
    // kernel decodes argv with the encoder _start already uses rather than a
    // second one. The words say which of the caller's streams the child gets:
    // below SYS_FD_MIN is a share, anything else is a descriptor being moved.
    String req;
    u8 head[SYS_SPAWN_HEAD * 4];
    sys_put_u32(head, SYS_STDIN);
    sys_put_u32(head + 4, 5); // a pipe end of the caller's, moved in
    sys_put_u32(head + 8, SYS_STDERR);
    CHECK(req.append(Str(reinterpret_cast<const char *>(head), sizeof(head))));
    CHECK(req.append(blob.str()));

    const u8 *q = reinterpret_cast<const u8 *>(req.data());
    CHECK_EQ(sys_get_u32(q), SYS_STDIN);
    CHECK_EQ(sys_get_u32(q + 4), 5);
    CHECK_EQ(sys_get_u32(q + 8), SYS_STDERR);
    CHECK_EQ(argv_count(q + SYS_SPAWN_HEAD * 4, req.size() - SYS_SPAWN_HEAD * 4), 4);
    CHECK(argv_at(q + SYS_SPAWN_HEAD * 4, req.size() - SYS_SPAWN_HEAD * 4, 0) == "tail");

    // The metadata is what says how much memory a process gets, and it is found
    // after any number of sections the parser does not care about.
    Module m;
    m.section(1, 4);
    m.custom("name", "xx");
    m.custom(PROC_SECTION, meta_body(PROC_MAGIC, PROC_ABI, 256).str());
    m.section(10, 3);

    Result<ProcMeta> r = exec_meta(m.str());
    CHECK(r.is_ok());
    if (r.is_ok()) {
        CHECK_EQ(r.value().initial_pages, 4);
        CHECK_EQ(r.value().max_pages, 256);
    }

    // Everything that is not a braam binary is refused rather than guessed at,
    // and Invalid is "this was never a program".
    auto refused = [](Result<ProcMeta> r, Error e) { return r.is_err() && r.error() == e; };

    Module none;
    none.section(1, 4);
    CHECK(refused(exec_meta(none.str()), Error::Invalid));

    Module bad_magic;
    bad_magic.custom(PROC_SECTION, meta_body(0xdeadbeef, PROC_ABI, 256).str());
    CHECK(refused(exec_meta(bad_magic.str()), Error::Invalid));

    // A section of ours whose number is not ours is the one refusal that is not
    // Invalid: the file is a program, built against another kernel, and the
    // repair is to rebuild it (Concept.md §4.3).
    Module bad_abi;
    bad_abi.custom(PROC_SECTION, meta_body(PROC_MAGIC, PROC_ABI + 1, 256).str());
    CHECK(refused(exec_meta(bad_abi.str()), Error::Unsupported));

    Module short_meta;
    short_meta.custom(PROC_SECTION, "\1\2\3\4");
    CHECK(refused(exec_meta(short_meta.str()), Error::Invalid));

    CHECK(refused(exec_meta(""), Error::Invalid));
    CHECK(refused(exec_meta("\0asm"), Error::Invalid));
    CHECK(refused(exec_meta("not a module at all"), Error::Invalid));

    // A section whose length runs past the end of the file is a broken file,
    // not a walk that reads past it.
    Module over;
    over.bytes.push('\1');
    over.bytes.push('\x40');
    over.bytes.append("xx");
    CHECK(refused(exec_meta(over.str()), Error::Invalid));
}
