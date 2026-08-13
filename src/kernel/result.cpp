#include "result.h"

Str error_name(Error e)
{
    switch (e) {
    case Error::Invalid:
        return "invalid";
    case Error::NoMemory:
        return "out of memory";
    case Error::NotFound:
        return "not found";
    case Error::Exists:
        return "already exists";
    case Error::NotDir:
        return "not a directory";
    case Error::IsDir:
        return "is a directory";
    case Error::Perm:
        return "permission denied";
    case Error::Io:
        return "i/o error";
    case Error::Cancelled:
        return "cancelled";
    case Error::Again:
        return "try again";
    case Error::Unsupported:
        return "unsupported";
    case Error::Closed:
        return "closed";
    case Error::NotEmpty:
        return "directory not empty";
    }
    return "unknown error";
}
