#include "console.h"

#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "tty.h"

namespace {

// Words and a pointer, so the globals stay trivially destructible (CLAUDE.md).
// The cooked pipe holds Strings and therefore has a destructor, so it is a heap
// block built on first use rather than a namespace-scope Channel.
u32 g_fg[CONSOLE_FG_MAX];
usize g_fg_n      = 0;
u32 g_fg_by       = 0; // who armed it
u32 g_interrupts  = 0;
Pipe *g_cooked_in = nullptr;

Pipe &cooked()
{
    if (!g_cooked_in) {
        g_cooked_in = static_cast<Pipe *>(heap_alloc(sizeof(Pipe)));
        if (!g_cooked_in)
            panic("console: out of memory");
        new (g_cooked_in) Pipe();
    }
    return *g_cooked_in;
}

// End of input belonged to the foreground that was there when ^D was typed, so
// a new one gets a channel that is open again — and empty, since type-ahead
// meant for what has gone is not input for what replaces it. Reopening rather
// than rebuilding leaves a reader that is still unwinding parked on something
// that exists.
void rearm()
{
    cooked().clear();
    cooked().reopen();
}

} // namespace

bool console_fg_add(u32 pid, u32 by)
{
    if (g_fg_n >= CONSOLE_FG_MAX)
        return false;
    if (!g_fg_n) {
        g_fg_by = by;
        rearm();
    }
    g_fg[g_fg_n++] = pid;
    return true;
}

void console_fg_clear()
{
    g_fg_n  = 0;
    g_fg_by = 0;
}

usize console_fg_count()
{
    return g_fg_n;
}

bool console_fg_has(u32 pid)
{
    for (usize i = 0; i < g_fg_n; i++)
        if (g_fg[i] == pid)
            return true;
    return false;
}

u32 console_fg_owner()
{
    return g_fg_n ? g_fg_by : 0;
}

u32 console_interrupts()
{
    return g_interrupts;
}

Source console_input()
{
    return pipe_source(cooked());
}

bool console_is_input(const Source &s)
{
    return g_cooked_in && s.ctx == g_cooked_in;
}

Task<i32> console_pump()
{
    String line; // the cooked line, sent whole on Enter

    for (;;) {
        Result<Key> r = co_await keys().recv();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue; // a stray wake
            co_return 0;  // cancelled, which only happens on the way down
        }

        Key k = r.value();

        // ^C reaches whatever is in front, whatever is claimed: a program that
        // has taken the screen and stopped answering must still be killable,
        // and that is also M4's acceptance criterion. With nobody in front it
        // is an ordinary key, which is what lets a line editor abandon a line
        // instead of being cancelled by it.
        if ((k.mods & MOD_CTRL) && k.code == 'c' && g_fg_n) {
            screen_write("^C");
            screen_newline();
            for (usize i = 0; i < g_fg_n; i++)
                sched_cancel(g_fg[i]);
            g_interrupts++;
            line.clear();
            continue;
        }

        // Raw: no echo and no line discipline, since the claimant is painting
        // the screen itself.
        //
        // A full ring is waited on, briefly, before anything is dropped. This
        // loop never suspends while the channel has something in it, so a burst
        // — a paste, or typing between two frames — arrives here as one run,
        // and without the wait a line longer than the ring lost its tail before
        // the claimant was ever scheduled. A claimant that *is* a process needs
        // several of these per key, since every one of its reads is a syscall
        // and a step.
        //
        // Bounded, and through the timer queue at zero delay rather than by
        // parking on the ring: the host answers a delay of 0 with another pump
        // straight away, so this costs microseconds when somebody is reading
        // and ends in a dropped key when nobody is — which is what keeps a
        // wedged program from taking ^C down with it.
        for (usize spin = 0; spin < KEY_WAIT && tty_raw() && tty_raw()->full(); spin++)
            if ((co_await Sleep(0)).is_err())
                co_return 0;
        if (KeyRing *raw = tty_raw()) {
            raw->try_send(k);
            continue;
        }

        Pipe &to = cooked();

        if (k.mods & MOD_CTRL) {
            if (k.code != 'd')
                continue;

            // End of input, not end of the console. A partial line goes first:
            // ^D after typing is "send what I have", as it is anywhere. The
            // channel is re-armed when something else is put in front.
            if (!line.empty()) {
                String chunk;
                if (chunk.assign(line.str()))
                    to.try_send(move(chunk));
                line.clear();
            }
            to.close();
            continue;
        }

        // A line at a time, which is what "cooked" means and what the pipe can
        // hold: one chunk per keystroke filled its eight slots after seven
        // characters, and the pump drops rather than parks, so the rest of the
        // line was lost. Echo stays per keystroke, so typing still appears as
        // it is typed.
        if (k.code == KEY_ENTER) {
            screen_newline();
            if (line.push('\n')) {
                String chunk;
                if (chunk.assign(line.str()))
                    to.try_send(move(chunk));
            }
            line.clear();
        } else if (k.printable()) {
            char utf8[4];
            usize n = utf8_encode(k.code, utf8);
            line.append(Str(utf8, n));
            screen_put(k.code);
        }
    }
}
