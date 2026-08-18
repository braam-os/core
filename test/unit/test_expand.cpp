#include "harness.h"
#include "kernel/str.h"
#include "sh/expand.h"

namespace {

char buf[160];

// The shell the expander is given, as table literals: the callbacks are the
// whole reason `$` needs no syscall, and this is what proves it.
struct Ent {
    Str name, value;
};

Ent tab[12];
usize tabn;
Str pos[8];
usize posn;

// `set` must copy, exactly as the shell's table does — the value it is handed
// is a temporary of the expansion that asked for it.
char store[256];
usize storen;

Str keep(Str s)
{
    usize at = storen;
    for (usize i = 0; i < s.size() && storen < sizeof(store); i++)
        store[storen++] = s[i];
    return Str(store + at, storen - at);
}

// The specials answer through the same lookup the shell wires to var.cpp, so
// the test has to serve them too. `$?` and `$$` are fixed here; only `$#` moves.
char g_num[2];

bool cb_look(void *, Str name, Str &value)
{
    if (name.size() == 1) {
        switch (name[0]) {
        case '?':
            value = "7";
            return true;
        case '$':
            value = "42";
            return true;
        case '!':
            value = "0";
            return true;
        case '#':
            g_num[0] = char('0' + posn % 10);
            value    = Str(g_num, 1);
            return true;
        default:
            break;
        }
    }
    for (usize i = 0; i < tabn; i++)
        if (tab[i].name == name) {
            value = tab[i].value;
            return true;
        }
    return false;
}

bool cb_set(void *, Str name, Str value)
{
    for (usize i = 0; i < tabn; i++)
        if (tab[i].name == name) {
            tab[i].value = keep(value);
            return true;
        }
    if (tabn == sizeof(tab) / sizeof(tab[0]))
        return false;
    tab[tabn++] = Ent{ keep(name), keep(value) };
    return true;
}

usize cb_count(void *)
{
    return posn;
}

Str cb_at(void *, usize i)
{
    if (i == 0)
        return "sh";
    return i <= posn ? pos[i - 1] : Str();
}

constexpr Vars VARS = { nullptr, cb_look, cb_set, cb_count, cb_at };

void reset()
{
    tabn = posn = storen = 0;
}

void def(Str name, Str value)
{
    tab[tabn++] = Ent{ keep(name), keep(value) };
}

void set_args(usize n, Str a = Str(), Str b = Str(), Str c = Str())
{
    posn = 0;
    Str v[3]{ a, b, c };
    for (usize i = 0; i < n; i++)
        pos[posn++] = keep(v[i]);
}

// Renders what a raw word expands to: each field in braces, an error as `!`
// and, when a parameter is what failed, `!name:message`. With `marks`, a field
// is rendered as `^` per quoted byte and `.` per plain one instead of as its
// text.
Str shape(Str raw, bool marks, Split split)
{
    Vec<Field> out;
    ExpandErr err;
    usize n = 0;

    auto put = [&](Str s) {
        for (usize i = 0; i < s.size() && n < sizeof(buf); i++)
            buf[n++] = s[i];
    };

    if (expand_word(raw, VARS, split, out, err).is_err()) {
        put("!");
        if (!err.name.empty()) {
            put(err.name);
            put(":");
            put(err.message);
        }
        return Str(buf, n);
    }

    for (const Field &f : out) {
        put("{");
        if (!marks)
            put(f.text.str());
        else
            for (usize i = 0; i < f.mark.size() && n < sizeof(buf); i++)
                buf[n++] = f.mark[i] ? '^' : '.';
        put("}");
    }
    return Str(buf, n);
}

Str text(Str raw)
{
    return shape(raw, false, Split::Fields);
}

Str mark(Str raw)
{
    return shape(raw, true, Split::Fields);
}

Str one(Str raw)
{
    return shape(raw, false, Split::One);
}

} // namespace

void test_expand()
{
    test_begin("expand");
    reset();

    // A word with nothing to do comes through as itself, in one field.
    CHECK(text("echo") == "{echo}");
    CHECK(text("a-b_c.d/e") == "{a-b_c.d/e}");

    // Quotes come off, and what they enclose survives whole.
    CHECK(text("'a b'") == "{a b}");
    CHECK(text("\"a b\"") == "{a b}");
    CHECK(text("''") == "{}");
    CHECK(text("\"\"") == "{}");

    // Quoted runs concatenate into the one word the lexer decided they were.
    CHECK(text("a''b") == "{ab}");
    CHECK(text("'a'\"b\"c") == "{abc}");
    CHECK(text("x'y z'w") == "{xy zw}");

    // A backslash outside quotes takes the next character literally.
    CHECK(text("\\|") == "{|}");
    CHECK(text("a\\ b") == "{a b}");
    CHECK(text("\\\\") == "{\\}");
    CHECK(text("\\'") == "{'}");

    // Inside double quotes only a quote, a backslash and a dollar are
    // escapable; anything else keeps its backslash.
    CHECK(text("\"a\\\"b\"") == "{a\"b}");
    CHECK(text("\"a\\\\b\"") == "{a\\b}");
    CHECK(text("\"a\\nb\"") == "{a\\nb}");
    CHECK(text("\"\\$x\"") == "{$x}");

    // A single quote quotes everything, a backslash and a dollar included.
    CHECK(text("'a\\b'") == "{a\\b}");
    CHECK(text("'$x'") == "{$x}");
    CHECK(text("'\"'") == "{\"}");
    CHECK(text("\"'\"") == "{'}");

    // A backslash quotes one byte, so a multi-byte character behind one is
    // marked in its first byte only.
    CHECK(text("\\\xc3\xa9") == "{\xc3\xa9}");
    CHECK(mark("\\\xc3\xa9") == "{^.}");

    // The lexer would have refused all three; they are errors here too.
    CHECK(text("'a") == "!");
    CHECK(text("\"a") == "!");
    CHECK(text("a\\") == "!");

    // ---- `$` ----

    reset();
    def("x", "hi");
    def("empty", "");

    CHECK(text("$x") == "{hi}");
    CHECK(text("${x}") == "{hi}");
    CHECK(text("a${x}b") == "{ahib}");
    CHECK(text("\"$x\"") == "{hi}");
    CHECK(text("'$x'") == "{$x}");

    // A name runs while a name can; the byte that ends it is ordinary again.
    CHECK(text("$x.$x") == "{hi.hi}");
    CHECK(text("$xy") == "");
    CHECK(text("${x}y") == "{hiy}");

    // A `$` that no parameter can be made of is a `$`.
    CHECK(text("$") == "{$}");
    CHECK(text("$-") == "{$-}");
    CHECK(text("a$ b") == "{a$ b}");

    // **The empty quoted word against the empty unquoted one.** Both are a
    // zero-length text beside a zero-length mark, so only the flag tells them
    // apart — which is the invariant this stage exists to add.
    CHECK(text("\"$empty\"") == "{}");
    CHECK(text("$empty") == "");
    CHECK(text("$nosuch") == "");
    CHECK(text("\"$nosuch\"") == "{}");
    CHECK(text("") == "");
    CHECK(one("") == "{}");

    // The four operators, on set, set-but-empty and unset. There are no colon
    // forms: `${x-y}` asks whether x is *set*.
    CHECK(text("${x-alt}") == "{hi}");
    CHECK(text("${empty-alt}") == "");
    CHECK(text("${nosuch-alt}") == "{alt}");
    CHECK(text("${nosuch-}") == "");
    CHECK(text("${x+alt}") == "{alt}");
    CHECK(text("${empty+alt}") == "{alt}");
    CHECK(text("${nosuch+alt}") == "");
    CHECK(text("${x?}") == "{hi}");
    CHECK(text("${nosuch?}") == "!nosuch:parameter not set");
    CHECK(text("${nosuch?is wanted}") == "!nosuch:is wanted");

    // The default is expanded in its turn, and a `${` protects what is in it.
    CHECK(text("${nosuch-$x}") == "{hi}");
    CHECK(text("${nosuch-a b}") == "{a}{b}");
    CHECK(text("\"${nosuch-a b}\"") == "{a b}");
    CHECK(text("${nosuch") == "!");
    CHECK(text("${}") == "!");
    CHECK(text("${x*}") == "!");

    // `${x=y}` assigns, and only when it has to.
    reset();
    def("x", "hi");
    CHECK(text("${nosuch=made}") == "{made}");
    CHECK(text("$nosuch") == "{made}");
    CHECK(text("${x=other}") == "{hi}");
    CHECK(text("${1=no}") == "!");

    // ---- the specials ----

    reset();
    set_args(3, "a", "b b", "c");

    CHECK(text("$0") == "{sh}");
    CHECK(text("$1.$3") == "{a.c}");
    CHECK(text("$9") == "");
    CHECK(text("${2}") == "{b}{b}");
    CHECK(text("\"${2}\"") == "{b b}");
    CHECK(text("$#") == "{3}");
    CHECK(text("${4-none}") == "{none}");
    CHECK(text("${3-none}") == "{c}");

    // `$*` and `$@` differ only inside quotes, where `"$@"` breaks between the
    // parameters and `"$*"` joins them.
    CHECK(text("$*") == "{a}{b}{b}{c}");
    CHECK(text("$@") == "{a}{b}{b}{c}");
    CHECK(text("\"$*\"") == "{a b b c}");
    CHECK(text("\"$@\"") == "{a}{b b}{c}");
    CHECK(text("x\"$@\"y") == "{xa}{b b}{cy}");
    CHECK(one("$@") == "{a b b c}");

    // With nothing to expand into, `"$@"` is the one quoted run that leaves no
    // field at all — where `""` leaves an empty one.
    set_args(0);
    CHECK(text("\"$@\"") == "");
    CHECK(text("\"$*\"") == "{}");
    CHECK(text("x\"$@\"y") == "{xy}");
    CHECK(text("$#") == "{0}");

    // ---- field splitting ----

    reset();
    def("two", "a b");
    def("pad", "  a  b  ");
    def("path", "x:y");

    CHECK(text("$two") == "{a}{b}");
    CHECK(text("\"$two\"") == "{a b}");
    CHECK(text("$pad") == "{a}{b}");
    CHECK(text("p$two.q") == "{pa}{b.q}");
    CHECK(one("$two") == "{a b}");

    // A separator the word itself carries never splits: the lexer already
    // decided where the word ends.
    CHECK(text("$path") == "{x:y}");
    def("IFS", ":");
    CHECK(text("$path") == "{x}{y}");
    CHECK(text("a:b") == "{a:b}");
    CHECK(text("$two") == "{a b}");

    // An empty IFS splits nothing at all.
    reset();
    def("two", "a b");
    def("IFS", "");
    CHECK(text("$two") == "{a b}");

    // ---- the mark ----

    reset();
    def("star", "*");

    // What quoting produced is marked; what an expansion produced is not, so a
    // metacharacter out of a variable stays live for the glob matcher.
    CHECK(mark("abc") == "{...}");
    CHECK(mark("'abc'") == "{^^^}");
    CHECK(mark("a'b'c") == "{.^.}");
    CHECK(mark("\"ab\"c") == "{^^.}");
    CHECK(mark("a\\ b") == "{.^.}");
    CHECK(mark("''") == "{}");
    CHECK(mark("$star") == "{.}");
    CHECK(mark("\"$star\"") == "{^}");
    CHECK(mark("\\*") == "{^}");
}
