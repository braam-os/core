// Stdio backed directly by the cell grid. M4 replaces this with a pair of
// Channel<Bytes> and moves the screen behind the terminal's reader.
#pragma once

#include "prog.h"

// out and err are the same sink: the split is meaningless until there is
// redirection to tell them apart.
Stdio stdio_console();
