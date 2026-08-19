#include "parse.h"

#include "kernel/alloc.h"
#include "kernel/host.h"
#include "name.h"
#include "tokenize.h"

Result<void> Tree::keep(Str s, Vec<Slice> &into)
{
    if (frozen_)
        panic("Tree: a word added after freeze");
    usize off = store_.size();
    if (!store_.append(s))
        return Err(Error::NoMemory);
    if (!into.push(Slice{ off, s.size() })) {
        store_.truncate(off);
        return Err(Error::NoMemory);
    }
    return {};
}

namespace {

// `name=` on the raw word, before any quote comes off: a name is what a `$`
// would accept, so `a2=1` assigns and `2a=1` is an ordinary word.
bool is_assignment(Str w)
{
    usize i = 0;
    while (i < w.size() && (i ? is_name_char(w[i]) : is_name_start(w[i])))
        i++;
    return i > 0 && i < w.size() && w[i] == '=';
}

} // namespace

Result<void> Tree::add_word(Str w, bool assignable)
{
    // An assignment counts only while nothing else has been seen: in `ls x=1`
    // the second word is an argument.
    bool leading = words_.size() - argv0_ == assign_n_;
    TRY_VOID(keep(w, words_));
    if (assignable && leading && is_assignment(w))
        assign_n_++;
    return {};
}

Result<void> Tree::add_redirect(Redir kind, Str target, bool literal)
{
    TRY_VOID(keep(target, targets_));
    if (!redirs_.push(Redirect{ kind, targets_.size() - 1, literal }))
        return Err(Error::NoMemory);
    return {};
}

Result<void> Tree::end_command()
{
    Command c;
    c.argv0   = argv0_;
    c.argc    = words_.size() - argv0_;
    c.redir0  = redir0_;
    c.redirn  = redirs_.size() - redir0_;
    c.assignn = assign_n_;
    if (!cmds_.push(c))
        return Err(Error::NoMemory);

    argv0_    = words_.size();
    redir0_   = redirs_.size();
    assign_n_ = 0;
    return {};
}

Result<u32> Tree::add_node(const Node &n)
{
    // Node 0 is the Nop every "nothing" index points at.
    if (nodes_.empty() && !nodes_.push(Node{}))
        return Err(Error::NoMemory);
    if (!nodes_.push(n))
        return Err(Error::NoMemory);
    if (n.kind == Kind::FuncDef)
        defines_ = true;
    return u32(nodes_.size() - 1);
}

Result<void> Tree::own_source()
{
    if (!source_.empty() || line_.empty())
        return {};
    if (!source_.assign(line_))
        return Err(Error::NoMemory);
    line_ = source_.str();
    return {};
}

Result<u32> Tree::add_kids(Span<const u32> kids)
{
    usize off = kids_.size();
    if (!kids_.reserve(off + kids.size()))
        return Err(Error::NoMemory);
    for (u32 k : kids)
        kids_.push(k);
    return u32(off);
}

Result<u32> Tree::add_ops(Span<const u8> ops)
{
    usize off = ops_.size();
    if (!ops_.reserve(off + ops.size()))
        return Err(Error::NoMemory);
    for (u8 o : ops)
        ops_.push(o);
    return u32(off);
}

// The Str tables are built only here, because store_ reallocates while it
// grows and every view into it would move. Moving a frozen Tree is still
// safe: String and Vec move by stealing the pointer, so nothing shifts.
Result<void> Tree::freeze()
{
    if (frozen_)
        return {};
    if (nodes_.empty() && !nodes_.push(Node{}))
        return Err(Error::NoMemory);
    if (!word_view_.reserve(words_.size()) || !target_view_.reserve(targets_.size()))
        return Err(Error::NoMemory);
    for (const Slice &s : words_)
        word_view_.push(Str(store_.data() + s.off, s.len));
    for (const Slice &s : targets_)
        target_view_.push(Str(store_.data() + s.off, s.len));
    frozen_ = true;
    return {};
}

Str Tree::text(u32 i) const
{
    const Node &n = nodes_[i];
    if (n.kind != Kind::Pipe || n.d <= n.c)
        return Str();
    return line_.substr(n.c, n.d - n.c);
}

Args Tree::args(usize i) const
{
    if (!frozen_)
        panic("Tree: args before freeze");
    const Command &c = cmds_[i];
    return Args{ Span<const Str>(word_view_.data() + c.argv0 + c.assignn, c.argc - c.assignn) };
}

Args Tree::assigns(usize i) const
{
    if (!frozen_)
        panic("Tree: assigns before freeze");
    const Command &c = cmds_[i];
    return Args{ Span<const Str>(word_view_.data() + c.argv0, c.assignn) };
}

Span<const Redirect> Tree::redirects(usize i) const
{
    const Command &c = cmds_[i];
    return Span<const Redirect>(redirs_.data() + c.redir0, c.redirn);
}

Str Tree::target(const Redirect &r) const
{
    if (!frozen_)
        panic("Tree: target before freeze");
    return target_view_[r.target];
}

namespace {

Option<Redir> redir_of(Tok t)
{
    switch (t) {
    case Tok::Less:
        return Redir::In;
    case Tok::Great:
        return Redir::Out;
    case Tok::DGreat:
        return Redir::Append;
    case Tok::ErrGreat:
        return Redir::ErrOut;
    case Tok::ErrDGreat:
        return Redir::ErrAppend;
    case Tok::DLess:
    case Tok::DLessDash:
        return Redir::Here;
    case Tok::GreatAnd:
        return Redir::OutDup;
    case Tok::ErrGreatAnd:
        return Redir::ErrDup;
    default:
        return None;
    }
}

// Recursive descent over one token of lookahead. Every level returns a node
// index; 0 never means a node, so it can mean "none".
struct Parser {
    Parser(Str text, Tree &tree, ParseErr &e) : lx(text), t(tree), err(e) {}

    Lexer lx;
    Tree &t;
    ParseErr &err;

    Tok tok = Tok::End;
    Str word;
    usize begin = 0; // where `tok` starts in the text
    usize prev  = 0; // where the token before it ended

    // Where the lexer resumes once the line ends: a here-doc's body lies
    // between that newline and its terminator, and this parser has taken it
    // already. 0 is "nothing pending".
    usize here_resume = 0;

    Result<void> advance()
    {
        prev          = lx.pos();
        Result<Tok> r = lx.next(word);
        if (r.is_err()) {
            // The lexer refuses only something left unclosed, which is exactly
            // the case a prompt should keep reading.
            err.message = "unterminated quote, ${ or $(";
            err.more    = true;
            return Err(Error::Invalid);
        }
        tok   = r.value();
        begin = lx.begin();

        // The newline that ends the command is delivered; what follows it is
        // the body, so the lexer starts again past the terminator.
        if (tok == Tok::Newline && here_resume) {
            lx.seek(here_resume);
            here_resume = 0;
        }
        return {};
    }

    // Takes a here-doc's body out of the text: the lines after the one being
    // parsed, up to one holding just the delimiter. Pure, because
    // line_incomplete parses the same text again on every keystroke line.
    Result<void> here_body(Redir kind, bool trim);

    // Both return the Error rather than a Result, so one spelling serves a
    // level that yields a node index and one that yields nothing.
    Error fail(Str m, bool more = false)
    {
        err.message = m;
        err.more    = more;
        return Error::Invalid;
    }

    Error oom()
    {
        err.message = "out of memory";
        return Error::NoMemory;
    }

    // Reserved by position: this is only ever asked where a command may
    // start, and a raw word still carries its quotes, so `'do'` cannot match.
    bool reserved(Str w) const { return tok == Tok::Word && word == w; }

    bool ends_list() const
    {
        return tok == Tok::End || tok == Tok::DSemi || tok == Tok::RParen || reserved("}") ||
               reserved("then") || reserved("elif") || reserved("else") || reserved("fi") ||
               reserved("do") || reserved("done") || reserved("esac");
    }

    Result<u32> list(u32 nest);
    Result<u32> and_or(u32 nest);
    Result<u32> pipeline(u32 nest);
    Result<u32> compound(u32 nest, bool &any);
    Result<u32> if_clause(u32 nest);
    Result<u32> loop_clause(u32 nest, bool until);
    Result<u32> for_clause(u32 nest);
    Result<u32> case_clause(u32 nest);
    Result<void> want(Str w);
    Result<void> skip_seps();
    Result<void> skip_newlines();
    Result<void> redirect(bool &took);
    Result<void> simple(Str first, bool have_first);
    Result<u32> func_def(u32 nest, Str name);
};

// A here-doc's delimiter with its quotes off, and whether it had any — a
// quoted one means the body is used as written. Not the expander's job: this
// runs at parse time, where nothing may allocate into the shell.
bool plain_word(Str w, char *out, usize cap, usize &n)
{
    bool quoted = false;
    n           = 0;
    for (usize i = 0; i < w.size() && n < cap; i++) {
        char c = w[i];
        if (c == '\'' || c == '"' || c == '\\') {
            quoted = true;
            continue;
        }
        out[n++] = c;
    }
    return quoted;
}

// What a separator or an operator is called in a diagnostic.
Str near_of(Tok tok)
{
    switch (tok) {
    case Tok::Pipe:
        return "syntax error near '|'";
    case Tok::AndIf:
        return "syntax error near '&&'";
    case Tok::OrIf:
        return "syntax error near '||'";
    case Tok::Amp:
        return "syntax error near '&'";
    case Tok::Semi:
        return "syntax error near ';'";
    case Tok::DSemi:
        return "syntax error near ';;'";
    case Tok::LParen:
        return "syntax error near '('";
    case Tok::RParen:
        return "syntax error near ')'";
    default:
        return "syntax error: expected a command";
    }
}

Result<void> Parser::here_body(Redir kind, bool trim)
{
    char delim[64];
    usize dn     = 0;
    bool literal = plain_word(word, delim, sizeof(delim), dn);
    Str end(delim, dn);

    Str text = lx.text();

    // After the newline that ends this command, or after the body a here-doc
    // in front of this one already took.
    usize at = here_resume;
    if (!at) {
        at = text.find('\n', lx.pos());
        if (at == Str::npos)
            return Err(fail("syntax error: a here-document needs a body", true));
        at++;
    }

    String body;
    usize i = at;
    for (;;) {
        usize nl   = text.find('\n', i);
        usize stop = nl == Str::npos ? text.size() : nl;
        Str line   = text.substr(i, stop - i);

        // `<<-` strips leading tabs from the body and from the terminator.
        Str bare = line;
        if (trim)
            while (!bare.empty() && bare[0] == '\t')
                bare = bare.substr(1);

        if (bare == end) {
            here_resume = nl == Str::npos ? text.size() : nl + 1;
            break;
        }
        // The text ran out before the terminator, so a prompt asks for more.
        if (nl == Str::npos)
            return Err(fail("syntax error: a here-document needs its delimiter", true));

        if (!body.append(bare) || !body.push('\n'))
            return Err(oom());
        i = nl + 1;
    }

    if (t.add_redirect(kind, body.str(), literal).is_err())
        return Err(oom());
    return {};
}

// One redirection, if the token is one. `took` says whether it was.
Result<void> Parser::redirect(bool &took)
{
    Option<Redir> r = redir_of(tok);
    took            = bool(r);
    if (!took)
        return {};

    bool trim = tok == Tok::DLessDash;
    TRY_VOID(advance());
    if (tok != Tok::Word)
        return Err(fail("syntax error: expected a file name"));

    if (r.value() == Redir::Here) {
        TRY_VOID(here_body(r.value(), trim));
    } else if (r.value() == Redir::OutDup || r.value() == Redir::ErrDup) {
        // `>&-` would have to say "this stream is closed", and nothing in the
        // ABI can: there is no null sink and no closed descriptor.
        if (word == "-")
            return Err(fail("syntax error: >&- is not supported"));
        if (word != "1" && word != "2")
            return Err(fail("syntax error: >& wants 1 or 2"));
        if (t.add_redirect(r.value(), word).is_err())
            return Err(oom());
    } else if (t.add_redirect(r.value(), word).is_err()) {
        return Err(oom());
    }
    return advance();
}

// `first` is a name the caller ate to see whether a `(` followed it.
Result<void> Parser::simple(Str first, bool have_first)
{
    bool any = have_first;

    if (have_first && t.add_word(first).is_err())
        return Err(oom());

    for (;;) {
        if (tok == Tok::Word) {
            if (t.add_word(word).is_err())
                return Err(oom());
            any = true;
            TRY_VOID(advance());
            continue;
        }
        bool took = false;
        TRY_VOID(redirect(took));
        if (!took)
            break;
    }

    // A redirection alone is not a command: `> f` truncates nothing here.
    // Never `more`: a text ending *inside* something is caught where its
    // operator was, so reaching here means it ended after a complete one.
    if (!any)
        return Err(fail(near_of(tok)));
    if (t.end_command().is_err())
        return Err(oom());
    return {};
}

// The word after a construct's keyword, which the construct must have.
Result<void> Parser::want(Str w)
{
    if (reserved(w))
        return advance();

    // A literal per branch, not one conditional expression: Str's length folds
    // only when the pointer is constant (Concept.md §C.3). Out of text asks
    // for more; anything else is a mistake with a whole line behind it.
    if (w == "then")
        err.message = "syntax error: expected 'then'";
    else if (w == "do")
        err.message = "syntax error: expected 'do'";
    else if (w == "done")
        err.message = "syntax error: expected 'done'";
    else if (w == "fi")
        err.message = "syntax error: expected 'fi'";
    else if (w == "in")
        err.message = "syntax error: expected 'in'";
    else if (w == "esac")
        err.message = "syntax error: expected 'esac'";
    else
        err.message = "syntax error: expected '}'";
    err.more = tok == Tok::End;
    return Err(Error::Invalid);
}

Result<void> Parser::skip_seps()
{
    while (tok == Tok::Semi || tok == Tok::Newline)
        TRY_VOID(advance());
    return {};
}

// A `case` takes newlines where it takes nothing else, so `;` is not skipped.
Result<void> Parser::skip_newlines()
{
    while (tok == Tok::Newline)
        TRY_VOID(advance());
    return {};
}

// Pairs in a flat run rather than v7's nest, because the walk recurses.
Result<u32> Parser::if_clause(u32 nest)
{
    Vec<u32> kids;
    u32 els = 0;

    for (;;) {
        TRY_VOID(advance()); // `if` or `elif`
        u32 cond = TRY(list(nest + 1));
        TRY_VOID(want("then"));
        u32 body = TRY(list(nest + 1));
        if (!kids.push(cond) || !kids.push(body))
            return Err(oom());
        if (!reserved("elif"))
            break;
    }

    if (reserved("else")) {
        TRY_VOID(advance());
        els = TRY(list(nest + 1));
    }
    TRY_VOID(want("fi"));

    u32 off = TRY(t.add_kids(kids));
    return t.add_node(Tree::Node{ Tree::Kind::If, false, off, u32(kids.size() / 2), els, 0 });
}

Result<u32> Parser::loop_clause(u32 nest, bool until)
{
    TRY_VOID(advance()); // `while` or `until`
    u32 cond = TRY(list(nest + 1));
    TRY_VOID(want("do"));
    u32 body = TRY(list(nest + 1));
    TRY_VOID(want("done"));

    Tree::Kind k = until ? Tree::Kind::Until : Tree::Kind::While;
    return t.add_node(Tree::Node{ k, false, cond, body, 0, 0 });
}

Result<u32> Parser::for_clause(u32 nest)
{
    TRY_VOID(advance()); // `for`
    if (tok != Tok::Word || !is_name(word))
        return Err(fail("syntax error: expected a name after 'for'", tok == Tok::End));

    // Name and words as one command, so the command expander serves them.
    // Not assignable: `for i in x=1` is an ordinary word.
    u32 cmd = u32(t.size());
    if (t.add_word(word, false).is_err())
        return Err(oom());
    TRY_VOID(advance());

    u32 has_in = 0;
    if (reserved("in")) {
        has_in = 1;
        TRY_VOID(advance());
        while (tok == Tok::Word) {
            if (t.add_word(word, false).is_err())
                return Err(oom());
            TRY_VOID(advance());
        }
    }
    if (t.end_command().is_err())
        return Err(oom());

    TRY_VOID(skip_seps());
    TRY_VOID(want("do"));
    u32 body = TRY(list(nest + 1));
    TRY_VOID(want("done"));

    return t.add_node(Tree::Node{ Tree::Kind::For, false, body, cmd, has_in, 0 });
}

// Arms as a flat run of pairs, as `if`'s branches are. Each arm's patterns are
// one command, so the command expander serves them; not assignable, since
// `case x in a=1)` is an ordinary pattern.
Result<u32> Parser::case_clause(u32 nest)
{
    TRY_VOID(advance()); // `case`
    if (tok != Tok::Word)
        return Err(fail("syntax error: expected a word after 'case'", tok == Tok::End));

    u32 subject = u32(t.size());
    if (t.add_word(word, false).is_err() || t.end_command().is_err())
        return Err(oom());
    TRY_VOID(advance());

    TRY_VOID(skip_newlines());
    TRY_VOID(want("in"));
    TRY_VOID(skip_newlines());

    Vec<u32> kids;
    while (!reserved("esac")) {
        if (tok == Tok::End)
            return Err(fail("syntax error: expected 'esac'", true));
        if (tok == Tok::LParen)
            TRY_VOID(advance());

        u32 pats = u32(t.size());
        for (;;) {
            if (tok != Tok::Word)
                return Err(fail("syntax error: expected a pattern", tok == Tok::End));
            if (t.add_word(word, false).is_err())
                return Err(oom());
            TRY_VOID(advance());
            if (tok != Tok::Pipe)
                break;
            TRY_VOID(advance());
        }
        if (tok != Tok::RParen)
            return Err(fail("syntax error: expected ')'", tok == Tok::End));
        if (t.end_command().is_err())
            return Err(oom());
        TRY_VOID(advance());

        // The patterns are ended before the body, since a command is built
        // through one pair of cursors shared by the whole parse.
        u32 body = TRY(list(nest + 1));
        if (!kids.push(pats) || !kids.push(body))
            return Err(oom());

        if (tok != Tok::DSemi)
            break;
        TRY_VOID(advance());
        TRY_VOID(skip_newlines());
    }
    TRY_VOID(want("esac"));

    u32 off = TRY(t.add_kids(kids));
    return t.add_node(Tree::Node{ Tree::Kind::Case, false, off, u32(kids.size() / 2), subject, 0 });
}

// `name ( ) compound`, with the name and the `(` already eaten. The body is a
// compound, so the name rides in a command of its own as `for`'s does.
Result<u32> Parser::func_def(u32 nest, Str name)
{
    TRY_VOID(advance()); // `(`
    if (tok != Tok::RParen) {
        // Anything between the parentheses means this was never a definition:
        // `echo (x)` is a `(` where no grammar takes one.
        if (tok == Tok::End)
            return Err(fail("syntax error: expected ')'", true));
        return Err(fail("syntax error near '('"));
    }
    TRY_VOID(advance());
    TRY_VOID(skip_newlines());

    u32 cmd = u32(t.size());
    if (t.add_word(name, false).is_err() || t.end_command().is_err())
        return Err(oom());

    bool any = false;
    u32 body = TRY(compound(nest + 1, any));
    if (!any)
        return Err(fail("syntax error: expected a compound command", tok == Tok::End));

    return t.add_node(Tree::Node{ Tree::Kind::FuncDef, false, body, cmd, 0, 0 });
}

// A compound command, or 0 with `any` false when the token is not one.
Result<u32> Parser::compound(u32 nest, bool &any)
{
    any = true;
    if (reserved("{")) {
        TRY_VOID(advance());
        u32 body = TRY(list(nest + 1));
        TRY_VOID(want("}"));
        return t.add_node(Tree::Node{ Tree::Kind::Group, false, body, 0, 0, 0 });
    }
    if (reserved("if"))
        return if_clause(nest);
    if (reserved("while"))
        return loop_clause(nest, false);
    if (reserved("until"))
        return loop_clause(nest, true);
    if (reserved("for"))
        return for_clause(nest);
    if (reserved("case"))
        return case_clause(nest);
    if (tok == Tok::LParen) {
        TRY_VOID(advance());
        u32 body = TRY(list(nest + 1));
        if (tok != Tok::RParen)
            return Err(fail("syntax error: expected ')'", tok == Tok::End));
        TRY_VOID(advance());
        return t.add_node(Tree::Node{ Tree::Kind::Subshell, false, body, 0, 0, 0 });
    }

    any = false;
    return u32(0);
}

Result<u32> Parser::pipeline(u32 nest)
{
    usize text0 = begin;

    bool neg = false;
    if (reserved("!")) {
        neg = true;
        TRY_VOID(advance());
    }

    bool is_compound = false;
    u32 n            = TRY(compound(nest, is_compound));

    // A definition is told from a command only by the token after the name, so
    // the name is eaten here and handed to whichever it turns out to be. After
    // compound(), which has first refusal: every keyword is a name too.
    Str name;
    bool named = false;
    if (!is_compound && !neg && tok == Tok::Word && is_name(word)) {
        name  = word;
        named = true;
        TRY_VOID(advance());
        if (tok == Tok::LParen) {
            n           = TRY(func_def(nest, name));
            is_compound = true;
            named       = false;
        }
    }

    if (is_compound) {
        // A pipeline's stages are commands, so a compound cannot be one of
        // them until they can be nodes. Redirecting it is another matter: the
        // walk hands its own base down.
        if (tok == Tok::Pipe)
            return Err(fail("syntax error: a compound command cannot be piped yet"));
        if (redir_of(tok)) {
            u32 cmd = u32(t.size());
            for (;;) {
                bool took = false;
                TRY_VOID(redirect(took));
                if (!took)
                    break;
            }
            if (t.end_command().is_err())
                return Err(oom());
            t.set_redirs(n, cmd);
        }
    } else {
        u32 first = u32(t.size());
        TRY_VOID(simple(name, named));
        while (tok == Tok::Pipe) {
            TRY_VOID(advance());
            while (tok == Tok::Newline)
                TRY_VOID(advance());
            if (tok == Tok::End)
                return Err(fail("syntax error: expected a command", true));
            TRY_VOID(simple(Str(), false));
        }
        u32 last = u32(t.size());
        if (last - first > Tree::MAX_STAGES)
            return Err(fail("too many commands in a pipeline"));
        n = TRY(
            t.add_node(Tree::Node{ Tree::Kind::Pipe, false, first, last, u32(text0), u32(prev) }));
    }

    if (neg)
        n = TRY(t.add_node(Tree::Node{ Tree::Kind::Not, false, n, 0, 0, 0 }));
    return n;
}

// A chain rather than a left-leaning pair per link, so the walk's depth is the
// text's nesting depth and not the number of `&&`s on one line.
Result<u32> Parser::and_or(u32 nest)
{
    u32 first = TRY(pipeline(nest));
    if (tok != Tok::AndIf && tok != Tok::OrIf)
        return first;

    Vec<u32> kids;
    Vec<u8> ops;
    if (!kids.push(first))
        return Err(oom());

    while (tok == Tok::AndIf || tok == Tok::OrIf) {
        if (!ops.push(u8(tok == Tok::AndIf ? Tree::Op::And : Tree::Op::Or)))
            return Err(oom());
        TRY_VOID(advance());
        while (tok == Tok::Newline)
            TRY_VOID(advance());
        if (tok == Tok::End)
            return Err(fail("syntax error: expected a command", true));
        u32 n = TRY(pipeline(nest));
        if (!kids.push(n))
            return Err(oom());
    }

    u32 off  = TRY(t.add_kids(kids));
    u32 oops = TRY(t.add_ops(ops));
    return t.add_node(Tree::Node{ Tree::Kind::AndOr, false, off, u32(kids.size()), oops, 0 });
}

Result<u32> Parser::list(u32 nest)
{
    if (nest >= Tree::MAX_NEST)
        return Err(fail("too deeply nested"));

    Vec<u32> kids;

    for (;;) {
        while (tok == Tok::Semi || tok == Tok::Newline)
            TRY_VOID(advance());
        if (ends_list())
            break;

        u32 n = TRY(and_or(nest));

        if (tok == Tok::Amp) {
            // Only a pipeline may go into the background: the shell keeps
            // running while it does, and nothing in a process can wait for a
            // sibling task, so a list would have nobody to run its rest.
            if (t.node(n).kind != Tree::Kind::Pipe)
                return Err(fail("cannot run a list in the background"));
            t.set_background(n);
            TRY_VOID(advance());
        } else if (tok != Tok::Semi && tok != Tok::Newline && !ends_list()) {
            return Err(fail(near_of(tok)));
        }

        if (!kids.push(n))
            return Err(oom());
    }

    if (kids.empty())
        return u32(0);
    if (kids.size() == 1)
        return kids[0];

    u32 off = TRY(t.add_kids(kids));
    return t.add_node(Tree::Node{ Tree::Kind::Seq, false, off, u32(kids.size()), 0, 0 });
}

} // namespace

Result<void> parse(Str line, Tree &out, ParseErr &err)
{
    err = ParseErr();
    out.set_source(line);

    Parser p(line, out, err);
    TRY_VOID(p.advance());

    u32 root = TRY(p.list(0));
    if (p.tok != Tok::End) {
        if (p.tok == Tok::Word)
            return Err(p.fail("syntax error: unexpected keyword"));
        return Err(p.fail(near_of(p.tok)));
    }

    out.set_root(root);
    if (out.freeze().is_err()) {
        err.message = "out of memory";
        return Err(Error::NoMemory);
    }
    return {};
}

void tree_hold(const Tree *t)
{
    if (t)
        t->refs++;
}

void tree_release(const Tree *t)
{
    if (t && --t->refs == 0)
        heap_delete(const_cast<Tree *>(t));
}
