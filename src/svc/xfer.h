// Moving files in and out of the tab (Concept.md §5.4). The picker and the
// download are the escape hatch that works in every browser, unlike
// showDirectoryPicker; both need the DOM, so the host relays them to the page.
#pragma once

#include "svc.h"

// The files the user chose, held on the host side until this goes away.
struct Picked {
    JsHandle set;
    usize count = 0;
};

// Shows the picker. An empty set means the user cancelled.
Task<Result<Picked>> pick_files();

Task<Result<String>> pick_name(const Picked &p, usize index);

// A chunk of one file, empty at its end.
Task<Result<String>> pick_read(const Picked &p, usize index, u64 off);

// Hands the bytes to the browser as a download.
Task<Result<void>> fexport_file(Str name, Str bytes);
