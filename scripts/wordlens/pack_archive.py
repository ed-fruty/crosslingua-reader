"""The committed Word Lens pack archive: lingua-data/wordlens/word-lens-packs.tar.xz.

The packs are binary, so they are committed as one compressed archive rather than as loose files.
tar + xz because Python's standard library writes it byte-for-byte reproducibly (fixed member
order, zero timestamps and owners, a fixed xz preset), so an unchanged pack set gives an unchanged
archive and no git churn; 7z would need a third-party writer. `tar -xJf` reads it on every CI
runner and developer machine.
"""

from __future__ import annotations

import io
import lzma
import os
import tarfile

ARCHIVE_NAME = "word-lens-packs.tar.xz"
MANIFEST_NAME = "word-lens-manifest.json"
ATTRIBUTION_NAME = "ATTRIBUTION.md"


def read_archive(path: str) -> dict[str, bytes]:
    """Member name -> bytes; empty when the archive does not exist yet."""
    if not os.path.exists(path):
        return {}
    members: dict[str, bytes] = {}
    with tarfile.open(path, "r:xz") as tar:
        for info in tar.getmembers():
            if not info.isfile() or "/" in info.name or info.name.startswith("."):
                raise ValueError(f"unexpected archive member {info.name!r}")
            f = tar.extractfile(info)
            members[info.name] = f.read() if f else b""
    return members


def archive_bytes(members: dict[str, bytes]) -> bytes:
    """Deterministic tar.xz of `members` (sorted, mtime 0, root owner, mode 0644)."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(members):
            data = members[name]
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 0
            info.mode = 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    return lzma.compress(raw.getvalue(), format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64,
                         preset=9 | lzma.PRESET_EXTREME)


def write_archive(path: str, members: dict[str, bytes]) -> bool:
    """Write the archive if its bytes change; returns whether it did."""
    data = archive_bytes(members)
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return True
