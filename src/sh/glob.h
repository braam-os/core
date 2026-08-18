// Filename generation: the walk over the store that match.h's patterns drive.
// Not pure — it lists directories, which is why it is here and not in
// expand.cpp or match.cpp.
#pragma once

#include "expand.h"
#include "kernel/task.h"
#include "kernel/vec.h"

// Expands every field of `in` holding a live metacharacter, appending what it
// matched to `out` in the store's own order; a field with no metacharacter,
// and one that matches nothing, moves across as it stands. `in` is left empty.
// The only errors are NoMemory and a Cancelled from ^C mid-listing.
Task<Result<void>> glob_fields(Vec<Field> &in, Vec<Field> &out);
