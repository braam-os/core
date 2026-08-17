#include "harness.h"
#include "proc/opt.h"

namespace {

// Every flag a line yields, rendered: a bare letter, `letter=value` when it
// took one, `!letter` on an error, then a slash and the operands. One
// comparison checks the letters, their order, the values and what is left.
Str scan(Args v, Opts spec, char *out, usize cap, Error &err)
{
    OptParse p(v, spec);
    Opt o;
    usize n = 0;

    auto put = [&](Str s) {
        for (usize i = 0; i < s.size() && n < cap; i++)
            out[n++] = s[i];
    };

    for (;;) {
        Result<bool> more = p.next(o);
        if (more.is_err()) {
            err = more.error();
            if (n)
                put(",");
            put("!");
            put(Str(&o.name, 1));
            break;
        }
        if (!more.value())
            break;
        if (n)
            put(",");
        put(Str(&o.name, 1));
        if (!o.value.empty()) {
            put("=");
            put(o.value);
        }
    }
    put("/");
    Args rest = p.rest();
    for (usize i = 0; i < rest.size(); i++) {
        if (i)
            put(" ");
        put(rest[i]);
    }
    return Str(out, n);
}

// ls's letters, and a valued one for head's -n.
constexpr Opts FLAGS{ "1CRSdhlr", "" };
constexpr Opts VALUED{ "v", "n" };

} // namespace

void test_opt()
{
    test_begin("opt");

    char buf[128];
    Error err = Error::Invalid;

    // Nothing at all, and operands with no flags.
    {
        Str argv[] = { "ls" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "/");
    }
    {
        Str argv[] = { "ls", "a", "b" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "/a b");
    }

    // Separate, bundled, and both — the bundle is read left to right.
    {
        Str argv[] = { "ls", "-l", "-R", "x" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l,R/x");
    }
    {
        Str argv[] = { "ls", "-lR", "x" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l,R/x");
    }
    {
        Str argv[] = { "ls", "-lR", "-S", "x" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l,R,S/x");
    }

    // The first operand ends the options: a flag after one is an operand too.
    {
        Str argv[] = { "ls", "-l", "x", "-R" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l/x -R");
    }

    // `--` ends them and is consumed; `-` alone is an operand, not a flag.
    {
        Str argv[] = { "ls", "-l", "--", "-R" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l/-R");
    }
    {
        Str argv[] = { "ls", "-", "-l" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "/- -l");
    }
    {
        Str argv[] = { "ls", "--" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "/");
    }

    // A valued letter takes the rest of its word, or the whole of the next.
    {
        Str argv[] = { "head", "-n5", "f" };
        CHECK(scan(Args{ argv }, VALUED, buf, sizeof(buf), err) == "n=5/f");
    }
    {
        Str argv[] = { "head", "-n", "5", "f" };
        CHECK(scan(Args{ argv }, VALUED, buf, sizeof(buf), err) == "n=5/f");
    }
    // It ends the bundle: what follows the letter is the value, not a flag.
    {
        Str argv[] = { "head", "-vn12", "f" };
        CHECK(scan(Args{ argv }, VALUED, buf, sizeof(buf), err) == "v,n=12/f");
    }
    {
        Str argv[] = { "head", "-vn", "12", "f" };
        CHECK(scan(Args{ argv }, VALUED, buf, sizeof(buf), err) == "v,n=12/f");
    }

    // A valued letter with nothing after it, and a letter the program does not
    // take — two different mistakes, and the letter at fault names itself.
    {
        Str argv[] = { "head", "-n" };
        CHECK(scan(Args{ argv }, VALUED, buf, sizeof(buf), err) == "!n/");
        CHECK(err == Error::NotFound);
    }
    {
        Str argv[] = { "ls", "-z", "x" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "!z/x");
        CHECK(err == Error::Invalid);
    }
    // From inside a bundle, and what was read before it still came out.
    {
        Str argv[] = { "ls", "-lz", "x" };
        CHECK(scan(Args{ argv }, FLAGS, buf, sizeof(buf), err) == "l,!z/x");
        CHECK(err == Error::Invalid);
    }
}
