// The prompt loop: what /bin/sh is.
#pragma once

#include "kernel/args.h"
#include "kernel/task.h"
#include "kernel/types.h"

// Where a shell's commands come from. Exactly one, picked out of argv by main.
enum class ShellIn : u8 {
    Console, // a prompt on the keyboard
    Stdin,   // -s, a line at a time
    File,    // sh <file>
    Command, // sh -c <command>
};

// What main hands the shell. Every Str views argv, which outlives the shell.
struct ShellStart {
    ShellIn from = ShellIn::Console;
    Str text;      // the file's path, or the -c command
    Str name0;     // $0
    Args args;     // $1 onwards
    u32 flags = 0; // the SH_* bits -e -x -u asked for
};

// Reads commands and runs them until `exit`, end of input, or a failure it
// cannot come back from. Reports the status the shell leaves with. Only
// ShellIn::Console holds the keyboard and hands out the foreground.
Task<i32> shell(ShellStart start);

// `exit`, which asks rather than acts: a builtin runs inside a line, and the
// loop reads the request when that line is finished.
void shell_exit(i32 status);

bool shell_exit_wanted(i32 &status);
