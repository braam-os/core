#include "parse.h"

#include "kernel/host.h"
#include "tokenize.h"

Result<void> Pipeline::keep(Str s, Vec<Slice> &into)
{
    if (frozen_)
        panic("Pipeline: a word added after freeze");
    usize off = store_.size();
    if (!store_.append(s))
        return Err(Error::NoMemory);
    if (!into.push(Slice{ off, s.size() })) {
        store_.truncate(off);
        return Err(Error::NoMemory);
    }
    return {};
}

Result<void> Pipeline::add_word(Str w)
{
    return keep(w, words_);
}

Result<void> Pipeline::add_redirect(Redir kind, Str target)
{
    TRY_VOID(keep(target, targets_));
    if (!redirs_.push(Redirect{ kind, targets_.size() - 1 }))
        return Err(Error::NoMemory);
    return {};
}

Result<void> Pipeline::end_command()
{
    if (cmds_.size() >= MAX_STAGES)
        return Err(Error::Invalid);

    Command c;
    c.argv0  = argv0_;
    c.argc   = words_.size() - argv0_;
    c.redir0 = redir0_;
    c.redirn = redirs_.size() - redir0_;
    if (!cmds_.push(c))
        return Err(Error::NoMemory);

    argv0_  = words_.size();
    redir0_ = redirs_.size();
    return {};
}

// The Str tables are built only here, because store_ reallocates while it
// grows and every view into it would move. Moving a frozen Pipeline is still
// safe: String and Vec move by stealing the pointer, so nothing shifts.
Result<void> Pipeline::freeze()
{
    if (frozen_)
        return {};
    if (!word_view_.reserve(words_.size()) || !target_view_.reserve(targets_.size()))
        return Err(Error::NoMemory);
    for (const Slice &s : words_)
        word_view_.push(Str(store_.data() + s.off, s.len));
    for (const Slice &s : targets_)
        target_view_.push(Str(store_.data() + s.off, s.len));
    frozen_ = true;
    return {};
}

Args Pipeline::args(usize i) const
{
    if (!frozen_)
        panic("Pipeline: args before freeze");
    const Command &c = cmds_[i];
    return Args{ Span<const Str>(word_view_.data() + c.argv0, c.argc) };
}

Span<const Redirect> Pipeline::redirects(usize i) const
{
    const Command &c = cmds_[i];
    return Span<const Redirect>(redirs_.data() + c.redir0, c.redirn);
}

Str Pipeline::target(const Redirect &r) const
{
    if (!frozen_)
        panic("Pipeline: target before freeze");
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
    default:
        return None;
    }
}

} // namespace

Result<void> parse(Str line, Pipeline &out, Str &message)
{
    Lexer lx(line);
    String w;
    bool any_word  = false; // the command being built has an argv
    bool any_token = false;

    for (;;) {
        Result<Tok> t = lx.next(w);
        if (t.is_err()) {
            message = t.error() == Error::Invalid ? "unterminated quote" : "out of memory";
            return Err(t.error());
        }

        Tok tok = t.value();
        if (tok == Tok::End)
            break;
        any_token = true;

        if (tok == Tok::Word) {
            if (out.add_word(w.str()).is_err()) {
                message = "out of memory";
                return Err(Error::NoMemory);
            }
            any_word = true;
            continue;
        }

        // `&` ends the line, and there is no list grammar for it to separate:
        // `a & b` would need one, and M7 does not need `a & b`.
        if (tok == Tok::Amp) {
            if (!any_word) {
                message = "syntax error near '&'";
                return Err(Error::Invalid);
            }
            Result<Tok> after = lx.next(w);
            if (after.is_err() || after.value() != Tok::End) {
                message = "syntax error: '&' must end the line";
                return Err(Error::Invalid);
            }
            out.set_background();
            break;
        }

        if (tok == Tok::Pipe) {
            if (!any_word) {
                message = "syntax error near '|'";
                return Err(Error::Invalid);
            }
            if (out.end_command().is_err()) {
                message = "too many commands in a pipeline";
                return Err(Error::Invalid);
            }
            any_word = false;
            continue;
        }

        Redir kind     = redir_of(tok).value();
        Result<Tok> t2 = lx.next(w);
        if (t2.is_err() || t2.value() != Tok::Word) {
            message = "syntax error: expected a file name";
            return Err(Error::Invalid);
        }
        if (out.add_redirect(kind, w.str()).is_err()) {
            message = "out of memory";
            return Err(Error::NoMemory);
        }
    }

    if (!any_word) {
        if (!any_token) {
            if (out.freeze().is_err()) {
                message = "out of memory";
                return Err(Error::NoMemory);
            }
            return {};
        }
        message = "syntax error: expected a command";
        return Err(Error::Invalid);
    }

    if (out.end_command().is_err()) {
        message = "too many commands in a pipeline";
        return Err(Error::Invalid);
    }
    if (out.freeze().is_err()) {
        message = "out of memory";
        return Err(Error::NoMemory);
    }
    return {};
}
