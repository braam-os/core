#include "stanza.h"

#include "kernel/fmt.h"
#include "kernel/traits.h"

namespace {

bool next_line(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    usize at = rest.find('\n');
    if (at == Str::npos) {
        line = rest;
        rest = Str();
    } else {
        line = rest.substr(0, at);
        rest = rest.substr(at + 1);
    }
    return true;
}

bool is_upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool is_letter(char c)
{
    return is_upper(c) || (c >= 'a' && c <= 'z');
}

// The first value of `letter`, or empty.
Str field(const Vec<StanzaField> &f, char letter)
{
    for (const StanzaField &x : f)
        if (x.letter == letter)
            return x.value;
    return Str();
}

bool has(const Vec<StanzaField> &f, char letter)
{
    for (const StanzaField &x : f)
        if (x.letter == letter)
            return true;
    return false;
}

// A required number: absent or unparseable is the record's problem.
bool take_number(const Vec<StanzaField> &f, char letter, u64 &out, bool required)
{
    if (!has(f, letter))
        return !required;
    Option<u64> v = stanza_number(field(f, letter));
    if (!v)
        return false;
    out = v.value();
    return true;
}

// The next space-separated word, advancing `rest`.
Str word(Str &rest)
{
    usize at = rest.find(' ');
    Str out  = at == Str::npos ? rest : rest.substr(0, at);
    rest     = at == Str::npos ? Str() : rest.substr(at + 1);
    return out;
}

bool put_field(String &out, char letter, Str value)
{
    return out.push(letter) && out.push(':') && out.append(value) && out.push('\n');
}

bool put_number(String &out, char letter, u64 v)
{
    Buf<24> b;
    b.put(v);
    return put_field(out, letter, b.str());
}

bool put_digest(String &out, char letter, const u8 d[SHA256_SIZE])
{
    char text[DIGEST_TEXT];
    return digest_write(d, Span<char>(text)) && put_field(out, letter, Str(text, sizeof(text)));
}

} // namespace

StanzaRead StanzaReader::next(Vec<StanzaField> &out)
{
    out.clear();
    bool unusable = false;
    bool started  = false;
    Str line;

    while (next_line(rest_, line)) {
        if (line.empty()) {
            if (!started)
                continue; // a run of empty lines is one separator
            return unusable ? StanzaRead::Unusable : StanzaRead::Ok;
        }
        started = true;

        if (line.size() < 2 || !is_letter(line[0]) || line[1] != ':')
            return StanzaRead::Malformed;

        char letter = line[0];
        Str value   = line.substr(2);

        if (known_.find(letter) == Str::npos) {
            if (is_upper(letter))
                unusable = true;
            continue;
        }
        if (STANZA_ACCUMULATING.find(letter) == Str::npos && has(out, letter))
            return StanzaRead::Malformed;
        if (!out.push(StanzaField{ letter, value }))
            return StanzaRead::Malformed;
    }

    if (!started)
        return StanzaRead::End;
    return unusable ? StanzaRead::Unusable : StanzaRead::Ok;
}

bool StanzaReader::one(Str text, Str known, Vec<StanzaField> &out)
{
    StanzaReader r(text, known);
    if (r.next(out) != StanzaRead::Ok)
        return false;
    // The probe reads into a vector of its own: next() clears what it is given.
    Vec<StanzaField> tail;
    return r.next(tail) == StanzaRead::End;
}

Option<u64> stanza_number(Str s)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0'))
        return None;
    u64 v = 0;
    for (usize i = 0; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9')
            return None;
        u64 d = u64(s[i] - '0');
        if (v > (~u64(0) - d) / 10)
            return None;
        v = v * 10 + d;
    }
    return v;
}

bool stanza_component(Str s)
{
    if (s.empty() || s == "." || s == "..")
        return false;
    for (usize i = 0; i < s.size(); i++)
        if (s[i] == '/' || s[i] == '\\' || u8(s[i]) < 0x20)
            return false;
    return true;
}

bool digest_parse(Str s, u8 out[SHA256_SIZE])
{
    if (!s.starts_with("Q2"))
        return false;
    Option<usize> n = base64_decode(s.substr(2), Span<u8>(out, SHA256_SIZE));
    return n && n.value() == SHA256_SIZE;
}

bool digest_write(const u8 d[SHA256_SIZE], Span<char> out)
{
    if (out.size() < DIGEST_TEXT)
        return false;
    out[0] = 'Q';
    out[1] = '2';
    return base64_encode(Bytes(d, SHA256_SIZE), out.subspan(2));
}

bool signed_split(Str text, Str &block, Str &body)
{
    if (text.starts_with("\n")) {
        block = Str();
        body  = text.substr(1);
        return true;
    }
    usize at = text.find("\n\n");
    if (at == Str::npos)
        return false;
    block = text.substr(0, at + 1);
    body  = text.substr(at + 2);
    return true;
}

StanzaRead signature_read(const Vec<StanzaField> &f, Vec<Signature> &out)
{
    for (const StanzaField &x : f) {
        Str rest = x.value;
        Signature s;
        s.algorithm = word(rest);
        s.key_name  = word(rest);
        s.signature = rest;
        if (s.algorithm.empty() || s.key_name.empty() || s.signature.empty())
            return StanzaRead::Unusable;
        if (!out.push(s))
            return StanzaRead::Unusable;
    }
    return StanzaRead::Ok;
}

StanzaRead header_read(const Vec<StanzaField> &f, IndexHeader &out)
{
    IndexHeader h;
    u64 grammar = 0;
    if (!has(f, 'X') || !has(f, 'N') || !has(f, 'G') || !has(f, 'E'))
        return StanzaRead::Unusable;
    if (!take_number(f, 'X', grammar, true) || grammar > ~u32(0))
        return StanzaRead::Unusable;
    if (!take_number(f, 'G', h.version, true) || !take_number(f, 'E', h.expiry, true))
        return StanzaRead::Unusable;

    h.grammar     = u32(grammar);
    h.url         = field(f, 'N');
    h.description = field(f, 'T');
    if (h.url.empty())
        return StanzaRead::Unusable;

    out = h;
    return StanzaRead::Ok;
}

// §3.2 less C and S, which the two readers below come by differently.
static StanzaRead package_rest(const Vec<StanzaField> &f, PackageStanza &p)
{
    u64 priority = 0;
    if (!has(f, 'P') || !has(f, 'V'))
        return StanzaRead::Unusable;
    if (!take_number(f, 'I', p.installed_size, false) ||
        !take_number(f, 't', p.build_time, false) || !take_number(f, 'k', priority, false) ||
        priority > ~u32(0))
        return StanzaRead::Unusable;

    p.name        = field(f, 'P');
    p.version     = field(f, 'V');
    p.description = field(f, 'T');
    p.depends     = field(f, 'D');
    p.provides    = field(f, 'p');
    p.install_if  = field(f, 'i');
    p.origin      = field(f, 'o');
    p.globs       = field(f, 'g');
    p.priority    = u32(priority);

    // §3.2: the store path is built out of these two.
    if (!stanza_component(p.name) || !stanza_component(p.version))
        return StanzaRead::Unusable;
    return StanzaRead::Ok;
}

StanzaRead package_read(const Vec<StanzaField> &f, PackageStanza &out)
{
    PackageStanza p;
    if (!has(f, 'C') || !has(f, 'S'))
        return StanzaRead::Unusable;
    if (!digest_parse(field(f, 'C'), p.digest) || !take_number(f, 'S', p.size, true))
        return StanzaRead::Unusable;
    if (StanzaRead s = package_rest(f, p); s != StanzaRead::Ok)
        return s;

    out = p;
    return StanzaRead::Ok;
}

StanzaRead package_read_info(const Vec<StanzaField> &f, PackageStanza &out)
{
    PackageStanza p;
    if (has(f, 'C') || has(f, 'S'))
        return StanzaRead::Unusable;
    if (StanzaRead s = package_rest(f, p); s != StanzaRead::Ok)
        return s;

    out = p;
    return StanzaRead::Ok;
}

StanzaRead anchor_read(const Vec<StanzaField> &f, Anchor &out)
{
    Anchor a;
    u64 grammar = 0;
    if (!has(f, 'X') || !has(f, 'G') || !has(f, 'E') || !has(f, 'H') || !has(f, 'K'))
        return StanzaRead::Unusable;
    if (!take_number(f, 'X', grammar, true) || grammar > ~u32(0))
        return StanzaRead::Unusable;
    if (!take_number(f, 'G', a.version, true) || !take_number(f, 'E', a.expiry, true))
        return StanzaRead::Unusable;
    a.grammar = u32(grammar);

    for (const StanzaField &x : f) {
        Str rest = x.value;
        if (x.letter == 'H') {
            AnchorThreshold t;
            t.use         = word(rest);
            Option<u64> n = stanza_number(rest);
            if (t.use.empty() || !n || n.value() > ~u32(0))
                return StanzaRead::Unusable;
            t.count = u32(n.value());
            if (!a.thresholds.push(t))
                return StanzaRead::Unusable;
        } else if (x.letter == 'K') {
            AnchorKey k;
            k.use       = word(rest);
            k.algorithm = word(rest);
            k.key       = rest;
            if (k.use.empty() || k.algorithm.empty() || k.key.empty())
                return StanzaRead::Unusable;
            if (!a.keys.push(k))
                return StanzaRead::Unusable;
        }
    }

    out = move(a);
    return StanzaRead::Ok;
}

StanzaRead db_read(const Vec<StanzaField> &f, DbRecord &out)
{
    DbRecord r;
    if (StanzaRead s = package_read(f, r.pkg); s != StanzaRead::Ok)
        return s;
    if (!has(f, 'G') || !take_number(f, 'G', r.index_version, true))
        return StanzaRead::Unusable;
    r.broken = field(f, 'b');

    // F, R and Z are positional: a directory, a name under the last F, and the
    // digest of the file the last R named.
    Str dir;
    bool have_dir = false, have_name = false;
    for (const StanzaField &x : f) {
        if (x.letter == 'F') {
            dir       = x.value;
            have_dir  = true;
            have_name = false;
        } else if (x.letter == 'R') {
            if (!have_dir)
                return StanzaRead::Unusable;
            DbFile file;
            file.dir  = dir;
            file.name = x.value;
            if (file.name.empty() || !r.files.push(file))
                return StanzaRead::Unusable;
            have_name = true;
        } else if (x.letter == 'Z') {
            if (!have_name || !digest_parse(x.value, r.files.back().digest))
                return StanzaRead::Unusable;
            have_name = false;
        }
    }

    out = move(r);
    return StanzaRead::Ok;
}

bool package_write(const PackageStanza &p, String &out)
{
    if (!put_digest(out, 'C', p.digest) || !put_field(out, 'P', p.name) ||
        !put_field(out, 'V', p.version) || !put_number(out, 'S', p.size))
        return false;
    if (p.installed_size && !put_number(out, 'I', p.installed_size))
        return false;
    if (!p.description.empty() && !put_field(out, 'T', p.description))
        return false;
    if (!p.origin.empty() && !put_field(out, 'o', p.origin))
        return false;
    if (p.build_time && !put_number(out, 't', p.build_time))
        return false;
    if (p.priority && !put_number(out, 'k', p.priority))
        return false;
    if (!p.globs.empty() && !put_field(out, 'g', p.globs))
        return false;
    if (!p.depends.empty() && !put_field(out, 'D', p.depends))
        return false;
    if (!p.provides.empty() && !put_field(out, 'p', p.provides))
        return false;
    if (!p.install_if.empty() && !put_field(out, 'i', p.install_if))
        return false;
    return true;
}

bool db_write(const DbRecord &r, String &out)
{
    if (!package_write(r.pkg, out) || !put_number(out, 'G', r.index_version))
        return false;
    if (!r.broken.empty() && !put_field(out, 'b', r.broken))
        return false;

    Str dir;
    bool first = true;
    for (const DbFile &f : r.files) {
        if (first || !(f.dir == dir)) {
            if (!put_field(out, 'F', f.dir))
                return false;
            dir   = f.dir;
            first = false;
        }
        if (!put_field(out, 'R', f.name) || !put_digest(out, 'Z', f.digest))
            return false;
    }
    return true;
}
