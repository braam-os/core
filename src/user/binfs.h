// BinFs — the program registry as a read-only filesystem, mounted on /bin.
//
// Programs are in-kernel coroutines until M8 gives them binaries of their own,
// so /bin would otherwise be an empty directory that `ls` could not explain.
// A file here reads as the program's usage line, which is the only thing about
// it there is to read. It lives in src/user/ because the registry does, and
// braam_fs must not depend upwards.
#pragma once

#include "fs/fs.h"

Fs *binfs_create();
