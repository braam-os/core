#!/usr/bin/env python3
"""Ed25519, and the two encodings the formats use.

    ed25519.py <path>...

A fresh private key at each path, and its public half and key id on stdout.

The one place a key is read. Package_Management.md §9: no private key belongs
in the tree, in anything built from it, or inside rootfs.zip — so a signer
reads one from a path, keeps nothing, and writes nothing but the signature.

`cryptography` is the only outside dependency in this tree, and only a tool
that *signs* needs it. Everything checked in was signed once; verifying is the
kernel's Sys::Verify, not this.
"""

import base64
import hashlib
import os
import sys

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
except ImportError:  # pragma: no cover — reported, not worked around
    ed25519 = None


def require():
    if ed25519 is None:
        sys.exit("ed25519.py: needs the cryptography package: pip3 install cryptography")


def generate(path):
    """A fresh key pair, the private half written to `path` and nothing else."""
    require()
    k = ed25519.Ed25519PrivateKey.generate()
    raw = k.private_bytes(encoding=serialization.Encoding.Raw,
                          format=serialization.PrivateFormat.Raw,
                          encryption_algorithm=serialization.NoEncryption())
    with open(path, "wb") as f:
        f.write(raw)
    return public_of(raw)


def load(path):
    """The private key at `path`: thirty-two raw bytes."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) != 32:
        sys.exit(f"ed25519.py: {path} is not a 32-byte raw private key")
    return raw


def public_of(private):
    require()
    k = ed25519.Ed25519PrivateKey.from_private_bytes(private)
    return k.public_key().public_bytes(encoding=serialization.Encoding.Raw,
                                       format=serialization.PublicFormat.Raw)


def sign(private, message):
    require()
    return ed25519.Ed25519PrivateKey.from_private_bytes(private).sign(message)


def b64(raw):
    return base64.b64encode(raw).decode()


def digest(raw):
    """Package_Formats.md §1.1's Q2: SHA-256, base64, one algorithm character."""
    return "Q2" + b64(hashlib.sha256(raw).digest())


def key_name(public):
    """trust_key_name: the digest of "<algorithm> <base64 key>"."""
    return digest(("ed25519 " + b64(public)).encode())


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: ed25519.py <path>...")
    for path in argv[1:]:
        if os.path.exists(path):
            sys.exit(f"ed25519.py: {path} exists; a key written over is a key lost")
    for path in argv[1:]:
        public = generate(path)
        print(f"{path} ed25519 {b64(public)} {key_name(public)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
