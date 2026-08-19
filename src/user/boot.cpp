#include "boot.h"

#include "console.h"
#include "exec.h"
#include "fs/hostfs.h"
#include "fs/opfsfs.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "kernel/version.h"
#include "procfs.h"
#include "svc/svc.h"
#include "tty.h"

namespace {

// Deaths in quick succession before init stops replacing the shell, and how
// long one has to last to count as having got going.
constexpr u32 RESPAWN_TRIES    = 3;
constexpr f64 RESPAWN_FLOOR_MS = 1000;

// The host's description of itself, asked for once. A pointer rather than a
// String, because a namespace-scope global must be trivially destructible.
String *g_host = nullptr;

// On a row of its own: a shell that died left its prompt on this one.
void say(Str s)
{
    if (screen().cursor_x != 0)
        screen_newline();
    screen_write(s);
    screen_newline();
}

// Yes or no on the grid, before there is a shell to ask with. The keyboard's
// receiver is the console pump, which init spawns before this task (main.cpp),
// so a claim through it is how anything reads a key — and nothing else holds
// one at boot. Raw keys are not echoed, so the answer is written here.
//
// Anything but y is no, ^C included: it arrives as an ordinary key, since
// nothing has armed a foreground.
Task<bool> ask(Str question, const u32 &pid)
{
    Buf<96> line;
    line.put(question).put(" [y/N] ");
    if (screen().cursor_x != 0)
        screen_newline();
    // Bright white for the question, back to the default for the answer.
    screen_style(COLOR_WHITE | COLOR_BRIGHT, COLOR_BLACK, 0);
    screen_write(line.str());
    screen_style(COLOR_WHITE, COLOR_BLACK, 0);

    KeyInput keys(pid);
    if (!keys.ok()) {
        screen_write("n\n");
        co_return false;
    }

    for (;;) {
        Result<Key> k = co_await keys.next();
        if (k.is_err()) {
            if (k.error() == Error::Again)
                continue;
            screen_write("n\n");
            co_return false;
        }
        bool yes = k.value().code == 'y' || k.value().code == 'Y';
        if (!yes && k.value().code != 'n' && k.value().code != 'N' && k.value().code != KEY_ENTER &&
            !(k.value().mods & MOD_CTRL))
            continue;
        screen_write(yes ? "y\n" : "n\n");
        co_return yes;
    }
}

// The stored image against this kernel's. Absent means an empty store, which
// is unpacked without asking — there is nothing there to overwrite and no
// prompt anyone could answer usefully. A mismatch is the user's call: the old
// /bin may be what they want kept, and a stale one says so for itself when
// exec_resolve refuses it.
Task<void> unpack_if_stale(const u32 &pid)
{
    String stored;
    if (Task<Result<String>> t = read_file(VERSION_PATH)) {
        Result<String> r = co_await t;
        if (r.is_ok())
            stored = move(r.value());
    }

    if (stored.str() == BRAAM_VERSION)
        co_return;

    if (!stored.empty()) {
        Buf<128> line;
        line.put("the stored image is ")
            .put(stored.str())
            .put(" and this kernel is ")
            .put(BRAAM_VERSION);
        say(line.str());
        bool yes = false;
        if (Task<bool> t = ask("replace /bin and /share?", pid))
            yes = co_await t;
        if (!yes) {
            say("keeping the stored image");
            co_return;
        }
    }

    Result<u32> r = Err(Error::NoMemory);
    if (Task<Result<u32>> t = storage_unpack(BRAAM_VERSION))
        r = co_await t;
    if (r.is_err()) {
        say("the root archive would not unpack — /bin is empty and there is no shell");
        co_return;
    }

    Buf<96> line;
    line.put("unpacked ").put(r.value()).put(" files");
    say(line.str());
}

// The directories the store is expected to have and the archive does not
// carry. Exists is the ordinary answer on every boot but the first.
Task<void> make_dirs()
{
    // Where `fimport` puts what the file picker hands over (Concept.md §5.4):
    // /import, at the root.
    // A directory like any other: the files arrive as bytes, and nothing about
    // them is a filesystem.
    constexpr Str DIRS[] = { "/home", "/import" };
    for (Str d : DIRS) {
        if (Task<Result<void>> t = vfs_mkdir(d)) {
            Result<void> r = co_await t;
            if (r.is_err() && r.error() != Error::Exists) {
                Buf<96> line;
                line.put(d).put(": ").put(error_name(r.error()));
                say(line.str());
            }
        }
    }
}

// /tmp is temporary against the store's persistence, not the tab's: OPFS keeps
// what is written to it, so what makes a temporary directory temporary now is
// emptying it here.
Task<void> wipe_tmp()
{
    if (Task<Result<void>> t = vfs_remove("/tmp", true))
        co_await t;
    if (Task<Result<void>> t = vfs_mkdir("/tmp"))
        co_await t;
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

// The half of the description above the blank line: the fields short enough to
// put on the boot grid. The rest — the locale and the agent string, which wraps
// a row on its own — waits in /proc/host for anyone who asks.
Str banner_half(Str s)
{
    for (usize i = 0; i + 1 < s.size(); i++)
        if (s[i] == '\n' && s[i + 1] == '\n')
            return s.substr(0, i + 1);
    return s;
}

// Where the value starts on a banner row, which is where the table the host
// and procfs.cpp write puts it too.
constexpr usize LABEL_COL = 9;

// One `name value` row of the description, written as `name: value`. The colon
// is presentation and lives only here: /proc/host stays a plain table, which is
// what lets `uname` read a field out of it with next_field.
void write_labelled(Str line)
{
    usize name = 0;
    while (name < line.size() && line[name] != ' ')
        name++;
    usize value = name;
    while (value < line.size() && line[value] == ' ')
        value++;

    Buf<160> out;
    out.put(line.substr(0, name)).put(':');

    // At least one space, so a name that fills the column still separates.
    usize col = name + 1;
    do {
        out.put(' ');
        col++;
    } while (col < LABEL_COL);
    out.put(line.substr(value));

    screen_write(out.str());
    screen_newline();
}

// What the system is running on, under the version line main.cpp already wrote.
// It is here rather than there because that export cannot await, and asking the
// host is the only way to know any of this.
//
// A description that does not arrive is not an error: the store is what boot
// cannot do without, and this is the motd's kind of line, not the OPFS one.
Task<void> show_host(const StorageBackend &b)
{
    if (Task<Result<String>> t = host_info()) {
        Result<String> r = co_await t;
        if (r.is_ok() && !r.value().empty()) {
            if (String *held = heap_new<String>(move(r.value()))) {
                heap_delete(g_host);
                g_host = held;
            }
        }
    }

    Str rest = banner_half(host_facts());
    while (!rest.empty()) {
        usize nl = rest.find('\n');
        Str line = nl == Str::npos ? rest : rest.substr(0, nl);
        rest     = nl == Str::npos ? Str() : rest.substr(nl + 1);
        if (!line.empty())
            write_labelled(line);
    }

    // The geometry is not here: it is on the screen being reported, and
    // /proc/host answers it live for anyone who wants the number.
    //
    // How much room there is, and not how much is left: the usage is the half
    // that moves, and a figure that was true at boot is the wrong one to leave
    // on the screen all session. `df` is where to ask.
    //
    // The durability is a property of the store, not of a mount, so it is here
    // rather than in df's table — as boot read it, a late persist() answer
    // correcting the store and not this line (§5.3).
    Buf<96> line;
    line.put("store:   OPFS, ");
    if (b.quota)
        line.put(u64(b.quota / 1000000)).put(" MB");
    else
        line.put("quota unknown");
    line.put(b.persisted ? Str(", persistent") : Str(", best-effort"));
    say(line.str());
}

// Why /bin/sh would not resolve, on one line. Three different repairs hide
// behind one Result: a store with no shell in it, one built against another
// kernel, and one whose bytes are damaged.
void no_shell(Error e)
{
    Buf<160> line;
    line.put(SHELL).put(": ");
    switch (e) {
    case Error::NotFound:
        line.put("not in the store");
        break;
    case Error::Unsupported:
        line.put("built for another process ABI — this kernel speaks ")
            .put(u32(PROC_ABI))
            .put(", so the stored image is stale");
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
}

} // namespace

Str host_facts()
{
    return g_host ? g_host->str() : Str();
}

Task<bool> boot_filesystem(const u32 &pid)
{
    // Idempotent. A second boot in a test finds the mounts already there and
    // leaves them, rather than reporting every one of them as an error.
    if (!vfs_mounts().empty())
        co_return true;

    // Concept.md §5.2: OPFS is absent in Safari private browsing, and the sync
    // access handles the fast path needs exist only in a worker. There is no
    // second store to fall back to any more — everything is in this one.
    StorageBackend b;
    if (Task<Result<StorageBackend>> t = storage_info()) {
        Result<StorageBackend> r = co_await t;
        if (r.is_ok())
            b = r.value();
    }
    if (!b.opfs || !b.sync) {
        say("this browser has no OPFS — Braam cannot run here");
        say("private browsing usually explains it; try an ordinary window");
        co_return false;
    }

    // What the machine is, before what is being done to it: the mounts and the
    // unpack below say what happened, and this says where.
    if (Task<void> t = show_host(b))
        co_await t;

    Fs *root = heap_new<OpfsFs>();
    if (!root || vfs_mount("/", root).is_err()) {
        say("no root filesystem");
        co_return false;
    }

    // The scheduler and the heap, readable with cat and grep. The job table is
    // not here any more: it is the shell's own memory, and the shell is a
    // process like any other (Concept.md §5.1).
    if (Fs *proc = procfs_create())
        if (vfs_mount("/proc", proc).is_err())
            say("/proc would not mount");

    if (Task<void> t = unpack_if_stale(pid))
        co_await t;
    if (Task<void> t = make_dirs())
        co_await t;
    if (Task<void> t = wipe_tmp())
        co_await t;

    // Landing in /home is what makes `echo hi > notes` persist by default. It
    // is the kernel's own directory now, and what /bin/sh inherits at exec.
    if (Task<Result<void>> t = vfs_chdir("/home"))
        co_await t;
    co_return true;
}

// What init hands the shell. No PATH — a command word resolves as function,
// then builtin, then /bin — and no TERM, since the terminal is a cell grid.
bool base_env(String &out)
{
    constexpr Str WORDS[] = { "HOME=/home", "SHELL=/bin/sh" };
    constexpr usize N     = sizeof(WORDS) / sizeof(WORDS[0]);

    usize n = argv_size(WORDS, N);
    if (!out.reserve(n))
        return false;
    for (usize i = 0; i < n; i++)
        out.push(0);
    argv_encode(WORDS, N, reinterpret_cast<u8 *>(out.data()));
    return true;
}

Task<i32> init_task(const u32 &pid)
{
    // The store comes first, and the mounts always did — but the shell used to
    // do them. It cannot any more: it is a file in /bin, which is in the store.
    bool up = false;
    if (Task<bool> t = boot_filesystem(pid))
        up = co_await t;
    if (!up)
        co_return 1;

    bool greeted  = false;
    bool restored = false;
    u32 tries     = 0;
    i32 status    = 1;

    // Encoded once: every shell this loop starts is entered with the same one.
    String env;
    if (!base_env(env))
        co_return 1;

    for (;;) {
        // Resolved every time round: the image was moved into the instance, so
        // the Executable cannot be reused.
        Executable exe;
        Result<void> found = Err(Error::NoMemory);
        if (Task<Result<void>> t = exec_resolve(SHELL, exe))
            found = co_await t;
        if (found.is_err()) {
            // Said on the grid rather than through a Stream, because there is
            // nobody left to print it: a store that will not give up /bin/sh
            // is a system with no prompt, and the reason has to reach the
            // screen.
            no_shell(found.error());

            // /bin is writable now, so `rm /bin/sh` is reachable from the
            // prompt — and the stamp would still match, so nothing would ever
            // put it back. This offer is what keeps the archive the thing the
            // store can be recovered from. Once: a second failure after a
            // fresh unpack is not something another unpack fixes.
            if (!restored) {
                restored = true;
                bool yes = false;
                if (Task<bool> t = ask("restore /bin and /share from the archive?", pid))
                    yes = co_await t;
                if (yes) {
                    Result<u32> r = Err(Error::NoMemory);
                    if (Task<Result<u32>> t = storage_unpack(BRAAM_VERSION))
                        r = co_await t;
                    if (r.is_ok())
                        continue;
                    say("the root archive would not unpack");
                }
            }
            say("there is no prompt — reload once the store is repaired");
            co_return 1;
        }
        // Init's own, and every replacement takes it again: the record under it
        // is removed and the host is told before this loop comes round, since
        // ~End calls proc_remove and proc_kill, and proc_kill is a host_svc
        // with nothing to await.
        exe.pid = pid;

        // Only now, with a shell that is going to run: a system with no prompt
        // coming should show why, not a greeting above a dead terminal. Once,
        // however many shells follow.
        if (!greeted) {
            greeted = true;
            if (Task<void> t = show_motd())
                co_await t;
        }

        f64 started = sched_now();
        bool died   = true;
        Args args{ Span<const Str>(&SHELL, 1) };

        // Scoped, so this shell's record — removed by a destructor in there —
        // is gone before the next one, which answers to the same pid.
        {
            Task<i32> t = exec_process(exe, args, stdio_console(), Str(), env.str(), &died);
            if (!t) {
                say("/bin/sh would not start");
                co_return 1;
            }
            status = co_await t;
        }

        // A shell that exited is the end of the session; so is a cancelled one,
        // which is the kernel going away rather than a shell to replace.
        if (!died || status == 130)
            break;

        // A shell that died is replaced (Concept.md §4) — but not for ever, and
        // a shell that lived a while is not a crash loop, so its death starts
        // the count again. A host with no worker to give is not a death: the
        // spawn waits for one rather than returning.
        tries = sched_now() - started >= RESPAWN_FLOOR_MS ? 1 : tries + 1;
        if (tries >= RESPAWN_TRIES) {
            say("the shell will not stay up — reload to start again");
            co_return status;
        }

        // The foreground the dead shell armed, or ^C at the next prompt reaches
        // pids that are gone (console.h). The keyboard and screen claims need
        // nothing: ~Proc drops both.
        console_fg_clear();

        Buf<96> line;
        line.put("the shell died (status ").put(status).put(") — starting another");
        say(line.str());
    }

    // The user's own `exit`, and there is no getty to start another. Say so
    // rather than leave a prompt that never comes back, and carry the status.
    Buf<96> line;
    line.put("the shell exited (status ").put(status).put(") — reload to start again");
    say(line.str());
    co_return status;
}
