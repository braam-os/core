// Stdio backed directly by the cell grid. The screen is not behind a byte
// channel: §2.3 says the terminal *is* a cell grid, so a queue in front of it
// would add a copy and a scheduling hop to reach an array screen_write already
// fills, and would put terminal output behind something that can drop.
#pragma once

#include "prog.h"

// out and err are the same sink, and stdin is empty. A pipeline replaces the
// ends it redirects; the rest stay pointed here.
Stdio stdio_console();
