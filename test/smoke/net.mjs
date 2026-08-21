// curl, a chat over a WebSocket, the file picker, the clipboard and the wall clock.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { E } from "../../web/abi.js";
import { pasted } from "../../web/keys.js";
import {
    CTRL, KEY, fail, net, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M6, first criterion: curl fetches a URL and prints the body. -i puts the
    // status and the headers in front of it.
    s = submit("clear", 3020);
    s = submit("curl /hello.txt", 3021);
    if (!rows(s).includes("hi there"))
        fail(`curl printed nothing: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3022);
    s = submit("curl -i /hello.txt", 3023);
    if (!rows(s).includes("HTTP 200"))
        fail(`curl -i did not print the status: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith("content-type: text/plain")))
        fail(`curl -i did not print the headers: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3024);
    s = submit("curl /nosuch", 3025);
    if (!rows(s).some((line) => line.startsWith("curl: /nosuch: not found")))
        fail(`a failed fetch said nothing: ${JSON.stringify(rows(s))}`);

    // A fetched body reaches a pipe like any other output.
    s = submit("clear", 3026);
    s = submit("curl /hello.txt | wc", 3027);
    if (!rows(s).some((line) => line.startsWith("1 2 9")))
        fail(`curl into a pipe printed ${JSON.stringify(rows(s))}, expected 1 2 9`);

    // The two ways a fetch fails in a browser, which look identical to fetch
    // itself: an origin the server will not allow, and nothing answering at
    // all. Each gets the hint that fits, and neither gets the other's.
    net.routes.set("https://denied.example", { fail: E.PERM });
    net.routes.set("https://dead.example", { fail: E.IO });

    s = submit("clear", 3028);
    s = submit("curl https://denied.example", 3029);
    if (!rows(s).some((line) => line.includes("cross-origin")))
        fail(`a refused origin said nothing about CORS: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3031);
    s = submit("curl https://dead.example", 3033);
    if (!rows(s).some((line) => line.startsWith("curl: no answer")))
        fail(`a dead network said nothing: ${JSON.stringify(rows(s))}`);
    if (rows(s).some((line) => line.includes("cross-origin")))
        fail(`a dead network was blamed on CORS: ${JSON.stringify(rows(s))}`);

    // M6, second criterion: a chat client over a WebSocket. The fake loops a
    // lone socket back to itself, so one client is a whole conversation; the
    // receiver is a job of its own, which is what makes the reply arrive while
    // the program is parked on the keyboard.
    s = submit("clear", 3030);
    // What arrives is written to stdout by the receiver task, so it redirects
    // like anything else — which it could not when the receiver was a job
    // outside the program, writing to the screen because the pipe it would
    // have used might already have been freed. First, while this is the only
    // socket: the fake loops a message back to a sender with no peer.
    type("chat ws://loop me > /home/chat.log");
    press(KEY.ENTER);
    run(3030.1);
    s = submit("hi there", 3030.2);
    press("c".codePointAt(0), CTRL);
    run(3030.3);
    s = submit("clear", 3030.4);
    s = submit("cat /home/chat.log", 3030.5);
    if (!rows(s).includes("me: hi there"))
        fail(`chat > log captured ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3030.6);
    type("chat ws://loop me");
    press(KEY.ENTER);
    run(3031);
    if (net.sockets.length !== 2)
        fail(`chat opened ${net.sockets.length} sockets, expected 2`);
    s = submit("hello", 3032);
    if (!rows(s).includes("me: hello"))
        fail(`the chat message did not come back: ${JSON.stringify(rows(s))}`);

    // ^C leaves the socket dropped and the prompt back. The receiver is a
    // second task inside the process now, so it goes when the instance does.
    press("c".codePointAt(0), CTRL);
    if (run(3033) !== -1)
        fail("^C left the chat receiver scheduled");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on chat left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (!net.sockets[1].closed)
        fail("chat did not drop its socket on the way out");

    // A descriptor closed under a parked read, which is the one sequence a
    // program can arrange: chat's receiver is parked on the socket while the
    // root task reads what is typed, and end of input closes that socket. The
    // close is served while the read is parked, because the root task waits for
    // its reply. What comes back is chat's own status, not a crash.
    s = submit("clear", 3034);
    type("chat ws://loop me");
    press(KEY.ENTER);
    run(3035);
    if (net.sockets.length !== 3)
        fail(`chat opened ${net.sockets.length} sockets, expected 3`);
    s = submit("still here", 3036); // the receiver parks again after this
    press("d".codePointAt(0), CTRL);
    if (run(3037) !== -1)
        fail("end of input left the chat receiver scheduled");
    s = screen();
    if (!net.sockets[2].closed)
        fail("chat did not close its socket at end of input");
    if (row(s, s.cursor_y) !== prompt())
        fail(`^D on chat left ${JSON.stringify(rows(s))}, expected a bare prompt`);

    // M6, third criterion: /import takes what the picker hands over, and
    // fexport sends a file back out through the browser.
    net.reset();
    s = submit("clear", 3040);
    s = submit("fimport", 3041);
    if (!rows(s).includes("/import/notes.txt"))
        fail(`fimport named nothing: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3042);
    s = submit("cat /import/notes.txt", 3043);
    if (!rows(s).includes("picked"))
        fail(`the imported file did not read back: ${JSON.stringify(rows(s))}`);

    submit("fexport /import/notes.txt", 3044);
    if (net.exported.length !== 1 || net.exported[0].name !== "notes.txt")
        fail(`fexport sent ${JSON.stringify(net.exported.map((f) => f.name))}`);
    if (new TextDecoder().decode(net.exported[0].bytes) !== "picked\n")
        fail(`fexport sent the wrong bytes: ${JSON.stringify(net.exported[0].bytes)}`);

    // The clipboard, and the wall clock the kernel's monotonic one cannot give.
    submit("echo copied | pbcopy", 3050);
    if (net.clipboard !== "copied\n")
        fail(`pbcopy left ${JSON.stringify(net.clipboard)} on the clipboard`);
    s = submit("clear", 3051);
    s = submit("pbpaste", 3052);
    if (!rows(s).includes("copied"))
        fail(`pbpaste printed nothing: ${JSON.stringify(rows(s))}`);

    // A browser that will not read the clipboard outside a user gesture — which
    // a command can never be. pbpaste asks for the one gesture that needs no
    // permission and parks until it happens.
    net.clipDenied = true;
    s = submit("clear", 3055);
    type("pbpaste");
    press(KEY.ENTER);
    if (run(3056) !== -1)
        fail("pbpaste did not park on the paste");
    s = screen();
    if (!rows(s).some((line) => line.startsWith("pbpaste: press ")))
        fail(`pbpaste did not ask for a gesture: ${JSON.stringify(rows(s))}`);
    if (!net.paste("pasted by hand"))
        fail("pbpaste was not waiting for a paste");
    run(3057);
    s = screen();
    if (!rows(s).includes("pasted by hand"))
        fail(`the paste never arrived: ${JSON.stringify(rows(s))}`);

    // ^C while it waits gets the prompt back, and leaves the arming behind it
    // to be reaped when the paste finally lands.
    type("pbpaste");
    press(KEY.ENTER);
    run(3058);
    press("c".codePointAt(0), CTRL);
    if (run(3059) !== -1)
        fail("^C left the paste wait scheduled");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on pbpaste left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    net.paste("too late");
    run(3060);
    net.clipDenied = false;

    s = submit("clear", 3053);
    s = submit("date -u", 3054);
    if (!rows(s).some((line) => /^\w\w\w \w\w\w \d\d \d\d:\d\d:\d\d \+0000 \d{4}$/.test(line)))
        fail(`date printed ${JSON.stringify(rows(s))}`);
}
