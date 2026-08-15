// The shell's grammar: a pipeline of commands, each with redirections.
//
//   pipeline := command ('|' command)* ['&']
//   command  := (word | redirect)+
//   redirect := '<' word | '>' word | '>>' word | '2>' word | '2>>' word
//
// Words are owned here rather than viewed out of the line, because quote
// removal makes a word something the line does not contain. They are appended
// to one growing store first and turned into Str only by freeze(), since the
// store moves under them until it stops growing.
#pragma once

#include "kernel/args.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/vec.h"

enum class Redir : u8 {
    In,        // < f
    Out,       // > f
    Append,    // >> f
    ErrOut,    // 2> f
    ErrAppend, // 2>> f
};

struct Redirect {
    Redir kind;
    usize target; // index into the pipeline's redirection targets
};

struct Command {
    usize argv0 = 0, argc = 0;    // slice of the word table
    usize redir0 = 0, redirn = 0; // slice of the redirection table
};

struct Pipeline {
    // Eight is well past anything typed by hand, and it bounds the pipe and
    // report tables the job runtime sizes off it.
    static constexpr usize MAX_STAGES = 8;

    // ---- before freeze() ----

    Result<void> add_word(Str w);

    Result<void> add_redirect(Redir kind, Str target);

    Result<void> end_command();

    Result<void> freeze();

    // `&`: the shell starts it and does not wait for it.
    void set_background() { background_ = true; }

    // ---- after freeze() ----

    bool frozen() const { return frozen_; }

    bool background() const { return background_; }

    usize size() const { return cmds_.size(); }

    const Command &operator[](usize i) const { return cmds_[i]; }

    Args args(usize i) const;

    Span<const Redirect> redirects(usize i) const;

    Str target(const Redirect &r) const;

private:
    struct Slice {
        usize off, len;
    };

    Result<void> keep(Str s, Vec<Slice> &into);

    String store_;         // every word's bytes, back to back
    Vec<Slice> words_;     // argv words, contiguous per command
    Vec<Slice> targets_;   // redirection targets
    Vec<Redirect> redirs_; // contiguous per command
    Vec<Command> cmds_;
    Vec<Str> word_view_;   // built by freeze() over the final store_
    Vec<Str> target_view_; //
    usize argv0_     = 0;  // where the command being built starts
    usize redir0_    = 0;
    bool frozen_     = false;
    bool background_ = false;
};

// Parses one line. An empty line yields a frozen pipeline with no commands.
// On failure `message` names the problem, ready to print after "braam: ".
Result<void> parse(Str line, Pipeline &out, Str &message);
