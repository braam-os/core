#include "tty.h"

#include "io.h"
#include "kernel/screen.h"

namespace {

Result<usize> to_screen(void *, Str s)
{
    screen_write(s);
    return s.size();
}

} // namespace

Stdio stdio_console()
{
    Stream s{ to_screen, nullptr, nullptr };
    return Stdio{ null_source(), s, s };
}
