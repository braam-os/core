#include "opt.h"

Result<bool> OptParse::next(Opt &out)
{
    if (at_ >= args_.size())
        return false;

    Str w = args_[at_];
    if (in_ == 0) {
        // Anything that is not a flag ends the options, `-` alone included.
        if (w.size() < 2 || w[0] != '-')
            return false;
        if (w == "--") {
            at_++;
            return false;
        }
        in_ = 1;
    }

    char c    = w[in_++];
    out       = Opt{ c, Str() };
    bool last = in_ >= w.size();

    // A valued letter takes the rest of its word, or the next word. Either way
    // it ends the bundle.
    if (spec_.valued.find(c) != Str::npos) {
        if (!last) {
            out.value = w.substr(in_);
            at_++;
        } else if (at_ + 1 < args_.size()) {
            out.value = args_[at_ + 1];
            at_ += 2;
        } else {
            at_++;
            in_ = 0;
            return Err(Error::NotFound);
        }
        in_ = 0;
        return true;
    }

    if (last) {
        at_++;
        in_ = 0;
    }
    // Advanced first, so a caller that carries on does not loop on the letter.
    if (spec_.flags.find(c) == Str::npos)
        return Err(Error::Invalid);
    return true;
}
