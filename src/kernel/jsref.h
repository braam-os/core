// A table of externref, and an RAII handle on one slot (Concept.md §3.7).
//
// The table is defined by this module, not imported: import_module/import_name
// apply to functions only. So the host never indexes it. The kernel reserves a
// slot, publishes the number, and the host deposits an object through the
// `ref` export; to use one, the kernel reads it out and passes it as an import
// argument.
#pragma once

#include "types.h"

// An index into the table. Slot 0 always holds null and is never handed out,
// so it doubles as "no object".
using JsSlot = u32;

// 0 when the table cannot grow.
JsSlot jsref_reserve();

void jsref_release(JsSlot slot);

// Null for slot 0, and for a slot the host has not deposited into.
__externref_t jsref_get(JsSlot slot);

void jsref_set(JsSlot slot, __externref_t obj);

// Slots handed out and not yet released, for the leak checks in the tests.
usize jsref_live();

// Owns one slot. Move-only, since two owners would free it twice.
struct JsRef {
    JsRef() = default;

    static JsRef reserve() { return JsRef{ jsref_reserve() }; }

    JsRef(const JsRef &)            = delete;
    JsRef &operator=(const JsRef &) = delete;

    JsRef(JsRef &&o) : slot_(o.slot_) { o.slot_ = 0; }

    JsRef &operator=(JsRef &&o)
    {
        if (this != &o) {
            jsref_release(slot_);
            slot_   = o.slot_;
            o.slot_ = 0;
        }
        return *this;
    }

    ~JsRef() { jsref_release(slot_); }

    bool ok() const { return slot_ != 0; }

    JsSlot slot() const { return slot_; }

    // Gives the slot away, to something that will release it later.
    JsSlot take()
    {
        JsSlot s = slot_;
        slot_    = 0;
        return s;
    }

private:
    explicit JsRef(JsSlot s) : slot_(s) {}

    JsSlot slot_ = 0;
};
