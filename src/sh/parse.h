// The shell's grammar: a line is a tree of pipelines.
//
//   line     := list
//   list     := and_or (sep and_or)* [sep]
//   sep      := ';' | '&' | newline
//   and_or   := pipeline (('&&' | '||') pipeline)*
//   pipeline := ['!'] (funcdef | compound | simple ('|' simple)*)
//   funcdef  := name '(' ')' newline* compound
//   compound := group | if | loop | for | case
//   group    := '{' list '}'
//   if       := 'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
//   loop     := ('while' | 'until') list 'do' list 'done'
//   for      := 'for' name ['in' word*] sep 'do' list 'done'
//   case     := 'case' word 'in' arm* 'esac'
//   arm      := ['('] word ('|' word)* ')' list [';;']
//   simple   := assign* (word | redirect)+ | assign+ redirect*
//   assign   := name '=' word, and only ahead of the first ordinary word
//   redirect := '<' word | '>' word | '>>' word | '2>' word | '2>>' word
//
// Words arrive raw — quotes and backslashes still in them, since expand.h
// takes those off later — but are owned here, because a parse outlives the
// text it came from. They turn into Str only in freeze(), since the store
// moves under them until it stops growing. The nodes are index links for the
// same reason: every vector reallocates as the parse grows.
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
    usize target; // index into the tree's redirection targets
};

// Why a parse failed. `more` means the text ended *inside* something, so a
// prompt asks for another line rather than complaining. Not an Error value:
// that enum is kernel-wide and this is the shell's alone.
struct ParseErr {
    Str message;
    bool more = false;
};

struct Command {
    usize argv0 = 0, argc = 0;    // slice of the word table
    usize redir0 = 0, redirn = 0; // slice of the redirection table
    usize assignn = 0;            // leading words of the slice that are name=value
};

struct Tree {
    // Eight is well past anything typed by hand, and it bounds the pipe and
    // report tables the job runtime sizes off it. Per pipeline, not per line.
    static constexpr usize MAX_STAGES = 8;

    // How deep one text may nest. This bounds the parser, which is on the
    // wasm stack; job.cpp bounds the walk with a counter of its own.
    static constexpr u32 MAX_NEST = 16;

    enum class Kind : u8 {
        Nop,     // node 0, so an index of 0 reads as "nothing"
        Pipe,    // a..b commands, c..d the bytes of the line they came from
        Seq,     // a, b: an offset and a count into the child list
        AndOr,   // the same, plus c: an offset into the operator list
        Not,     // ! a
        Group,   // { a }
        If,      // a, b: b (condition, body) pairs in the child list; c the else
        While,   // a the condition, b the body
        Until,   // the same, run while the condition fails
        For,     // a the body, b the command holding the name then the words,
                 // c set when there was an `in`
        Case,    // a, b: b (patterns, body) pairs in the child list, where a
                 // pair is the command holding that arm's patterns then the
                 // body node; c the command holding the word being matched
        FuncDef, // a the body node, b the command holding the name
    };

    enum class Op : u8 { And, Or };

    // Fixed size, no owning members, index links only.
    struct Node {
        Kind kind       = Kind::Nop;
        bool background = false;
        u32 a = 0, b = 0, c = 0, d = 0;
    };

    // ---- before freeze() ----

    // `assignable` off for a `for` loop's name and words, which are not a
    // command and where a leading `x=1` is an ordinary word.
    Result<void> add_word(Str w, bool assignable = true);

    Result<void> add_redirect(Redir kind, Str target);

    Result<void> end_command();

    // The index of the node added, which is what links it to its parent.
    Result<u32> add_node(const Node &n);

    // Appended whole: a nested list is parsed while this one is still
    // collecting, so pushing as we go would interleave the two.
    Result<u32> add_kids(Span<const u32> kids);

    Result<u32> add_ops(Span<const u8> ops);

    void set_root(u32 n) { root_ = n; }

    // What text() views. Not copied, so a Tree outliving its text has none.
    void set_source(Str s) { line_ = s; }

    // Copies what set_source only viewed, for a tree that outlives its line.
    Result<void> own_source();

    Result<void> freeze();

    // ---- after freeze() ----

    bool frozen() const { return frozen_; }

    // Whether any FuncDef was added.
    bool defines() const { return defines_; }

    // How many function entries hold it. A field and two free functions, not a
    // destructor: one would delete the implicit move test_parse.cpp needs.
    // Mutable, so the walk can hold what it was handed as const.
    mutable u32 refs = 0;

    u32 root() const { return root_; }

    const Node &node(u32 i) const { return nodes_[i]; }

    void set_background(u32 i) { nodes_[i].background = true; }

    u32 kid(u32 i) const { return kids_[i]; }

    Op op(u32 i) const { return Op(ops_[i]); }

    // The bytes a node came from, which is what `jobs` lists. Empty for a
    // node that spans none.
    Str text(u32 i) const;

    // Commands, across the whole line: a Pipe node names its own slice.
    usize size() const { return cmds_.size(); }

    const Command &operator[](usize i) const { return cmds_[i]; }

    // The command proper, with its assignment prefix taken off, and that
    // prefix on its own. Either may be empty: `x=1` is a command with no argv
    // and `ls` one with no assignments.
    Args args(usize i) const;

    Args assigns(usize i) const;

    Span<const Redirect> redirects(usize i) const;

    Str target(const Redirect &r) const;

private:
    struct Slice {
        usize off, len;
    };

    Result<void> keep(Str s, Vec<Slice> &into);

    Str line_;             // what was parsed, for text() to view
    String source_;        // a copy of it, once own_source() has been called
    String store_;         // every word's bytes, back to back
    Vec<Slice> words_;     // argv words, contiguous per command
    Vec<Slice> targets_;   // redirection targets
    Vec<Redirect> redirs_; // contiguous per command
    Vec<Command> cmds_;
    Vec<Node> nodes_;      // nodes_[0] is always Nop
    Vec<u32> kids_;        // child lists, so a Node stays fixed-size
    Vec<u8> ops_;          // one per link of an AndOr chain
    Vec<Str> word_view_;   // built by freeze() over the final store_
    Vec<Str> target_view_; //
    u32 root_       = 0;
    usize argv0_    = 0; // where the command being built starts
    usize redir0_   = 0;
    usize assign_n_ = 0; // how many of its words so far are assignments
    bool frozen_    = false;
    bool defines_   = false;
};

// A tree a function body pins outlives the line it was parsed from.
void tree_hold(const Tree *t);
void tree_release(const Tree *t);

// Parses one text, which may be several lines. An empty one yields a frozen
// tree whose root is 0. On failure `err.message` names the problem, ready to
// print after "braam: ".
Result<void> parse(Str line, Tree &out, ParseErr &err);
