// Globbing, `case`, and symbolic links over the real store.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { shows, submit } from "./harness.mjs";

export function check() {
    // Globbing over the real store, and `case` beside it: both go through
    // match.cpp, which is why they land together. The unit suite has the
    // matcher; what it cannot see is a listing reaching a real argv. Late in
    // the file, since each of these spends pids the ps cases above count on.
    submit("mkdir /home/g", 13172);
    submit("mkdir /home/g/sub", 13172.1);
    submit("touch /home/g/aaa /home/g/bb", 13172.2);
    submit("echo x > /home/g/.dot", 13172.3);

    const t = shows(13173, 0.01);

    t.is("echo /bin/l*", "/bin/less /bin/ln /bin/ls");
    t.is("echo /home/g/*", "/home/g/aaa /home/g/bb /home/g/sub");
    t.is("echo /home/g/.*", "/home/g/.dot"); // a leading dot is asked for
    t.is("echo /home/g/?b", "/home/g/bb");
    t.is("echo /home/g/[ab]*", "/home/g/aaa /home/g/bb");
    t.is("echo /home/g/*/", "/home/g/sub/"); // a trailing slash means dirs
    t.is("echo g*", "g"); // relative: the walk lists the cwd
    t.is("echo '/bin/l*'", "/bin/l*"); // quoted: the star is itself
    t.is("echo /bin/nosuch*", "/bin/nosuch*"); // no match: the word as written
    t.is("for f in /home/g/[ab]*; do echo $f; done", "/home/g/aaa|/home/g/bb");
    t.is("v=/home/g/b*; echo $v", "/home/g/bb"); // a star out of a variable

    t.is("case hi in h*) echo yes;; *) echo no;; esac", "yes");
    t.is("case hi in 'h*') echo yes;; *) echo no;; esac", "no"); // quoted pattern
    t.is("case hi in a|hi) echo two;; esac", "two");
    t.is("case hi in a) echo no;; esac", ""); // no arm ran
    t.is("case /home/g/bb in */bb) echo path;; esac", "path");
    submit("rm -r /home/g", t.at());

    // Symbolic links, end to end. The unit suite reaches the VFS; only a real
    // shell reaches `ln`, `ls -l`, `test -h` and a glob.
    {
        const t = shows(13200, 0.01);

        submit("mkdir /home/s", t.at());
        submit("mkdir /home/s/dir", t.at());
        submit("echo hello > /home/s/file", t.at());
        submit("echo deep > /home/s/dir/inner", t.at());

        // Read through, and its own kind in a listing.
        submit("ln -s /home/s/file /home/s/tofile", t.at());
        t.is("cat /home/s/tofile", "hello");
        t.is("ls /home/s", "dir/     file     tofile@");
        t.is("test -h /home/s/tofile && echo yes", "yes");
        t.is("test -f /home/s/tofile && echo yes", "yes"); // -f follows
        t.is("test -h /home/s/file || echo no", "no");
        t.is("test -L /home/s/tofile && echo yes", "yes"); // -L is -h's name

        // -l inside a directory: the name is joined to it to read the target.
        t.is("ls -l /home/s", "total 2|dir   0            - dir/|" +
            "file  6 Jun 19 20:14 file|link 12 Jun 19 20:14 tofile@ -> /home/s/file");

        // An intermediate component: the store answers NotDir, the walk takes
        // over.
        submit("ln -s /home/s/dir /home/s/todir", t.at());
        t.is("cat /home/s/todir/inner", "deep");
        t.is("test -d /home/s/todir && echo yes", "yes");

        // -R descends on directories alone, so dir is entered and todir is not.
        t.is("ls -R /home/s", "/home/s:|dir/     file     todir@   tofile@||/home/s/dir:|inner");

        // A relative target reads against the directory holding the link.
        submit("ln -s file /home/s/rel", t.at());
        t.is("cat /home/s/rel", "hello");

        // A link may cross a mount: every hop goes back through the table.
        submit("ln -s /proc/uptime /home/s/up", t.at());
        t.is("test -f /home/s/up && echo yes", "yes");
        t.is("test -s /home/s/up && echo yes", "yes"); // read through, non-empty
        // ...and the mount it lands in refuses the write, not the one named.
        t.is("echo x > /home/s/up", "/home/s/up: permission denied");

        // The target is printed as written rather than resolved.
        t.is("ls -l /home/s/rel", "link 4 Jun 19 20:14 /home/s/rel@ -> file");
        t.is("ls -l /home/s/todir", "link 11 Jun 19 20:14 /home/s/todir@ -> /home/s/dir");

        // A dangling link is still a link.
        submit("ln -s /home/s/nothing /home/s/dangle", t.at());
        t.is("test -h /home/s/dangle && echo yes", "yes");
        t.is("test -f /home/s/dangle || echo no", "no");

        // A trailing-slash pattern means directories, and a link to one counts.
        t.is("echo /home/s/*/", "/home/s/dir/ /home/s/todir/");

        // Removing a link leaves what it pointed at.
        submit("rm /home/s/tofile", t.at());
        t.is("cat /home/s/file", "hello");

        // No hard links, and `ln` says so rather than making a copy.
        t.is("ln /home/s/file /home/s/hard", "ln: only symbolic links; use -s");
        t.is("test -r /home/s/hard || echo none", "none"); // -r is existence

        // An existing name needs -f, which removes without following.
        t.is("ln -s /home/s/dir /home/s/rel", "ln: /home/s/rel: already exists");
        submit("ln -sf /home/s/dir /home/s/rel", t.at());
        t.is("ls -l /home/s/rel", "link 11 Jun 19 20:14 /home/s/rel@ -> /home/s/dir");
        t.is("cat /home/s/file", "hello"); // the old target is untouched

        // A cycle is bounded rather than walked for ever.
        submit("ln -s /home/s/b /home/s/a", t.at());
        submit("ln -s /home/s/a /home/s/b", t.at());
        t.is("cat /home/s/a", "cat: /home/s/a: too many symbolic links");

        submit("rm -r /home/s", t.at());
    }
}
