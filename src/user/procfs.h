// ProcFs — the scheduler, the heap, the mount table and the job table as a
// read-only filesystem, mounted on /proc. `cat` and `grep` are then the
// introspection tools, and there is no second interface to keep in step.
//
// The tree is flat: /proc/42 is a file, not a directory. A process here has one
// line of state, so a directory level would hold exactly one file. It lives in
// src/user/ rather than src/fs/ because it reads the scheduler and the job
// table, and braam_fs must not depend upwards.
#pragma once

#include "fs/fs.h"

Fs *procfs_create();
