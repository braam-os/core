#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/string.h"
#include "kernel/text.h"

namespace {

// The two that exist. There are no signals, so this is not a table of them:
// 0 is the shell ending and 2 is the ^C an interactive shell survives. Every
// other number is refused rather than silently kept.
bool signal_of(Str w, u32 &sig)
{
    if (w == "0" || w == "EXIT") {
        sig = 0;
        return true;
    }
    if (w == "2" || w == "INT") {
        sig = 2;
        return true;
    }
    return false;
}

bool put_one(String &out, u32 sig)
{
    Str action;
    if (!trap_get(sig, action))
        return true;
    return out.append("trap -- '") && out.append(action) && out.append(sig ? "' 2\n" : "' 0\n");
}

} // namespace

// A builtin because the action is a text this shell must run in its own walk,
// and the walk is what a child does not have.
Task<i32> builtin_trap(Args args, ShIo io)
{
    if (args.size() == 1) {
        String out;
        if (!put_one(out, 0) || !put_one(out, 2))
            co_return 1;
        if (Task<Result<void>> t = write_all(io.out, out.str()))
            co_return (co_await t).is_ok() ? 0 : 1;
        co_return 1;
    }

    Str action  = args[1];
    bool remove = action == "-";
    usize first = 2;

    // `trap 0` with no action is v7's reset, and the only form where the first
    // operand is a signal.
    u32 sig = 0;
    if (args.size() == 2 && signal_of(action, sig)) {
        remove = true;
        first  = 1;
    }

    if (first >= args.size()) {
        co_await write_all(io.err, "usage: trap [<action>|-] <signal>...\n");
        co_return 2;
    }

    for (usize i = first; i < args.size(); i++) {
        if (!signal_of(args[i], sig)) {
            if (Task<void> e = errln("trap", args[i], Error::Unsupported))
                co_await e;
            co_return 1;
        }
        if (remove) {
            trap_clear(sig);
            continue;
        }
        // `trap '' 2` cannot be honoured: CancelState::cancelled is sticky, so
        // once a ^C has been delivered nothing can decline it. Refused rather
        // than accepted and ignored.
        if (action.empty() && sig == 2) {
            co_await write_all(io.err, "trap: cannot ignore an interrupt\n");
            co_return 1;
        }
        if (!trap_set(sig, action)) {
            co_await write_all(io.err, "trap: out of memory\n");
            co_return 1;
        }
    }
    co_return 0;
}
