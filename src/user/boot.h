// Bringing the filesystem up. The shell awaits this before its first prompt,
// because everything it can be asked to do needs the mounts in place.
#pragma once

#include "kernel/task.h"

Task<void> boot_filesystem();
