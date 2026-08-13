#include "tty.h"

#include "io.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"

namespace {

Result<usize> to_screen(void *, Str s)
{
    screen_write(s);
    return s.size();
}

// Pointers, so the globals stay trivially destructible (CLAUDE.md).
KeyRing *g_raw = nullptr;
Pipe *g_cooked = nullptr;

} // namespace

Stdio stdio_console()
{
    Stream s{ to_screen, nullptr, nullptr };
    return Stdio{ null_source(), s, s };
}

KeyInput::KeyInput()
{
    ring_ = static_cast<KeyRing *>(heap_alloc(sizeof(KeyRing)));
    if (!ring_)
        return;
    new (ring_) KeyRing();
    prev_ = g_raw;
    g_raw = ring_;
}

KeyInput::~KeyInput()
{
    if (!ring_)
        return;
    if (g_raw == ring_)
        g_raw = prev_;
    ring_->~KeyRing();
    heap_free(ring_);
}

InputClaim::InputClaim(Pipe *to) : prev_(g_cooked)
{
    g_cooked = to;
}

InputClaim::~InputClaim()
{
    g_cooked = prev_;
}

KeyRing *tty_raw()
{
    return g_raw;
}

Pipe *tty_cooked()
{
    return g_cooked;
}
