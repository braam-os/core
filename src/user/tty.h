// Stdio backed directly by the cell grid, and the keyboard's routing table.
//
// The screen is not behind a byte channel: §2.3 says the terminal *is* a cell
// grid, so a queue in front of it would add a copy and a scheduling hop to
// reach an array screen_write already fills, and would put terminal output
// behind something that can drop.
//
// The keyboard is the other way round: it is one Channel with one receiver
// (channel.h), and while a pipeline runs that receiver is the tty pump. A
// full-screen program therefore does not take the keyboard — it asks the pump
// to route to it, which is what the two claims below are. Both are RAII and
// both restore whatever was in force before, so a claim nests.
#pragma once

#include "io.h"
#include "kernel/key.h"
#include "prog.h"

// out and err are the same sink, and stdin is empty. A pipeline replaces the
// ends it redirects; the rest stay pointed here.
Stdio stdio_console();

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
    KeyInput();

    KeyInput(const KeyInput &)            = delete;
    KeyInput &operator=(const KeyInput &) = delete;

    ~KeyInput();

    bool ok() const { return ring_ != nullptr; }

    // Await it directly: `Result<Key> r = co_await keys.next();`, treating
    // Err(Again) as a stray wake, the way every other reader does.
    KeyRing::Recv next() { return ring_->recv(); }

private:
    KeyRing *ring_ = nullptr;
    KeyRing *prev_ = nullptr;
};

// Cooked bytes to another job's stdin — what `fg` needs, since the pump that is
// running belongs to the pipeline `fg` is a stage of, not to the job it adopts.
struct InputClaim {
    // Null routes back to the pump's own job — a job whose stages have all
    // finished has no pipe left to claim.
    explicit InputClaim(Pipe *to);

    InputClaim(const InputClaim &)            = delete;
    InputClaim &operator=(const InputClaim &) = delete;

    ~InputClaim();

private:
    Pipe *prev_ = nullptr;
};

// What the pump consults, in this order. Both are null when nothing is claimed.
KeyRing *tty_raw();
Pipe *tty_cooked();
