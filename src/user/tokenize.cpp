#include "tokenize.h"

#include "kernel/text.h"

Result<void> tokenize(Str line, Vec<Str> &argv) {
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i]))
            i++;

        usize start = i;
        while (i < line.size() && !is_space(line[i]))
            i++;

        if (i > start && !argv.push(line.substr(start, i - start)))
            return Err(Error::NoMemory);
    }
    return {};
}
