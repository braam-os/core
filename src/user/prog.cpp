#include "prog.h"

namespace {

// Zero-initialised and trivially destructible: an ordinary global is enough.
Program *g_head;

bool before(Str a, Str b) {
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]);
    return a.size() < b.size();
}

} // namespace

// Sorted insert, so `help` needs no sort and its output does not depend on the
// unspecified order of static initialisation across translation units.
void program_register(Program &p) {
    Program **slot = &g_head;
    while (*slot && before((*slot)->name, p.name))
        slot = &(*slot)->next;
    p.next = *slot;
    *slot = &p;
}

const Program *program_find(Str name) {
    for (const Program *p = g_head; p; p = p->next)
        if (p->name == name)
            return p;
    return nullptr;
}

const Program *program_first() {
    return g_head;
}
