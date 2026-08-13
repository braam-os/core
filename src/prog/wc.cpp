#include "kernel/fmt.h"
#include "kernel/text.h"
#include "user/io.h"
#include "user/prog.h"

// Counts over the raw chunks, so nothing here depends on where a chunk breaks.
BRAAM_PROGRAM(prog_wc, "wc", "[<file>...] — count the lines, words and bytes")
{
    Inputs files;
    if (i32 bad = co_await open_inputs(files, args.tail(), "wc", io))
        co_return bad;

    Source in = input_of(files, io);
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
    if ((co_await write_all(io.out, b.str())).is_err())
        co_return 1;
    co_return 0;
}
