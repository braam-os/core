// The half of the zip reader that needs a syscall, kept out of zip.cpp so that
// one compiles into tests.wasm.
#pragma once

#include "kernel/result.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "zip.h"

// An entry's bytes: stored ones straight out of the archive, deflated ones
// through Sys::Inflate, stopping at the declared size. An error from the
// operation or from any later read is fatal, and neither is a short read.
Task<Result<String>> zip_read(const ZipEntry &e);
