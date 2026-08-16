// The embedding API: everything a host page needs to put a braam terminal on
// itself, and nothing about how braam works.
//
//   import { mount } from "./braam.js";
//   const term = mount({ canvas: document.getElementById("screen") });
//
// One instance is one worker, which is the isolation M8 builds on rather than
// an accident, so mounting twice on a page gives two independent kernels that
// share nothing but the origin's storage.
//
// Everything that used to be inline in index.html lives here: the worker, the
// OffscreenCanvas transfer, the resize and device-pixel-ratio watches, the
// keyboard, and the three services a worker cannot perform for itself.

import { E } from "./abi.js";
import { consumes, normalise, pasted } from "./keys.js";

// Persistence is granted to the origin, not to a terminal, so the request is
// made once for the page however many are mounted. Asking twice is not merely
// redundant: a second call while the first is outstanding may not settle until
// the first one has, and a worker whose boot waits on the answer waits with it.
let persistence = null;

function persistOnce(wanted) {
    if (!wanted)
        return Promise.resolve(false);
    if (!persistence) {
        persistence = (async () => {
            try {
                if (navigator.storage && navigator.storage.persist)
                    return await navigator.storage.persist();
            } catch {
                // A refusal is an answer; `df` reports best-effort mode.
            }
            return false;
        })();
    }
    return persistence;
}

// How long boot may wait for that answer. M6 blocked on it outright, on the
// reasoning that `df` reporting the wrong durability is worse than a tick of
// delay — and it is, but the call took over five seconds in headless Firefox,
// which is a blank screen rather than a tick. So a provisional "best effort"
// goes down after this, and the real answer follows and corrects it.
const PERSIST_GRACE_MS = 250;

// A canvas hands its pixels over exactly once, so a terminal that has been
// disposed cannot be mounted again on the same element — make a new one.
export function mount(options = {}) {
    const canvas = options.canvas;
    if (!canvas)
        throw new Error("braam: mount needs a canvas");
    if (!canvas.transferControlToOffscreen)
        throw new Error("braam: this browser has no OffscreenCanvas");

    const onLog = options.onLog || ((text) => console.log(text));
    const onError = options.onError || ((text) => console.error(text));

    const worker = new Worker(new URL(options.workerUrl || "./worker.js", import.meta.url),
                              { type: "module" });

    // First of all: the worker fetches nothing until it knows what to fetch.
    worker.postMessage({
        kind: "options",
        options: {
            wasmUrl: options.wasmUrl,
            bundleUrl: options.bundleUrl,
            procWorkerUrl: options.procWorkerUrl,
            palette: options.palette,
            fontFamily: options.fontFamily,
            fontSize: options.fontSize,
        },
    });

    // navigator.storage.persist() exists only on the main thread (Concept.md
    // §A.2), and asking for it is what keeps files from being evicted without
    // warning. Boot waits for it, so it is answered twice when it is slow: once
    // provisionally, once for real.
    let answered = false;
    const grace = setTimeout(() => {
        if (!answered)
            worker.postMessage({ kind: "persisted", value: false });
    }, PERSIST_GRACE_MS);

    persistOnce(options.persist !== false).then((granted) => {
        answered = true;
        clearTimeout(grace);
        worker.postMessage({ kind: "persisted", value: granted });
    });

    const offscreen = canvas.transferControlToOffscreen();
    worker.postMessage({ kind: "canvas", canvas: offscreen }, [offscreen]);

    // The page owns the pixel box; the worker owns the font and therefore the
    // geometry. devicePixelContentBoxSize is the exact device-pixel box, and is
    // already right under fractional zoom.
    function viewport(entry) {
        const dpr = window.devicePixelRatio || 1;
        let width, height;
        if (entry && entry.devicePixelContentBoxSize && entry.devicePixelContentBoxSize.length) {
            const box = entry.devicePixelContentBoxSize[0];
            width = box.inlineSize;
            height = box.blockSize;
        } else {
            const rect = canvas.getBoundingClientRect();
            width = Math.round(rect.width * dpr);
            height = Math.round(rect.height * dpr);
        }
        worker.postMessage({ kind: "viewport", width, height, dpr });
    }

    const observer = new ResizeObserver((entries) => viewport(entries[0]));
    observer.observe(canvas);

    // devicePixelRatio changes when the window moves to another monitor or the
    // browser zooms; the query has to be re-armed at the new ratio each time.
    let live = true;

    function watchRatio() {
        matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener(
            "change",
            () => {
                if (!live)
                    return;
                viewport(null);
                watchRatio();
            },
            { once: true });
    }
    watchRatio();

    // The mouse selects, and nothing about it reaches the kernel: the page
    // names cells in device pixels, the renderer highlights them and reads them
    // back as text (Concept.md §3.5). It costs no keystroke and no syscall.
    let selection = "";
    let dragging = null; // the pointer id of the drag in progress

    function device(event) {
        const rect = canvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        return {
            x: Math.round((event.clientX - rect.left) * dpr),
            y: Math.round((event.clientY - rect.top) * dpr),
        };
    }

    function drag(phase, event) {
        const { x, y } = device(event);
        worker.postMessage({ kind: "select", phase, x, y });
    }

    function onPointerDown(event) {
        if (event.button !== 0)
            return;
        // A click has to focus, or the copy chord below would go to whatever
        // the page focused last.
        canvas.focus();
        dragging = event.pointerId;
        canvas.setPointerCapture(event.pointerId);
        drag("start", event);
    }

    function onPointerMove(event) {
        if (dragging === event.pointerId)
            drag("move", event);
    }

    function onPointerUp(event) {
        if (dragging !== event.pointerId)
            return;
        dragging = null;
        drag("end", event);
    }

    canvas.addEventListener("pointerdown", onPointerDown);
    canvas.addEventListener("pointermove", onPointerMove);
    canvas.addEventListener("pointerup", onPointerUp);
    canvas.addEventListener("pointercancel", onPointerUp);

    function copy() {
        const text = selection;
        selection = ""; // ^C interrupts again from here, without waiting for a reply
        worker.postMessage({ kind: "deselect" });
        if (!navigator.clipboard) {
            onError("braam: this origin has no clipboard");
            return;
        }
        // Inside the keydown on purpose: the keystroke is the transient
        // activation that permits the write (Concept.md §A.2). The page's own
        // copy of the text is what makes that possible — asking the worker for
        // it would answer a turn too late.
        navigator.clipboard.writeText(text)
            .catch((e) => onError(`braam: copy refused: ${e.message}`));
    }

    const letter = (event, c) =>
        !event.altKey && (event.key === c || event.key === c.toUpperCase());

    // Scoped to the canvas rather than the window: an embedded terminal shares
    // its page, and two of them must not both read the same keystroke.
    function onKeyDown(event) {
        // Ctrl+C — Cmd+C on a Mac — copies when there is a selection, and is
        // ^C when there is not. A terminal with no second copy key has to
        // overload it, and copying clears the selection, so the next one
        // interrupts. Every other key just drops the selection, in the worker.
        if (selection && (event.ctrlKey || event.metaKey) && letter(event, "c")) {
            event.preventDefault(); // or the browser copies its empty selection over ours
            copy();
            return;
        }

        // Select all is the platform's chord and not Ctrl+A, which is the line
        // editor's beginning-of-line and cannot be overloaded: there is no
        // "there is a selection" to tell the two apart the way there is for
        // copy. A browser that claims Ctrl+Shift+A for itself keeps it.
        if ((event.metaKey || (event.ctrlKey && event.shiftKey)) && letter(event, "a")) {
            event.preventDefault();
            worker.postMessage({ kind: "selectall" });
            return;
        }

        const key = normalise(event);
        if (consumes(event, key))
            event.preventDefault();
        if (key)
            worker.postMessage({ kind: "key", code: key.code, mods: key.mods });
    }
    canvas.addEventListener("keydown", onKeyDown);
    if (canvas.tabIndex < 0)
        canvas.tabIndex = 0;

    // The three host services a worker cannot perform itself: navigator.
    // clipboard, <input type="file"> and a download all need the DOM
    // (Concept.md §5.4). The worker asks by id and the answer goes straight
    // back.
    let picker = options.picker || null;
    let ownPicker = false;

    function filePicker() {
        if (!picker) {
            picker = document.createElement("input");
            picker.type = "file";
            picker.multiple = true;
            picker.hidden = true;
            document.body.appendChild(picker);
            ownPicker = true;
        }
        return picker;
    }

    function pick() {
        const input = filePicker();
        return new Promise((resolve) => {
            const done = async () => {
                const files = [...input.files];
                input.value = "";
                resolve(await Promise.all(files.map(async (f) => ({
                    name: f.name,
                    bytes: new Uint8Array(await f.arrayBuffer()),
                }))));
            };
            input.addEventListener("change", done, { once: true });
            input.addEventListener("cancel", () => {
                input.removeEventListener("change", done);
                resolve([]);
            }, { once: true });
            // The keystroke that ran the command is still the current
            // activation, which is what lets this open without a click of its
            // own.
            input.click();
        });
    }

    function save(name, bytes) {
        const url = URL.createObjectURL(new Blob([bytes]));
        const a = document.createElement("a");
        a.href = url;
        a.download = name;
        a.click();
        setTimeout(() => URL.revokeObjectURL(url), 10000);
    }

    // Reading the clipboard is only allowed from inside a user-gesture handler,
    // and a command reaches this page long after its keystroke returned — so
    // the async API is refused by design in some browsers. A paste is itself
    // the gesture, and needs no permission at all, so a refused read waits for
    // one.
    let pasteWaiter = null;

    // Cmd+V, or Ctrl+V where that is the chord: the browser hands the text over
    // and the terminal types it. A `pbpaste` that is waiting takes it instead —
    // it asked for exactly this gesture, and a program reading the clipboard
    // wants the text rather than the keystrokes.
    //
    // The paste event is the document's, not the canvas's, so an embedded
    // terminal only claims one when it holds the focus; otherwise the paste
    // belongs to whatever field the page focused.
    function onPaste(event) {
        if (document.activeElement !== canvas)
            return;
        const text = event.clipboardData ? event.clipboardData.getData("text") : "";
        if (pasteWaiter) {
            const resolve = pasteWaiter;
            pasteWaiter = null;
            event.preventDefault();
            resolve(text);
            return;
        }
        if (!text)
            return;
        event.preventDefault();
        worker.postMessage({ kind: "paste", codes: pasted(text) });
    }
    addEventListener("paste", onPaste);

    function awaitPaste() {
        return new Promise((resolve) => {
            if (pasteWaiter)
                pasteWaiter(""); // a superseded wait answers empty, not never
            pasteWaiter = resolve;
        });
    }

    async function service(data) {
        switch (data.svc) {
        case "clipRead":
            return await navigator.clipboard.readText();
        case "clipWait":
            return await awaitPaste();
        case "clipWrite":
            await navigator.clipboard.writeText(data.text);
            return null;
        case "pick":
            return await pick();
        case "save":
            save(data.name, data.bytes);
            return null;
        }
        throw new Error(`unknown service ${data.svc}`);
    }

    worker.onmessage = ({ data }) => {
        if (data.kind === "selection") {
            selection = data.text;
            return;
        }
        if (data.kind === "svc") {
            service(data)
                .then((value) => worker.postMessage({ kind: "svc-reply", id: data.id, value }))
                .catch((e) => worker.postMessage({
                    kind: "svc-reply",
                    id: data.id,
                    error: (e && (e.name === "NotAllowedError" || e.name === "SecurityError"))
                        ? E.PERM : E.IO,
                }));
            return;
        }
        if (data.kind === "stats")
            return; // whoever asked is listening for itself
        if (data.kind === "error")
            onError(data.text);
        else
            onLog(data.text);
    };

    worker.onerror = (e) => onError(`worker error: ${e.message}`);

    return {
        canvas,
        worker,

        focus() {
            canvas.focus();
        },

        // What the mouse has marked, which is what the copy chord writes.
        selection() {
            return selection;
        },

        // Everything this page attached, in one place: a host that swaps views
        // must be able to let go of a terminal completely.
        dispose() {
            live = false;
            observer.disconnect();
            canvas.removeEventListener("keydown", onKeyDown);
            canvas.removeEventListener("pointerdown", onPointerDown);
            canvas.removeEventListener("pointermove", onPointerMove);
            canvas.removeEventListener("pointerup", onPointerUp);
            canvas.removeEventListener("pointercancel", onPointerUp);
            removeEventListener("paste", onPaste);
            if (ownPicker && picker.parentNode)
                picker.parentNode.removeChild(picker);
            worker.onmessage = null;
            worker.onerror = null;

            // Asked first, told after. A process is a worker of the
            // kernel's worker, and terminating a parent is specified to take
            // its children with it — but a leaked one is a core spinning for
            // the life of the page, which is too much to leave to a spec
            // nobody here can check. The timeout is the backstop for a kernel
            // worker too wedged to read the message.
            worker.postMessage({ kind: "dispose" });
            setTimeout(() => worker.terminate(), 0);
        },
    };
}
