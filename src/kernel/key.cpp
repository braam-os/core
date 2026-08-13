#include "key.h"

#include "traits.h"

namespace {

// Constant-initialised, and trivially destructible, so it needs neither the
// static init nor the __cxa_atexit that --no-entry leaves unavailable.
Channel<Key> g_keys;

static_assert(is_trivially_destructible<Channel<Key>>, "a global must not need atexit");

} // namespace

Channel<Key> &keys()
{
    return g_keys;
}
