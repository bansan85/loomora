"""Verify a macOS recovery image against its chunklist, with the code of OSX-KVM's fetch script (ADR 0019).

usage: python3 -I tools/macvm-verify-dmg.py <fetch-macOS-v2.py> <BaseSystem.chunklist> <BaseSystem.dmg>

Checks Apple's RSA signature of the chunklist and the SHA-256 of every chunk. Standard library only.
Does not download anything and does not store the image.
"""
import hashlib
import importlib.util
import sys

script, cnk, dmg = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("fetchmac", script)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)  # definitions only; main() is behind __main__
chunks = list(mod.verify_chunklist(cnk))  # asserts the structure and checks Apple's RSA signature
total = 0
with open(dmg, "rb") as f:
    for i, (size, sha) in enumerate(chunks):
        data = f.read(size)
        assert len(data) == size, "short read at chunk %d" % i
        assert hashlib.sha256(data).digest() == sha, "bad hash at chunk %d" % i
        total += size
    assert f.read(1) == b"", "trailing data after the last chunk"
print("OK: %d chunks, %d bytes, signature and every chunk hash verified" % (len(chunks), total))
