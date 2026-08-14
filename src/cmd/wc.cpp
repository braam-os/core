#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// Counts over the raw chunks, so nothing here depends on where a chunk breaks
// — and a chunk now breaks where a syscall does rather than where a pipe did.
Task<i32> proc_main(Args args)
{
    Input in(args.tail(), SYS_STDIN);
    if (i32 bad = co_await in.open_all("wc"))
        co_return bad;

    u32 lines = 0, words = 0, bytes = 0;
    bool in_word = false;

    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                co_return r.error() == Error::Cancelled ? 130 : 1;
            break;
        }
        Str s = r.value().str();
        for (usize i = 0; i < s.size(); i++) {
            bytes++;
            if (s[i] == '\n')
                lines++;
            if (is_space(s[i]))
                in_word = false;
            else if (!in_word) {
                in_word = true;
                words++;
            }
        }
    }

    Buf<48> b;
    b.put(lines).put(' ').put(words).put(' ').put(bytes).put('\n');
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;
    co_return 0;
}
