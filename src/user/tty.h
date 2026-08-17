// Stdio backed directly by the cell grid, and the keyboard's routing table.
//
// The screen is not behind a byte channel: §2.3 says the terminal *is* a cell
// grid, so a queue in front of it would add a copy and a scheduling hop to
// reach an array screen_write already fills, and would put terminal output
// behind something that can drop.
//
// The keyboard is the other way round: it is one Channel with one receiver
// (channel.h), and that receiver is the console pump, for good (console.h). A
// program therefore does not take the keyboard — it asks the pump to route to
// it, which is what the claims below are, and the prompt is no exception.
//
// Each of the two routes — raw keys and the screen — has one holder at a time,
// kernel-side, named by the pid that took it. A second claim is refused with
// Err(Perm) rather than nested, and a claim clears the route only if it is
// still the holder, so two claimants may die in either order.
#pragma once

#include "io.h"
#include "kernel/key.h"
#include "kernel/screen.h"
#include "prog.h"

// What init enters /bin/sh with: out and err are the grid, and stdin is the
// console's cooked input (console.h) — which is what makes `cat` with no
// argument read what is typed, one level below the shell that spawned it.
Stdio stdio_console();

// Whether a stream is the grid rather than a pipe or a file: the sink
// stdio_console() installs is the whole of the difference. Sys::Tty's caller.
bool tty_is_console(const Stream &s);

// Enough to hold a burst of typing between two resumptions of a program that
// repaints on every key. Beyond it, keys drop — the policy key() already uses
// on the keyboard ring itself.
constexpr usize KEY_RING = 32;

using KeyRing = Channel<Key, KEY_RING>;

// Raw keys to a full-screen program: no echo and no line discipline. ^C is not
// delivered — it still cancels the pipeline, so a wedged editor stays killable.
//
// The ring is a heap block because a KeyRing inside a coroutine frame would
// push it past the allocator's top size class and cost a whole 64 KiB span.
struct KeyInput {
    explicit KeyInput(u32 pid);

    KeyInput(const KeyInput &)            = delete;
    KeyInput &operator=(const KeyInput &) = delete;

    ~KeyInput();

    bool ok() const { return ring_ != nullptr; }

    // Why it was refused: Perm when another pid holds the keyboard, NoMemory
    // when the ring would not allocate.
    Error error() const { return err_; }

    // Await it directly: `Result<Key> r = co_await keys.next();`, treating
    // Err(Again) as a stray wake, the way every other reader does.
    KeyRing::Recv next() { return ring_->recv(); }

private:
    KeyRing *ring_ = nullptr;
    Error err_     = Error::NoMemory;
};

// The alternate screen, as RAII: whoever holds this has the grid for as long
// as it lives, and the shell's screen comes back when it dies. A destructor
// rather than a call at the end, so that a process killed mid-paint gets its
// screen restored anyway — ~Proc in proctab.h is what runs it.
//
// There is no second grid: the cells are copied to a heap block and copied
// back, which is what "alternate screen" means when the terminal is an array
// rather than a byte stream. It lives here beside the two keyboard claims
// because all three answer the same question — who owns the terminal while a
// program has it.
struct FullScreen {
    explicit FullScreen(u32 pid);

    FullScreen(const FullScreen &)            = delete;
    FullScreen &operator=(const FullScreen &) = delete;

    ~FullScreen();

    // False when another pid holds the screen, or when the snapshot would not
    // allocate. The caller should give up: taking the screen without being
    // able to give it back is worse than not running at all.
    bool ok() const { return saved_ != nullptr; }

    Error error() const { return err_; }

private:
    Error err_   = Error::NoMemory;
    Cell *saved_ = nullptr;
    u32 cols_ = 0, rows_ = 0;
    u32 cursor_x_ = 0, cursor_y_ = 0;
    bool cursor_on_ = false;
};

// What the pump consults before it cooks. Null when nothing is claimed.
//
// There were three routes and there are two. The cooked one was `fg`'s, which
// borrowed the pump of the pipeline it was a stage of to feed the job it
// adopted; a shell that is a process feeds its own children through a pipe it
// holds, and `fg` is Sys::Fg and a wait.
KeyRing *tty_raw();

// Who holds each of the two claims a process makes, or 0 when it is free.
u32 tty_keys_owner();
u32 tty_screen_owner();
